// Offline replay of a raw gt_imu_probe CSV through the real PoseEstimator.
// Used to evaluate estimator/heading-lock changes against recorded hardware
// data (including injected extra gyro bias to emulate a stale estimate).
//
//   imu_replay FILE.csv [--orientation config/orientation.json]
//              [--fit START END] [--fit-bias x,y,z] [--hard-iron x,y,z]
//              [--no-mag] [--observe] [--stats] [--bias-error x,y,z] [--from S]
//              [--print-every S] [--trace OUT.csv [--trace-hz HZ]]
//              [--startup-audit STEP [--audit-horizon S] [--audit-out OUT.csv]]
//              [--confirm S] [--confirm-agree DEG_S] [--cal-window S]
//              [--episode S] [--episode-gates TILT,PREDICT_DEG,RATIO]
//
// --trace writes a decimated row per 1/HZ s (bias, gates, pose, and the
// calibrated magnetometer heading in the estimate's own earth frame, whose
// change is the gyro-only heading drift). --startup-audit restarts a fresh
// estimator every STEP s of the recording and reports time-to-tracking and
// the first-minute gyro-only drift against the magnetometer (needs a mag
// calibration in the orientation file). --confirm / --confirm-agree /
// --cal-window override the startup confirmation period, its agreement
// tolerance and the startup window length. --episode / --episode-gates
// override the gravity-verified single-episode confirmation (minimum span,
// 0 disables; horizontal disagreement, predicted gravity swing, observed
// ratio).
//
//              [--hard-iron-track] [--baseline-no-track] [--eval-until S] [--tracker-log]
//              [--inject-hard-iron x,y,z --inject-from S [--inject-until S]]
//
// --inject-hard-iron adds a head-fixed field (e.g. an earbud's magnets) to the
// raw magnetometer from --inject-from on, and runs an untouched baseline
// estimator alongside: the summary reports how far the injected run's view
// yaw departs from the baseline (the view motion the change would cause).

#define _CRT_SECURE_NO_WARNINGS
#include "imu/mag_heading.h"
#include "imu/orientation_calibration.h"
#include "imu/pose_estimator.h"
#include "imu/pose_smoother.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace gt;

namespace {

bool parse_vec(const char* text, Vec3& out) {
    return std::sscanf(text, "%f,%f,%f", &out.x, &out.y, &out.z) == 3;
}

float angle_between_deg(const Quat& a, const Quat& b) {
    const Quat d = quat_multiply(quat_conjugate(a), b);
    const float v = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return 2.0f * std::atan2(v, std::fabs(d.w)) * 57.29578f;
}

// Display-side evaluation at the renderer's cadence. "Reading" frames are the
// ones where the raw head speed (0.25 s EMA) is below 3 deg/s; on those the
// displayed frame-to-frame rotation is what makes text swim. "Turn" frames
// (above 20 deg/s) measure the lag the smoother adds to deliberate motion.
struct SmoothEval {
    PoseSmoother smoother;
    bool enabled = false;
    bool have_prev = false;
    Quat prev_raw;
    Quat prev_shown;
    float speed_ema = 0.0f;
    double read_frames = 0, read_raw_motion = 0, read_shown_motion = 0, read_offset = 0;
    double turn_frames = 0, turn_lag_sq = 0;
    float max_offset = 0.0f;
    std::vector<float> read_steps;
    // Motion-to-photon view: each shown frame is compared with the raw pose
    // `latency_s` later (when the frame would reach the eye), by speed class.
    float latency_s = 0.025f;
    struct Pending {
        double t;
        Quat shown;
        int cls;  // 0 reading (< 3 deg/s), 1 transition, 2 turning (> 20 deg/s)
    };
    std::deque<Pending> pending;
    double photon_sq[3] = {0, 0, 0};
    double photon_n[3] = {0, 0, 0};
    float photon_max = 0.0f;

    void sample(double t, const Quat& raw) {
        while (!pending.empty() && pending.front().t + latency_s <= t) {
            const Pending& p = pending.front();
            const float off = angle_between_deg(p.shown, raw);
            photon_sq[p.cls] += off * off;
            photon_n[p.cls] += 1;
            photon_max = std::max(photon_max, off);
            pending.pop_front();
        }
    }

    void frame(const Quat& raw, float dt, double t) {
        const Quat shown = smoother.update(raw, dt);
        if (have_prev) {
            pending.push_back(Pending{t, shown, speed_ema < 3.0f ? 0 : (speed_ema > 20.0f ? 2 : 1)});
        }
        if (have_prev) {
            const float raw_step = angle_between_deg(prev_raw, raw);
            const float shown_step = angle_between_deg(prev_shown, shown);
            const float k = dt / (0.25f + dt);
            speed_ema += (raw_step / dt - speed_ema) * k;
            const float offset = angle_between_deg(shown, raw);
            max_offset = std::max(max_offset, offset);
            if (speed_ema < 3.0f) {
                read_frames += 1;
                read_raw_motion += raw_step;
                read_shown_motion += shown_step;
                read_offset += offset;
                read_steps.push_back(shown_step);
            } else if (speed_ema > 20.0f) {
                turn_frames += 1;
                turn_lag_sq += offset * offset;
            }
        }
        prev_raw = raw;
        prev_shown = shown;
        have_prev = true;
    }

    void report(float frame_dt) {
        const double seconds = read_frames * frame_dt;
        std::sort(read_steps.begin(), read_steps.end());
        const float p95 = read_steps.empty() ? 0.0f : read_steps[read_steps.size() * 95 / 100];
        std::printf("smooth-eval: reading %.0f s | raw motion %.3f deg/s | shown motion %.3f deg/s "
                    "(%.1f px/s) | shown p95 step %.4f deg | mean world offset %.3f deg\n",
                    seconds, read_raw_motion / std::max(seconds, 1e-9),
                    read_shown_motion / std::max(seconds, 1e-9),
                    read_shown_motion / std::max(seconds, 1e-9) / 0.024, p95,
                    read_offset / std::max(read_frames, 1.0));
        std::printf("smooth-eval: turning %.0f s | rms lag %.3f deg | max offset %.3f deg\n",
                    turn_frames * frame_dt, std::sqrt(turn_lag_sq / std::max(turn_frames, 1.0)),
                    max_offset);
        auto rms = [&](int c) { return std::sqrt(photon_sq[c] / std::max(photon_n[c], 1.0)); };
        std::printf("smooth-eval: photon error at +%.0f ms | reading %.4f | transition %.3f | turning %.3f "
                    "deg rms | max %.3f deg\n",
                    latency_s * 1000.0f, rms(0), rms(1), rms(2), photon_max);
    }
};

bool load_csv(const std::string& path, std::vector<ImuSample>& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        unsigned long long host = 0;
        unsigned tick = 0;
        ImuSample s;
        const int n = std::sscanf(line.c_str(), "%llu,%u,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &host, &tick,
                                  &s.accel_mps2.x, &s.accel_mps2.y, &s.accel_mps2.z, &s.gyro_degs.x,
                                  &s.gyro_degs.y, &s.gyro_degs.z, &s.mag_ut.x, &s.mag_ut.y, &s.mag_ut.z,
                                  &s.temp_c);
        if (n != 12) {
            continue;
        }
        s.host_time_us = host;
        s.tick_100us = tick;
        out.push_back(s);
    }
    return !out.empty();
}

Vec3 apply_mat(const std::array<float, 9>& m, const Vec3& v) {
    return Vec3{m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

Vec3 rotate_vec(const Quat& q, const Vec3& v) {
    const Quat r = quat_multiply(quat_multiply(q, Quat{0.0f, v.x, v.y, v.z}), quat_conjugate(q));
    return Vec3{r.x, r.y, r.z};
}

float percentile(std::vector<float> v, float p) {
    if (v.empty()) {
        return 0.0f;
    }
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<float>(v.size())))];
}

// --startup-audit STEP: a fresh estimator is started every STEP seconds of the
// recording, as if the wearer launched the app at that moment, and runs until
// startup calibration completes and then for the audit horizon. For every
// start it reports how long tracking took to open and the gyro-only heading
// drift over the horizon, measured against the magnetometer in the estimate's
// own earth frame (the heading lock is forced to observe-only). A startup bias
// that averaged a head movement shows up as tens of degrees of drift.
int run_startup_audit(const std::vector<ImuSample>& samples, PoseEstimator::Config cfg,
                      const MagCalibration& cal, float step_s, float horizon_s,
                      const std::string& out_path) {
    if (!cal.valid) {
        std::printf("startup audit needs a magnetometer calibration (hard iron)\n");
        return 1;
    }
    cfg.mag_lock.observe_only = true;
    const uint32_t tick0 = samples.front().tick_100us;
    auto t_of = [&](const ImuSample& s) { return static_cast<double>(s.tick_100us - tick0) * 1e-4; };
    std::vector<float> norms;
    for (size_t i = 0; i < samples.size(); i += 97) {
        const Vec3 f = mag_to_package(samples[i].mag_ut, cal);
        norms.push_back(std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z));
    }
    const float field_ref = percentile(norms, 0.5f);
    std::FILE* out = out_path.empty() ? nullptr : std::fopen(out_path.c_str(), "w");
    if (out) {
        std::fprintf(out, "start_s,done,ready_s,bias_x,bias_y,bias_z,temp_c,drift_deg,reseeds,"
                          "reseed_s,final_x,final_y,final_z,episode_reseeds,reverts\n");
    }
    std::vector<float> ready_times;
    std::vector<float> drifts;
    int starts = 0;
    int never = 0;
    unsigned reseed_total = 0;
    unsigned episode_total = 0;
    unsigned revert_total = 0;
    const double end_t = t_of(samples.back());
    size_t i0 = 0;
    for (double start = 0.0; start + 60.0 + horizon_s < end_t; start += step_s) {
        while (i0 < samples.size() && t_of(samples[i0]) < start) {
            ++i0;
        }
        PoseEstimator est(cfg);
        ++starts;
        bool ready_seen = false;
        double t_ready = 0.0;
        Vec3 bias{};
        float temp = 0.0f;
        double reseed_s = -1.0;
        Vec3 final_bias{};
        double head_x0 = 0.0, head_y0 = 0.0, head_x1 = 0.0, head_y1 = 0.0;
        int n0 = 0, n1 = 0;
        for (size_t k = i0; k < samples.size(); ++k) {
            const ImuSample& s = samples[k];
            const double t = t_of(s) - start;
            if (!ready_seen && t > 60.0) {
                break;
            }
            if (!est.add_sample(s)) {
                continue;
            }
            if (!ready_seen) {
                ready_seen = true;
                t_ready = t;
                bias = est.gyro_bias_degs();
                temp = s.temp_c;
            }
            const double since = t - t_ready;
            if (reseed_s < 0.0 && est.startup_reseeds() > 0) {
                reseed_s = since;
            }
            final_bias = est.gyro_bias_degs();
            if (since > horizon_s) {
                break;
            }
            const bool head_window = since < 1.0;
            const bool tail_window = since >= horizon_s - 1.0;
            if (!head_window && !tail_window) {
                continue;
            }
            const Vec3 f = rotate_vec(est.fused_orientation(),
                                      apply_mat(cfg.sensor_to_head, mag_to_package(s.mag_ut, cal)));
            const float norm = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
            if (std::fabs(norm - field_ref) > 0.08f * field_ref) {
                continue;
            }
            if (head_window) {
                head_x0 += f.x;
                head_y0 += f.y;
                ++n0;
            } else {
                head_x1 += f.x;
                head_y1 += f.y;
                ++n1;
            }
        }
        float drift = NAN;
        if (ready_seen && n0 > 50 && n1 > 50) {
            const double a0 = std::atan2(head_y0, head_x0);
            const double a1 = std::atan2(head_y1, head_x1);
            double d = (a1 - a0) * 57.29578;
            while (d > 180.0) d -= 360.0;
            while (d < -180.0) d += 360.0;
            drift = static_cast<float>(d);
            drifts.push_back(std::fabs(drift));
        }
        if (ready_seen) {
            ready_times.push_back(static_cast<float>(t_ready));
        } else {
            ++never;
        }
        reseed_total += est.startup_reseeds();
        episode_total += est.startup_episode_reseeds();
        revert_total += est.startup_reverts();
        if (out) {
            std::fprintf(out, "%.1f,%d,%.3f,%.4f,%.4f,%.4f,%.2f,%.3f,%u,%.2f,%.4f,%.4f,%.4f,%u,%u\n", start,
                         ready_seen ? 1 : 0, ready_seen ? t_ready : -1.0, bias.x, bias.y, bias.z,
                         temp, drift, est.startup_reseeds(), reseed_s, final_bias.x, final_bias.y,
                         final_bias.z, est.startup_episode_reseeds(), est.startup_reverts());
        }
    }
    if (out) {
        std::fclose(out);
    }
    int over5 = 0;
    int over10 = 0;
    double sum = 0.0;
    for (float d : drifts) {
        over5 += d > 5.0f ? 1 : 0;
        over10 += d > 10.0f ? 1 : 0;
        sum += d;
    }
    std::printf("startup-audit: %d starts, %d never calibrated within 60 s, %u startup reseeds "
                "(%u single-episode), %u reverts\n",
                starts, never, reseed_total, episode_total, revert_total);
    std::printf("startup-audit: ready after p50 %.2f s, p90 %.2f s, p99 %.2f s, max %.2f s\n",
                percentile(ready_times, 0.5f), percentile(ready_times, 0.9f), percentile(ready_times, 0.99f),
                percentile(ready_times, 1.0f));
    std::printf("startup-audit: |drift| over %.0f s (n=%zu): mean %.2f, p50 %.2f, p90 %.2f, p99 %.2f, max %.2f deg; "
                ">5 deg %d, >10 deg %d\n",
                horizon_s, drifts.size(), drifts.empty() ? 0.0 : sum / static_cast<double>(drifts.size()),
                percentile(drifts, 0.5f), percentile(drifts, 0.9f), percentile(drifts, 0.99f),
                percentile(drifts, 1.0f), over5, over10);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: imu_replay FILE.csv [options]\n");
        return 2;
    }
    std::string csv = argv[1];
    std::string orientation_path;
    float fit_start = -1.0f;
    float fit_end = -1.0f;
    Vec3 fit_bias{};
    Vec3 hard_iron{};
    bool have_hard_iron = false;
    bool use_mag = true;
    bool observe = false;
    bool stats = false;
    SmoothEval eval;
    float lock_tau = 0.0f;
    float lock_ki = -1.0f;
    PoseSmoother::Config smooth_cfg;
    Vec3 bias_error{};
    float from_s = 0.0f;
    float print_every = 10.0f;
    float bias_error_from = 0.0f;
    std::string trace_path;
    float trace_hz = 20.0f;
    float audit_step = 0.0f;
    float audit_horizon = 60.0f;
    std::string audit_out;
    float cal_window = -1.0f;
    float episode_min_s = -1.0f;
    Vec3 episode_gates{};  // tilt deg/s, predicted-min deg, observed ratio
    bool have_episode_gates = false;
    float confirm_s = -1.0f;
    float confirm_agree = -1.0f;
    bool track_hard_iron = false;
    bool tracker_log = false;
    bool baseline_no_track = false;
    float eval_until = 1e30f;
    Vec3 inject{};
    bool have_inject = false;
    float inject_from = 0.0f;
    float inject_until = 1e30f;
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--startup-audit") && i + 1 < argc) {
            audit_step = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--audit-horizon") && i + 1 < argc) {
            audit_horizon = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--audit-out") && i + 1 < argc) {
            audit_out = argv[++i];
        } else if (!std::strcmp(a, "--confirm") && i + 1 < argc) {
            confirm_s = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--confirm-agree") && i + 1 < argc) {
            confirm_agree = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--cal-window") && i + 1 < argc) {
            cal_window = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--episode") && i + 1 < argc) {
            episode_min_s = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--episode-gates") && i + 1 < argc) {
            have_episode_gates = parse_vec(argv[++i], episode_gates);
        } else if (!std::strcmp(a, "--trace") && i + 1 < argc) {
            trace_path = argv[++i];
        } else if (!std::strcmp(a, "--trace-hz") && i + 1 < argc) {
            trace_hz = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--orientation") && i + 1 < argc) {
            orientation_path = argv[++i];
        } else if (!std::strcmp(a, "--fit") && i + 2 < argc) {
            fit_start = static_cast<float>(std::atof(argv[++i]));
            fit_end = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--fit-bias") && i + 1 < argc) {
            parse_vec(argv[++i], fit_bias);
        } else if (!std::strcmp(a, "--hard-iron") && i + 1 < argc) {
            have_hard_iron = parse_vec(argv[++i], hard_iron);
        } else if (!std::strcmp(a, "--observe")) {
            observe = true;
        } else if (!std::strcmp(a, "--stats")) {
            stats = true;
        } else if (!std::strcmp(a, "--smooth-eval")) {
            eval.enabled = true;
        } else if (!std::strcmp(a, "--min-cutoff") && i + 1 < argc) {
            smooth_cfg.min_cutoff_hz = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--predict") && i + 1 < argc) {
            smooth_cfg.predict_s = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--predict-gate") && i + 1 < argc) {
            Vec3 g{};
            if (parse_vec(argv[++i], g)) {
                smooth_cfg.predict_gate_lo_degs = g.x;
                smooth_cfg.predict_gate_hi_degs = g.y;
                smooth_cfg.predict_cutoff_hz = g.z;
            }
        } else if (!std::strcmp(a, "--latency") && i + 1 < argc) {
            eval.latency_s = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--beta") && i + 1 < argc) {
            smooth_cfg.beta = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--lock-tau") && i + 1 < argc) {
            lock_tau = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--lock-ki") && i + 1 < argc) {
            lock_ki = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--stabilise") && i + 1 < argc) {
            apply_reading_hold(std::atoi(argv[++i]), smooth_cfg);
        } else if (!std::strcmp(a, "--hold") && i + 1 < argc) {
            Vec3 h{};
            if (parse_vec(argv[++i], h)) {
                smooth_cfg.hold_inner_deg = h.x;
                smooth_cfg.hold_outer_deg = h.y;
                smooth_cfg.hold_settle_tau_s = h.z;
            }
        } else if (!std::strcmp(a, "--hard-iron-track")) {
            track_hard_iron = true;
        } else if (!std::strcmp(a, "--no-hard-iron-track")) {
            track_hard_iron = false;
        } else if (!std::strcmp(a, "--tracker-log")) {
            tracker_log = true;
        } else if (!std::strcmp(a, "--baseline-no-track")) {
            baseline_no_track = true;
        } else if (!std::strcmp(a, "--eval-until") && i + 1 < argc) {
            eval_until = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--inject-hard-iron") && i + 1 < argc) {
            have_inject = parse_vec(argv[++i], inject);
        } else if (!std::strcmp(a, "--inject-from") && i + 1 < argc) {
            inject_from = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--inject-until") && i + 1 < argc) {
            inject_until = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--no-mag")) {
            use_mag = false;
        } else if (!std::strcmp(a, "--bias-error") && i + 1 < argc) {
            parse_vec(argv[++i], bias_error);
        } else if (!std::strcmp(a, "--bias-error-from") && i + 1 < argc) {
            bias_error_from = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--from") && i + 1 < argc) {
            from_s = static_cast<float>(std::atof(argv[++i]));
        } else if (!std::strcmp(a, "--print-every") && i + 1 < argc) {
            print_every = static_cast<float>(std::atof(argv[++i]));
        } else {
            std::printf("unknown option %s\n", a);
            return 2;
        }
    }

    std::vector<ImuSample> samples;
    if (!load_csv(csv, samples)) {
        std::printf("failed to read %s\n", csv.c_str());
        return 1;
    }
    const uint32_t tick0 = samples.front().tick_100us;
    auto t_of = [&](const ImuSample& s) { return static_cast<float>(s.tick_100us - tick0) * 1e-4f; };
    std::printf("loaded %zu samples, %.1f s\n", samples.size(), t_of(samples.back()));

    PoseEstimator::Config cfg;
    if (!orientation_path.empty()) {
        std::string error;
        if (!load_orientation_calibration(orientation_path, cfg.sensor_to_head, error)) {
            std::printf("orientation: %s\n", error.c_str());
            return 1;
        }
        MagCalibration file_cal;
        if (load_mag_calibration(orientation_path, file_cal, error) && file_cal.valid && !have_hard_iron) {
            hard_iron = file_cal.hard_iron_ut;
            have_hard_iron = true;
            std::printf("hard iron from file: (%.2f, %.2f, %.2f)\n", hard_iron.x, hard_iron.y, hard_iron.z);
        }
    }

    if (fit_start >= 0.0f) {
        std::vector<ImuSample> window;
        for (const auto& s : samples) {
            const float t = t_of(s);
            if (t >= fit_start && t < fit_end) {
                window.push_back(s);
            }
        }
        const MagFitResult fit = fit_mag_hard_iron(window, fit_bias);
        std::printf("fit [%.0f,%.0f]: %s hard_iron=(%.2f, %.2f, %.2f) |B|=%.2f resid=%.3f rotation=%.0f tilt=%.0f deg\n",
                    fit_start, fit_end, fit.code, fit.hard_iron_ut.x, fit.hard_iron_ut.y, fit.hard_iron_ut.z,
                    fit.field_ut, fit.residual_rms_ut, fit.rotation_coverage_deg, fit.tilt_coverage_deg);
        if (fit.ok) {
            hard_iron = fit.hard_iron_ut;
            have_hard_iron = true;
        }
    }
    if (use_mag && have_hard_iron) {
        cfg.mag_calibration.valid = true;
        cfg.mag_calibration.hard_iron_ut = hard_iron;
    }

    cfg.mag_lock.observe_only = observe;
    if (lock_tau > 0.0f) {
        cfg.mag_lock.tau_s = lock_tau;
    }
    if (lock_ki >= 0.0f) {
        cfg.mag_lock.integral_gain_scale = lock_ki;
    }
    cfg.mag_lock.hard_iron.enabled = track_hard_iron;
    if (confirm_s >= 0.0f) {
        cfg.startup_confirm_s = confirm_s;
    }
    if (confirm_agree > 0.0f) {
        cfg.startup_confirm_agree_degs = confirm_agree;
    }
    if (episode_min_s >= 0.0f) {
        cfg.startup_episode_min_s = episode_min_s;
    }
    if (have_episode_gates) {
        cfg.startup_episode_tilt_degs = episode_gates.x;
        cfg.startup_episode_predict_min_deg = episode_gates.y;
        cfg.startup_episode_observed_ratio = episode_gates.z;
    }
    if (cal_window > 0.0f) {
        cfg.calibration_window_s = cal_window;
        cfg.bias_samples = static_cast<int>(cal_window * 476.0f);
    }
    if (audit_step > 0.0f) {
        MagCalibration audit_cal;
        audit_cal.valid = have_hard_iron;
        audit_cal.hard_iron_ut = hard_iron;
        return run_startup_audit(samples, cfg, audit_cal, audit_step, audit_horizon, audit_out);
    }
    PoseEstimator est(cfg);
    PoseEstimator::Config baseline_cfg = cfg;
    // The reference run: the same estimator on the untouched stream (with
    // --baseline-no-track: the stored calibration without tracking).
    baseline_cfg.mag_lock.hard_iron.enabled = track_hard_iron && !baseline_no_track;
    PoseEstimator baseline(baseline_cfg);
    uint32_t last_evaluated = 0;
    double diff_sq = 0.0;
    double diff_n = 0.0;
    float diff_max = 0.0f;
    std::vector<float> diffs;
    float hold_s = 0.0f;
    float last_t = 0.0f;
    eval.smoother.configure(smooth_cfg);
    constexpr float kFrameDt = 1.0f / 60.0f;
    float next_frame = -1.0f;
    float next_print = from_s;
    // Per-print-interval gate statistics: which rest/adaptation gate was
    // open, so a stalled bias estimate can be attributed to a specific gate.
    long n_int = 0, n_rest = 0, n_still = 0, n_routine = 0, n_escape = 0;
    double sum_dev = 0.0;
    float min_yaw = 1e9f;
    float max_yaw = -1e9f;
    // --trace: a decimated per-row record for offline drift attribution. The
    // magnetometer heading is placed in the estimate's own earth frame, so its
    // change over time is the gyro-only heading drift (plus field distortion),
    // with no lock state, reacquisition or reference reset in between.
    std::FILE* trace = nullptr;
    if (!trace_path.empty()) {
        trace = std::fopen(trace_path.c_str(), "w");
        if (!trace) {
            std::printf("cannot write %s\n", trace_path.c_str());
            return 1;
        }
        std::fprintf(trace, "t,temp_c,still,rest,adapt,bias_x,bias_y,bias_z,yaw,pitch,roll,mag_heading,"
                            "mag_h_ut,mag_v_ut,yaw_travel_pos,yaw_travel_neg,rollbacks\n");
    }
    MagCalibration trace_cal;
    trace_cal.valid = have_hard_iron;
    trace_cal.hard_iron_ut = hard_iron;
    auto head_map = [&](const Vec3& v) {
        const auto& m = cfg.sensor_to_head;
        return Vec3{m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                    m[6] * v.x + m[7] * v.y + m[8] * v.z};
    };
    auto rotate = [](const Quat& q, const Vec3& v) {
        const Quat r = quat_multiply(quat_multiply(q, Quat{0.0f, v.x, v.y, v.z}), quat_conjugate(q));
        return Vec3{r.x, r.y, r.z};
    };
    float next_trace = -1.0f;
    float prev_t = -1.0f;
    double travel_pos = 0.0;
    double travel_neg = 0.0;
    for (auto s : samples) {
        const float t = t_of(s);
        if (t < from_s) {
            continue;
        }
        if (t >= bias_error_from) {
            s.gyro_degs.x += bias_error.x;
            s.gyro_degs.y += bias_error.y;
            s.gyro_degs.z += bias_error.z;
        }
        bool ready = false;
        if (have_inject) {
            const bool base_ready = baseline.add_sample(s);
            if (t >= inject_from && t < inject_until) {
                s.mag_ut.x += inject.x;
                s.mag_ut.y += inject.y;
                s.mag_ut.z += inject.z;
            }
            const bool ready_inj = est.add_sample(s);
            if (base_ready && ready_inj && t >= inject_from && t < eval_until) {
                float d = est.euler().yaw_deg - baseline.euler().yaw_deg;
                while (d > 180.0f) {
                    d -= 360.0f;
                }
                while (d < -180.0f) {
                    d += 360.0f;
                }
                diff_sq += static_cast<double>(d) * d;
                diff_n += 1.0;
                diff_max = std::max(diff_max, std::fabs(d));
                if ((static_cast<long>(diff_n) % 48) == 0) {
                    diffs.push_back(std::fabs(d));
                }
            }
            ready = ready_inj;
        } else {
            ready = est.add_sample(s);
        }
        if (!ready) {
            prev_t = -1.0f;
            continue;
        }
        {
            const auto& tr = est.mag_lock().hard_iron_tracker();
            if (tr.hypothesis_pending() && t > last_t) {
                hold_s += t - last_t;
            }
            last_t = t;
            const uint32_t events = tr.steps_detected() + tr.hypotheses_adopted() + tr.hypotheses_rejected();
            if (tracker_log && events != last_evaluated) {
                last_evaluated = events;
                const Vec3 st = tr.last_step_ut();
                const Vec3 hy = tr.hypothesis_ut();
                const Vec3 off = tr.offset_from_calibration_ut();
                std::printf("tracker t=%7.1f steps=%u adopted=%u rejected=%u last_step=(%+.2f,%+.2f,%+.2f) |%.2f| "
                            "pending=%d hyp=(%+.2f,%+.2f,%+.2f) sep=%.2f scale=%.2f(last %.2f) windows=%u "
                            "off=(%+.2f,%+.2f,%+.2f) |B|=%.1f\n",
                            t, tr.steps_detected(), tr.hypotheses_adopted(), tr.hypotheses_rejected(), st.x,
                            st.y, st.z, std::sqrt(st.x * st.x + st.y * st.y + st.z * st.z),
                            tr.hypothesis_pending() ? 1 : 0, hy.x, hy.y, hy.z, tr.evidence_separation_ut2(),
                            tr.hypothesis_scale(), tr.last_scale(), tr.windows_evaluated(), off.x, off.y, off.z,
                            est.mag_lock().field_ut());
            }
        }
        if (trace) {
            const Vec3 b = est.gyro_bias_degs();
            const Quat q = est.fused_orientation();
            const Vec3 w = rotate(q, head_map(Vec3{s.gyro_degs.x - b.x, s.gyro_degs.y - b.y,
                                                   s.gyro_degs.z - b.z}));
            const float step_dt = prev_t >= 0.0f ? t - prev_t : 0.0f;
            if (w.z > 0.0f) {
                travel_pos += w.z * step_dt;
            } else {
                travel_neg -= w.z * step_dt;
            }
            prev_t = t;
            if (t >= next_trace) {
                const Vec3 f = rotate(q, head_map(mag_to_package(s.mag_ut, trace_cal)));
                const Euler te = est.euler();
                std::fprintf(trace, "%.3f,%.2f,%d,%d,%d,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.4f,%.4f,%u\n",
                             t, s.temp_c, est.still() ? 1 : 0, est.rest() ? 1 : 0, est.adapt_state(), b.x, b.y,
                             b.z, te.yaw_deg, te.pitch_deg, te.roll_deg,
                             std::atan2(f.y, f.x) * 57.29578f, std::sqrt(f.x * f.x + f.y * f.y), f.z,
                             travel_pos, travel_neg, est.escape_rollbacks());
                next_trace = t + 1.0f / std::max(trace_hz, 0.1f);
            }
        }
        const Euler e = est.euler();
        if (eval.enabled) {
            eval.sample(t, est.quat());
            if (t >= next_frame) {
                eval.frame(est.quat(), kFrameDt, t);
                next_frame = (next_frame < 0.0f ? t : next_frame) + kFrameDt;
            }
        }
        ++n_int;
        n_rest += est.rest() ? 1 : 0;
        n_still += est.still() ? 1 : 0;
        n_routine += est.adapt_state() == 1 ? 1 : 0;
        n_escape += est.adapt_state() == 2 ? 1 : 0;
        sum_dev += est.stillness_degs();
        if (t >= next_print) {
            if (have_inject) {
                const auto& tr = est.mag_lock().hard_iron_tracker();
                std::printf("vs-baseline t=%7.1f dyaw=%+7.2f state=%s/%s pending=%d armed=%d J=%.2f "
                            "foreign=%u transients=%u\n",
                            t, e.yaw_deg - baseline.euler().yaw_deg, mag_state_name(est.mag_lock().state()),
                            mag_state_name(baseline.mag_lock().state()), tr.hypothesis_pending() ? 1 : 0,
                            tr.step_pending() ? 1 : 0, tr.step_signal_ut(), tr.foreign_steps(),
                            tr.transients_rejected());
            }
            const auto& lock = est.mag_lock();
            const Vec3 b = est.gyro_bias_degs();
            std::printf("bias=(%+.3f,%+.3f,%+.3f) adapt=%d still=%d rb=%u ", b.x, b.y, b.z, est.adapt_state(),
                        est.still() ? 1 : 0, est.escape_rollbacks());
            std::printf("t=%6.1f yaw=%8.2f pitch=%7.2f roll=%7.2f | mag %s state=%d err=%6.2f int=%6.3f "
                        "|B|=%5.1f dip=%5.1f corr_total=%7.2f reacq=%u\n",
                        t, e.yaw_deg, e.pitch_deg, e.roll_deg, est.mag_lock_active() ? "on " : "off",
                        static_cast<int>(lock.state()), lock.error_deg(), lock.integral_degs(), lock.field_ut(),
                        lock.dip_deg(), lock.total_correction_deg(), lock.reacquisitions());
            if (stats) {
                const double n = n_int > 0 ? static_cast<double>(n_int) : 1.0;
                std::printf("   gates: rest=%3.0f%% still=%3.0f%% routine=%3.0f%% escape=%3.0f%% dev=%.2f\n",
                            100.0 * n_rest / n, 100.0 * n_still / n, 100.0 * n_routine / n,
                            100.0 * n_escape / n, sum_dev / n);
            }
            n_int = n_rest = n_still = n_routine = n_escape = 0;
            sum_dev = 0.0;
            next_print = t + print_every;
        }
        min_yaw = std::min(min_yaw, e.yaw_deg);
        max_yaw = std::max(max_yaw, e.yaw_deg);
    }
    if (eval.enabled) {
        eval.report(kFrameDt);
    }
    {
        const auto& tr = est.mag_lock().hard_iron_tracker();
        const Vec3 hi = tr.hard_iron_ut();
        std::printf("hard-iron tracker: %s steps=%u foreign=%u transients=%u adopted=%u rejected=%u lock_steps=%u "
                    "pending=%.1f s final=(%.2f, %.2f, %.2f)\n",
                    track_hard_iron ? "on" : "off", tr.steps_detected(), tr.foreign_steps(), tr.transients_rejected(),
                    tr.hypotheses_adopted(), tr.hypotheses_rejected(), est.mag_lock().hard_iron_steps(), hold_s, hi.x,
                    hi.y, hi.z);
    }
    if (have_inject && diff_n > 0.0) {
        std::sort(diffs.begin(), diffs.end());
        const float p95 = diffs.empty() ? 0.0f : diffs[diffs.size() * 95 / 100];
        std::printf("hard-iron-eval: injected (%.2f, %.2f, %.2f) uT from %.0f s: view yaw vs baseline "
                    "max %.3f deg, rms %.3f deg, p95 %.3f deg\n",
                    inject.x, inject.y, inject.z, inject_from, diff_max, std::sqrt(diff_sq / diff_n), p95);
    }
    const Euler e = est.euler();
    std::printf("final yaw=%.3f pitch=%.3f roll=%.3f\n", e.yaw_deg, e.pitch_deg, e.roll_deg);
    if (trace) {
        std::fclose(trace);
    }
    return 0;
}
