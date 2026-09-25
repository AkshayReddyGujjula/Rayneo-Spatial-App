// Offline replay of a raw gt_imu_probe CSV through the real PoseEstimator.
// Used to evaluate estimator/heading-lock changes against recorded hardware
// data (including injected extra gyro bias to emulate a stale estimate).
//
//   imu_replay FILE.csv [--orientation config/orientation.json]
//              [--fit START END] [--fit-bias x,y,z] [--hard-iron x,y,z]
//              [--no-mag] [--observe] [--stats] [--bias-error x,y,z] [--from S]
//              [--print-every S]

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

    void frame(const Quat& raw, float dt) {
        const Quat shown = smoother.update(raw, dt);
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
    for (int i = 2; i < argc; ++i) {
        const char* a = argv[i];
        if (!std::strcmp(a, "--orientation") && i + 1 < argc) {
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
    PoseEstimator est(cfg);
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
        const bool ready = est.add_sample(s);
        if (!ready) {
            continue;
        }
        const Euler e = est.euler();
        if (eval.enabled && t >= next_frame) {
            eval.frame(est.quat(), kFrameDt);
            next_frame = (next_frame < 0.0f ? t : next_frame) + kFrameDt;
        }
        ++n_int;
        n_rest += est.rest() ? 1 : 0;
        n_still += est.still() ? 1 : 0;
        n_routine += est.adapt_state() == 1 ? 1 : 0;
        n_escape += est.adapt_state() == 2 ? 1 : 0;
        sum_dev += est.stillness_degs();
        if (t >= next_print) {
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
    const Euler e = est.euler();
    std::printf("final yaw=%.3f pitch=%.3f roll=%.3f\n", e.yaw_deg, e.pitch_deg, e.roll_deg);
    return 0;
}
