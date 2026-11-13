#include "imu/pose_estimator.h"

#include <algorithm>
#include <cmath>

namespace gt {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadPerDeg = kPi / 180.0f;

Quat accel_align_quat(const Vec3& accel) {
    const float n = std::sqrt(accel.x * accel.x + accel.y * accel.y + accel.z * accel.z);
    if (n < 1e-3f) {
        return Quat{};
    }
    const Vec3 a{accel.x / n, accel.y / n, accel.z / n};
    if (a.z > 0.99999f) {
        return Quat{};
    }
    if (a.z < -0.99999f) {
        return Quat{0.0f, 1.0f, 0.0f, 0.0f};
    }
    const float axis_norm = std::sqrt(a.y * a.y + a.x * a.x);
    const float theta = std::acos(a.z);
    const float s = std::sin(theta * 0.5f);
    return Quat{std::cos(theta * 0.5f), (a.y / axis_norm) * s, (-a.x / axis_norm) * s, 0.0f};
}

float vec_norm(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3 vec_sub(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 clamp_bias(const Vec3& v, float limit) {
    return Vec3{std::max(-limit, std::min(limit, v.x)), std::max(-limit, std::min(limit, v.y)),
                std::max(-limit, std::min(limit, v.z))};
}

}  // namespace

void PoseEstimator::configure(const Config& cfg) {
    cfg_ = cfg;
    filter_.reset();
    filter_.set_beta(cfg.beta);
    mag_lock_active_ = cfg.mag_heading_lock && cfg.mag_calibration.valid;
    mag_lock_.configure(cfg.mag_lock);
    have_last_mag_ = false;

    warmup_samples_ = 0;
    calibration_phase_s_ = 0.0f;
    reset_calibration_window();
    confirm_sums_ = {};
    confirm_windows_ = {};
    confirm_episodes_ = {};
    confirm_count_ = 0;
    confirm_active_ = false;
    confirm_time_s_ = 0.0f;
    startup_reseeds_ = 0;
    startup_reverts_ = 0;
    startup_episode_reseeds_ = 0;
    single_active_ = false;
    single_episode_ = 0;
    single_sum_ = Vec3{};
    single_windows_ = 0;
    single_first_gravity_ = Vec3{};
    single_first_s_ = 0.0f;
    single_rotation_ = Quat{};
    confirm_reseeded_ = false;
    confirm_pair_last_ = -1;
    confirm_snapshot_ = Vec3{};
    confirm_consensus_ = Vec3{};
    motion_events_ = 0;
    bias_done_ = false;
    calibrated_ = false;

    bias_degs_ = Vec3{};
    time_s_ = 0.0f;
    have_tick_ = false;
    last_tick_ = 0;

    gyro_lpf_ = Vec3{};
    gyro_dev_ema_ = Vec3{};
    accel_lpf_ = Vec3{};
    accel_dev_ema_ = Vec3{};
    rest_lpf_ready_ = false;
    rest_now_ = false;
    still_ = false;
    still_time_ = 0.0f;
    stillness_degs_ = 0.0f;
    accel_dev_mps2_ = 0.0f;
    holdoff_s_ = 0.0f;

    adapt_state_ = BiasAdaptState::calibrating;
    escape_dwell_s_ = 0.0f;
    escape_open_ = false;
    escape_armed_ = false;
    escape_snapshot_ = Vec3{};
    escape_return_dwell_s_ = 0.0f;
    escape_rollbacks_ = 0;
    corrected_rate_degs_ = 0.0f;

    ref_heading_ = Quat{};
    ref_tilt_ = Quat{};
    drift_correction_ = Quat{};
    q_prev_live_ = Quat{};
    have_prev_live_ = false;
    have_ref_ = false;
    initialized_ = false;
    fused_ = 0;
}

Vec3 PoseEstimator::map_gyro(const Vec3& v) const {
    if (!cfg_.map_package_axes) {
        return v;
    }
    const auto& m = cfg_.sensor_to_head;
    return Vec3{m[0] * v.x + m[1] * v.y + m[2] * v.z,
                m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

Vec3 PoseEstimator::map_accel(const Vec3& v) const {
    return map_gyro(v);
}

Vec3 PoseEstimator::map_mag(const Vec3& v) const {
    return map_gyro(v);
}

// Continuous, VQF-inspired rest detection. Both the gyro and the accelerometer
// are low-passed; the deviations of the raw signals from those references are
// themselves smoothed (the gyro here is noisy enough that an unfiltered
// deviation would flicker). Rest requires BOTH deviations below the enter gates
// continuously for the dwell; a motion excursion on EITHER signal resets the
// dwell. In the hysteresis band between the gates the dwell neither advances
// nor resets, so a mild periodic tremor can still accumulate toward rest while
// a genuine excursion cannot.
void PoseEstimator::update_rest_detector(const Vec3& raw, const Vec3& accel, float dt) {
    if (!rest_lpf_ready_) {
        gyro_lpf_ = raw;
        accel_lpf_ = accel;
        gyro_dev_ema_ = Vec3{};
        accel_dev_ema_ = Vec3{};
        stillness_degs_ = 0.0f;
        accel_dev_mps2_ = 0.0f;
        still_time_ = 0.0f;
        rest_lpf_ready_ = true;
        rest_now_ = true;
        still_ = cfg_.still_hold_s <= 0.0f;
        holdoff_s_ = 0.0f;
        return;
    }

    const float gyro_alpha = dt / (cfg_.fast_ema_tau_s + dt);
    gyro_lpf_.x += (raw.x - gyro_lpf_.x) * gyro_alpha;
    gyro_lpf_.y += (raw.y - gyro_lpf_.y) * gyro_alpha;
    gyro_lpf_.z += (raw.z - gyro_lpf_.z) * gyro_alpha;
    const Vec3 gyro_dev{std::fabs(raw.x - gyro_lpf_.x), std::fabs(raw.y - gyro_lpf_.y),
                        std::fabs(raw.z - gyro_lpf_.z)};

    const float accel_alpha = dt / (cfg_.accel_lpf_tau_s + dt);
    accel_lpf_.x += (accel.x - accel_lpf_.x) * accel_alpha;
    accel_lpf_.y += (accel.y - accel_lpf_.y) * accel_alpha;
    accel_lpf_.z += (accel.z - accel_lpf_.z) * accel_alpha;
    const Vec3 accel_dev{std::fabs(accel.x - accel_lpf_.x), std::fabs(accel.y - accel_lpf_.y),
                         std::fabs(accel.z - accel_lpf_.z)};

    // Asymmetric deviation smoothing: drain fast toward an already-quiet
    // signal so rest recovers ~1 s after a pan (scenario 18), rise slow so
    // noise flicker still cannot validate rest (bug 13 margins untouched).
    const float dev_up = dt / (cfg_.dev_ema_tau_s + dt);
    const float dev_down = dt / (cfg_.dev_ema_fall_tau_s + dt);
    gyro_dev_ema_.x +=
        (gyro_dev.x - gyro_dev_ema_.x) * (gyro_dev.x < gyro_dev_ema_.x ? dev_down : dev_up);
    gyro_dev_ema_.y +=
        (gyro_dev.y - gyro_dev_ema_.y) * (gyro_dev.y < gyro_dev_ema_.y ? dev_down : dev_up);
    gyro_dev_ema_.z +=
        (gyro_dev.z - gyro_dev_ema_.z) * (gyro_dev.z < gyro_dev_ema_.z ? dev_down : dev_up);
    accel_dev_ema_.x +=
        (accel_dev.x - accel_dev_ema_.x) * (accel_dev.x < accel_dev_ema_.x ? dev_down : dev_up);
    accel_dev_ema_.y +=
        (accel_dev.y - accel_dev_ema_.y) * (accel_dev.y < accel_dev_ema_.y ? dev_down : dev_up);
    accel_dev_ema_.z +=
        (accel_dev.z - accel_dev_ema_.z) * (accel_dev.z < accel_dev_ema_.z ? dev_down : dev_up);

    // Euclidean magnitudes are invariant under the calibrated sensor-to-head
    // rotation. A componentwise maximum would make the same physical diagonal
    // motion look quieter merely because it was split across sensor axes.
    stillness_degs_ = vec_norm(gyro_dev_ema_);
    accel_dev_mps2_ = vec_norm(accel_dev_ema_);
    const float raw_rate = bias_done_ ? vec_norm(gyro_lpf_) : 0.0f;
    const bool quiet = stillness_degs_ < cfg_.rest_gyro_dev_degs &&
                       accel_dev_mps2_ < cfg_.rest_accel_dev_mps2 &&
                       raw_rate < cfg_.rest_raw_rate_limit_degs;
    const bool motion = stillness_degs_ > cfg_.motion_dev_threshold_degs ||
                        accel_dev_mps2_ > cfg_.motion_accel_dev_mps2 ||
                        raw_rate > cfg_.motion_raw_rate_degs;
    rest_now_ = quiet;
    if (motion) {
        if (still_ || still_time_ > 0.0f) {
            ++motion_events_;
        }
        still_time_ = 0.0f;
        still_ = false;
        holdoff_s_ = cfg_.motion_holdoff_s;
    } else {
        if (quiet) {
            still_time_ += dt;
            if (still_time_ >= cfg_.still_hold_s) {
                still_ = true;
            }
        }
        if (holdoff_s_ > 0.0f) {
            holdoff_s_ = std::max(0.0f, holdoff_s_ - dt);
        }
    }
}

// Move the estimate toward `target` with the given first-order gain, bounded by
// `max_step` and hard-clamped to the documented bias band. Every runtime update
// goes through here, so no path can jump the estimate.
void PoseEstimator::step_bias(const Vec3& target, float k, float max_step) {
    const float targets[3] = {target.x, target.y, target.z};
    float* values[3] = {&bias_degs_.x, &bias_degs_.y, &bias_degs_.z};
    for (int axis = 0; axis < 3; ++axis) {
        float step = (targets[axis] - *values[axis]) * k;
        step = std::max(-max_step, std::min(max_step, step));
        const float value = *values[axis] + step;
        *values[axis] = std::max(-cfg_.bias_limit_degs, std::min(cfg_.bias_limit_degs, value));
    }
}

void PoseEstimator::reset_calibration_window() {
    calibration_run_s_ = 0.0f;
    calibration_run_samples_ = 0;
    calibration_sum_ = Vec3{};
    calibration_accel_sum_ = Vec3{};
}

// Adds one sample to the contiguous calibration window (or discards the window
// when the sample does not qualify) and returns true once a complete window is
// available in calibration_sum_ / calibration_run_samples_ (and the matching
// accelerometer sum). The caller consumes it and resets the window.
bool PoseEstimator::accumulate_calibration_window(const Vec3& raw, const Vec3& accel, float dt,
                                                  bool qualifies) {
    if (!qualifies) {
        reset_calibration_window();
        return false;
    }
    calibration_run_s_ += dt;
    calibration_run_samples_ += 1;
    calibration_sum_.x += raw.x;
    calibration_sum_.y += raw.y;
    calibration_sum_.z += raw.z;
    calibration_accel_sum_.x += accel.x;
    calibration_accel_sum_.y += accel.y;
    calibration_accel_sum_.z += accel.z;
    return calibration_run_samples_ >= 1 && calibration_run_samples_ >= cfg_.bias_samples &&
           calibration_run_s_ >= cfg_.calibration_window_s;
}

Vec3 PoseEstimator::confirm_episode_mean(int index) const {
    const float n = static_cast<float>(confirm_windows_[index]);
    return Vec3{confirm_sums_[index].x / n, confirm_sums_[index].y / n, confirm_sums_[index].z / n};
}

void PoseEstimator::reset_escape() {
    escape_dwell_s_ = 0.0f;
    escape_open_ = false;
    escape_armed_ = false;
    escape_return_dwell_s_ = 0.0f;
}

// A further calibration-grade window during startup confirmation. Windows are
// averaged per rest episode (the startup window opens the first one); the
// first pair of episodes whose means agree decides. A consensus inside
// routine authority confirms the estimate and ends confirmation. A consensus
// outside it replaces the bias in one step (the pre-reseed bias is
// snapshotted) and confirmation keeps watching later episodes: the first one
// that agrees with the consensus ends it; one that instead agrees with the
// snapshot reverts the bias to it in one step (two separate steady slow turns
// had outvoted a correct startup bias) and ends it. One continuous slow turn
// is a single episode, so it can never confirm itself - unless gravity proves
// the startup bias wrong (confirm_single_episode).
void PoseEstimator::confirm_startup_window(const Vec3& mean, const Vec3& accel_mean) {
    // Gravity-verified single episode: the windows of the current rest
    // episode after the startup window itself.
    if (!single_active_ || single_episode_ != motion_events_) {
        single_active_ = true;
        single_episode_ = motion_events_;
        single_sum_ = Vec3{};
        single_windows_ = 0;
        single_first_gravity_ = accel_mean;
        single_first_s_ = time_s_;
        single_rotation_ = Quat{};
    }
    single_sum_.x += mean.x;
    single_sum_.y += mean.y;
    single_sum_.z += mean.z;
    ++single_windows_;

    int current = confirm_count_ - 1;
    if (current >= 0 && confirm_episodes_[current] == motion_events_) {
        confirm_sums_[current].x += mean.x;
        confirm_sums_[current].y += mean.y;
        confirm_sums_[current].z += mean.z;
        ++confirm_windows_[current];
    } else if (confirm_count_ < kMaxConfirmEpisodes) {
        current = confirm_count_++;
        confirm_sums_[current] = mean;
        confirm_windows_[current] = 1;
        confirm_episodes_[current] = motion_events_;
    } else {
        confirm_active_ = false;
        return;
    }
    const Vec3 episode = confirm_episode_mean(current);
    const float agree = cfg_.startup_confirm_agree_degs;
    if (confirm_reseeded_) {
        if (current <= confirm_pair_last_) {
            return;  // still inside the consensus episode
        }
        if (vec_norm(vec_sub(episode, confirm_consensus_)) <= agree) {
            confirm_active_ = false;
        } else if (vec_norm(vec_sub(episode, confirm_snapshot_)) <= agree) {
            bias_degs_ = clamp_bias(confirm_snapshot_, cfg_.bias_limit_degs);
            ++startup_reverts_;
            reset_escape();
            confirm_active_ = false;
        }
        return;
    }
    for (int j = 0; j < current; ++j) {
        const Vec3 other = confirm_episode_mean(j);
        if (vec_norm(vec_sub(episode, other)) > agree) {
            continue;
        }
        const Vec3 consensus{0.5f * (episode.x + other.x), 0.5f * (episode.y + other.y),
                             0.5f * (episode.z + other.z)};
        if (vec_norm(vec_sub(consensus, bias_degs_)) > cfg_.adapt_residual_cap_degs) {
            confirm_snapshot_ = bias_degs_;
            confirm_consensus_ = consensus;
            confirm_pair_last_ = current;
            confirm_reseeded_ = true;
            bias_degs_ = clamp_bias(consensus, cfg_.bias_limit_degs);
            ++startup_reseeds_;
            reset_escape();
        } else {
            confirm_active_ = false;
        }
        return;
    }
    confirm_single_episode(current, accel_mean);
}

// One rest episode as confirmation evidence. A steady slow turn and a stale
// bias are the same gyro measurement, but not the same gravity measurement:
// if the current bias were right, the head turned at d = (episode mean -
// bias) during the episode, and gravity, fixed in the earth frame, would have
// swung through the angle that rotation predicts in the sensor frame. Only
// the component of d along gravity (a yaw about the vertical) predicts no
// swing. So when d has a horizontal part beyond routine authority, predicts
// a clear swing, and gravity stayed put, the current bias is wrong by that
// part and the episode mean is the measured rate at rest. The vertical part
// cannot be checked by gravity; it is taken with the episode on the strength
// of that proof (the startup window has been shown to contain motion) and
// the revert snapshot keeps it: a later episode that agrees with the startup
// bias corrected only horizontally restores that instead.
bool PoseEstimator::confirm_single_episode(int current, const Vec3& accel_mean) {
    if (cfg_.startup_episode_min_s <= 0.0f || !single_active_ || single_windows_ < 2) {
        return false;
    }
    const float span = time_s_ - single_first_s_;
    if (span < cfg_.startup_episode_min_s) {
        return false;
    }
    const float n = static_cast<float>(single_windows_);
    const Vec3 episode{single_sum_.x / n, single_sum_.y / n, single_sum_.z / n};
    const Vec3 d = vec_sub(episode, bias_degs_);
    const float d_norm = vec_norm(d);
    const float g_norm = vec_norm(accel_mean);
    const float g0_norm = vec_norm(single_first_gravity_);
    if (d_norm <= cfg_.adapt_residual_cap_degs || g_norm < 1e-3f || g0_norm < 1e-3f) {
        return false;
    }
    const Vec3 g{accel_mean.x / g_norm, accel_mean.y / g_norm, accel_mean.z / g_norm};
    const float d_vertical = d.x * g.x + d.y * g.y + d.z * g.z;
    const Vec3 d_horizontal{d.x - d_vertical * g.x, d.y - d_vertical * g.y, d.z - d_vertical * g.z};
    const float h_norm = vec_norm(d_horizontal);
    if (h_norm < cfg_.startup_episode_tilt_degs) {
        return false;
    }
    // The window means must stand for the whole episode: the rotation the bias
    // in use integrated over every sample must carry the window-mean
    // disagreement's horizontal part (a slow tilt during windows that was
    // undone between them is not evidence).
    const Quat& q = single_rotation_;
    const float v_norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    const float rot_rad = 2.0f * std::atan2(v_norm, std::fabs(q.w));
    const float sign = q.w < 0.0f ? -1.0f : 1.0f;
    const float k = v_norm > 1e-9f ? sign * rot_rad / v_norm / kRadPerDeg : 0.0f;
    const Vec3 rot_deg{q.x * k, q.y * k, q.z * k};
    const float along = (rot_deg.x * d_horizontal.x + rot_deg.y * d_horizontal.y +
                         rot_deg.z * d_horizontal.z) / h_norm;
    if (along < 0.5f * h_norm * span) {
        return false;
    }
    // atan2 of |cross| and dot keeps sub-degree angles exact in float (bug 25).
    auto angle_deg = [](const Vec3& a, const Vec3& b) {
        const Vec3 c{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        return std::atan2(vec_norm(c), a.x * b.x + a.y * b.y + a.z * b.z) / kRadPerDeg;
    };
    // If that rotation were real, gravity (fixed in the earth frame) would
    // now read conj(q) * g0 * q in the sensor frame.
    const Vec3 g0{single_first_gravity_.x / g0_norm, single_first_gravity_.y / g0_norm,
                  single_first_gravity_.z / g0_norm};
    const Quat moved = quat_multiply(quat_multiply(quat_conjugate(q), Quat{0.0f, g0.x, g0.y, g0.z}), q);
    const float predicted_deg = angle_deg(g0, Vec3{moved.x, moved.y, moved.z});
    if (predicted_deg < cfg_.startup_episode_predict_min_deg) {
        return false;
    }
    const float observed_deg = angle_deg(g0, g);
    if (observed_deg > cfg_.startup_episode_observed_ratio * predicted_deg) {
        return false;
    }
    confirm_snapshot_ = clamp_bias(Vec3{bias_degs_.x + d_horizontal.x, bias_degs_.y + d_horizontal.y,
                                        bias_degs_.z + d_horizontal.z},
                                   cfg_.bias_limit_degs);
    confirm_consensus_ = episode;
    confirm_pair_last_ = current;
    confirm_reseeded_ = true;
    bias_degs_ = clamp_bias(episode, cfg_.bias_limit_degs);
    ++startup_reseeds_;
    ++startup_episode_reseeds_;
    reset_escape();
    return true;
}

void PoseEstimator::finish_calibration() {
    const float n = static_cast<float>(calibration_run_samples_);
    const Vec3 mean{calibration_sum_.x / n, calibration_sum_.y / n, calibration_sum_.z / n};
    bias_degs_ = clamp_bias(mean, cfg_.bias_limit_degs);
    calibrated_ = true;
    bias_done_ = true;
    reset_calibration_window();
    confirm_count_ = 0;
    confirm_time_s_ = 0.0f;
    confirm_reseeded_ = false;
    confirm_pair_last_ = -1;
    single_active_ = false;  // the startup window itself is not single-episode evidence
    confirm_active_ = cfg_.startup_confirm_s > 0.0f;
    if (confirm_active_) {
        confirm_sums_[0] = mean;
        confirm_windows_[0] = 1;
        confirm_episodes_[0] = motion_events_;
        confirm_count_ = 1;
    }

    // Restart the fusion from a clean, gravity-aligned state.
    filter_.reset();
    mag_lock_.reset();
    have_tick_ = false;
    have_ref_ = false;
    initialized_ = false;
    have_prev_live_ = false;
    drift_correction_ = Quat{};
    fused_ = 0;

    escape_dwell_s_ = 0.0f;
    escape_open_ = false;
    escape_armed_ = false;
    escape_return_dwell_s_ = 0.0f;
    adapt_state_ = BiasAdaptState::idle;
}

bool PoseEstimator::add_sample(const ImuSample& sample) {
    if (!std::isfinite(sample.gyro_degs.x) || !std::isfinite(sample.gyro_degs.y) ||
        !std::isfinite(sample.gyro_degs.z) || !std::isfinite(sample.accel_mps2.x) ||
        !std::isfinite(sample.accel_mps2.y) || !std::isfinite(sample.accel_mps2.z) ||
        (cfg_.mag_weight > 0.0f &&
         (!std::isfinite(sample.mag_ut.x) || !std::isfinite(sample.mag_ut.y) ||
          !std::isfinite(sample.mag_ut.z)))) {
        return false;
    }

    float dt = 1.0f / 476.0f;
    const uint32_t tick = sample.tick_100us;
    if (have_tick_) {
        const uint32_t delta_ticks = tick - last_tick_;
        const float dt_candidate = static_cast<float>(delta_ticks) * 1e-4f;
        if (dt_candidate > 1e-5f && dt_candidate < 0.05f) {
            dt = dt_candidate;
        } else if (dt_candidate >= 0.05f) {
            // Samples were lost: skip this one and restart the interval rather than
            // integrating it with a substituted dt (which silently halves motion).
            // The missing interval may contain arbitrary motion, so it also breaks
            // startup calibration, rest qualification, rollback confirmation and
            // drift-absorption continuity.
            last_tick_ = tick;
            reset_calibration_window();
            ++motion_events_;
            gyro_lpf_ = sample.gyro_degs;
            gyro_dev_ema_ = Vec3{};
            accel_lpf_ = sample.accel_mps2;
            accel_dev_ema_ = Vec3{};
            rest_lpf_ready_ = true;
            rest_now_ = false;
            still_ = false;
            still_time_ = 0.0f;
            holdoff_s_ = cfg_.motion_holdoff_s;
            escape_return_dwell_s_ = 0.0f;
            have_prev_live_ = false;
            return false;
        }
    }
    last_tick_ = tick;
    have_tick_ = true;
    ++warmup_samples_;
    time_s_ += dt;

    // Multi-second sensor warmup: the first samples after stream-on are not
    // trustworthy, so they never reach the rest detector or the fusion (the
    // legacy sample floor is still honoured for older callers).
    if (time_s_ < cfg_.warmup_s || warmup_samples_ < cfg_.settle_samples) {
        return false;
    }

    const Vec3 raw = sample.gyro_degs;
    update_rest_detector(raw, sample.accel_mps2, dt);

    if (!bias_done_) {
        adapt_state_ = BiasAdaptState::calibrating;
        // One contiguous, high-confidence rest window. Any sample that is not
        // high-confidence rest discards the run outright: quiet fragments
        // separated by motion are never stitched together into a bias.
        const bool window_sample =
            still_ && rest_now_ && holdoff_s_ <= 0.0f &&
            stillness_degs_ < cfg_.calibration_gyro_dev_degs &&
            accel_dev_mps2_ < cfg_.calibration_accel_dev_mps2;
        calibration_phase_s_ += dt;
        if (accumulate_calibration_window(raw, sample.accel_mps2, dt, window_sample)) {
            finish_calibration();
        } else if (cfg_.calibration_timeout_s > 0.0f &&
                   calibration_phase_s_ >= cfg_.calibration_timeout_s) {
            // Never open tracking with an uncalibrated zero bias. Starting from
            // that fallback produced several degrees of visible settling in the
            // live logs. Reset only the diagnostic epoch and continue waiting for
            // one valid contiguous window.
            calibration_phase_s_ = 0.0f;
        }
        return false;
    }

    // --- runtime bias ownership ------------------------------------------------
    const bool rest_eligible = still_ && rest_now_ && holdoff_s_ <= 0.0f;
    if (confirm_active_) {
        // Startup confirmation keeps collecting calibration-grade windows
        // (same gates as the startup window, plus the runtime raw-rate rest
        // ceiling) for a bounded time after tracking opens.
        const bool window_sample = rest_eligible &&
                                   stillness_degs_ < cfg_.calibration_gyro_dev_degs &&
                                   accel_dev_mps2_ < cfg_.calibration_accel_dev_mps2;
        if (accumulate_calibration_window(raw, sample.accel_mps2, dt, window_sample)) {
            const float n = static_cast<float>(calibration_run_samples_);
            const Vec3 mean{calibration_sum_.x / n, calibration_sum_.y / n, calibration_sum_.z / n};
            const Vec3 accel_mean{calibration_accel_sum_.x / n, calibration_accel_sum_.y / n,
                                  calibration_accel_sum_.z / n};
            reset_calibration_window();
            confirm_startup_window(mean, accel_mean);
        }
        confirm_time_s_ += dt;
        if (confirm_time_s_ >= cfg_.startup_confirm_s) {
            confirm_active_ = false;
        }
        if (!confirm_active_) {
            reset_calibration_window();
        }
    }
    const float raw_rate_magnitude = vec_norm(gyro_lpf_);
    const Vec3 residual = vec_sub(gyro_lpf_, bias_degs_);
    const bool routine_eligible = rest_eligible && vec_norm(residual) <= cfg_.adapt_residual_cap_degs;

    // Guarded escape rollback, checked before any adaptation this sample. Once
    // the escape has moved the estimate away from its snapshot, a stable return
    // of the observed rate into the snapshot's band means the escape was
    // chasing an episodic rotation (an ambiguous slow turn) rather than a
    // persistent bias: restore the snapshot and leave no post-stop reverse
    // slide. The confirmation dwell rejects a transient pass through the band.
    if (escape_open_ && escape_armed_) {
        if (!rest_eligible) {
            // Crossing the snapshot band during a real head movement is not
            // evidence that the residual disappeared. Confirmation must be one
            // continuous, qualified rest interval.
            escape_return_dwell_s_ = 0.0f;
        } else if (vec_norm(vec_sub(gyro_lpf_, escape_snapshot_)) <=
                   cfg_.escape_return_tol_degs) {
            escape_return_dwell_s_ += dt;
            if (escape_return_dwell_s_ >= cfg_.escape_return_dwell_s) {
                bias_degs_ = escape_snapshot_;
                ++escape_rollbacks_;
                escape_open_ = false;
                escape_armed_ = false;
                escape_dwell_s_ = 0.0f;
                escape_return_dwell_s_ = 0.0f;
                adapt_state_ = BiasAdaptState::idle;
            }
        } else {
            escape_return_dwell_s_ = 0.0f;
        }
    }

    // While an escape excursion is armed the fast routine update is NOT allowed
    // to undo it: that unwind is exactly the post-stop reverse slide the guard
    // exists to prevent. The excursion is resolved either by the guarded
    // rollback below (the observed rate returns to the snapshot band) or by the
    // slow escape itself (the rate persists, so the estimate catches up to it).
    const bool escape_excursion_active = escape_open_ && escape_armed_;

    adapt_state_ = BiasAdaptState::idle;
    if (routine_eligible && !escape_excursion_active) {
        // Locally bounded routine update: the target sits at most
        // `adapt_residual_cap_degs` away from the current estimate, so a
        // deliberate slow yaw is never learned wholesale.
        adapt_state_ = BiasAdaptState::routine;
        step_bias(gyro_lpf_, dt / cfg_.bias_adapt_fast_tau_s, cfg_.bias_slew_degs_per_s * dt);
        escape_dwell_s_ = 0.0f;
        escape_open_ = false;
        escape_armed_ = false;
        escape_return_dwell_s_ = 0.0f;
    } else if (rest_eligible) {
        // An unexplained residual while at rest accumulates recovery time. The
        // accumulation survives separate rest windows, so a wearer who moves
        // periodically cannot lock a stale estimate out, and it is cleared only
        // once the estimate explains the data again.
        escape_dwell_s_ += dt;
        if (escape_dwell_s_ >= cfg_.adapt_escape_s) {
            adapt_state_ = BiasAdaptState::escape;
            if (!escape_open_) {
                escape_open_ = true;
                escape_snapshot_ = bias_degs_;
                escape_armed_ = false;
                escape_return_dwell_s_ = 0.0f;
            }
            if (vec_norm(vec_sub(bias_degs_, escape_snapshot_)) >
                cfg_.escape_excursion_min_degs) {
                escape_armed_ = true;
            }
            // Deliberately slow (the slow tau): a 20 s ambiguous turn may give
            // up only a small part of its travel while the escape watches, and
            // the rollback above releases that part when the turn stops.
            step_bias(gyro_lpf_, dt / cfg_.bias_adapt_slow_tau_s, cfg_.bias_slew_degs_per_s * dt);
        }
    }

    // Single-episode evidence: the rotation the bias in use attributes to the
    // head since the episode's first window (all samples, not only windows).
    if (confirm_active_ && single_active_ && !confirm_reseeded_) {
        const Vec3 w{(raw.x - bias_degs_.x) * kRadPerDeg * dt, (raw.y - bias_degs_.y) * kRadPerDeg * dt,
                     (raw.z - bias_degs_.z) * kRadPerDeg * dt};
        const float angle = vec_norm(w);
        Quat step{};
        if (angle > 1e-9f) {
            const float s = std::sin(0.5f * angle) / angle;
            step = Quat{std::cos(0.5f * angle), w.x * s, w.y * s, w.z * s};
        }
        single_rotation_ = quat_normalize(quat_multiply(single_rotation_, step));
    }

    // --- pose path: always live -----------------------------------------------
    if (!initialized_) {
        filter_.set_orientation(accel_align_quat(map_accel(sample.accel_mps2)));
        initialized_ = true;
        set_reference(filter_.orientation());
        have_prev_live_ = false;
    }

    const Vec3 corrected{raw.x - bias_degs_.x, raw.y - bias_degs_.y, raw.z - bias_degs_.z};
    corrected_rate_degs_ = vec_norm(corrected);
    Vec3 gyro = map_gyro(corrected);
    gyro.x *= kRadPerDeg;
    gyro.y *= kRadPerDeg;
    gyro.z *= kRadPerDeg;

    filter_.update(gyro, map_accel(sample.accel_mps2), map_mag(sample.mag_ut), cfg_.mag_weight, dt);
    if (mag_lock_active_) {
        apply_mag_heading(sample, dt);
    }

    // Drift absorption is opt-in: while the head is still, each *slow*
    // incremental rotation is absorbed fully into a correction subtracted from
    // the published pose. The pose itself is never frozen or snapped - every
    // sample integrates the corrected gyro.
    const Quat live = filter_.orientation();
    const bool absorb_eligible = still_ && rest_now_ && holdoff_s_ <= 0.0f;
    if (have_prev_live_ && absorb_eligible && cfg_.drift_rate_cap_degs > 0.0f &&
        raw_rate_magnitude < cfg_.drift_rate_cap_degs) {
        const Quat increment = quat_multiply(live, quat_conjugate(q_prev_live_));
        drift_correction_ = quat_multiply(increment, drift_correction_);
    }
    q_prev_live_ = live;
    have_prev_live_ = true;

    // The correction must never become a permanent workspace rotation, so it
    // bleeds back toward identity and is hard-clamped.
    if (absorb_eligible) {
        if (cfg_.drift_leak_tau_s > 0.0f) {
            float scale = 1.0f - dt / cfg_.drift_leak_tau_s;
            if (scale < 0.0f) {
                scale = 0.0f;
            }
            drift_correction_ = quat_scaled(drift_correction_, scale);
        }
        const float correction_deg = quat_angle_degs(drift_correction_);
        if (correction_deg > cfg_.drift_limit_degs) {
            drift_correction_ =
                quat_scaled(drift_correction_, cfg_.drift_limit_degs / correction_deg);
        }
    }

    if (!have_ref_) {
        set_reference(filter_.orientation());
    }
    ++fused_;
    return true;
}

static Vec3 quat_rotate_vec(const Quat& q, const Vec3& v) {
    const Quat p{0.0f, v.x, v.y, v.z};
    const Quat r = quat_multiply(quat_multiply(q, p), quat_conjugate(q));
    return Vec3{r.x, r.y, r.z};
}

// The heading lock sees the calibrated field in the earth frame of the current
// estimate and returns a small rotation about earth Z. It is premultiplied onto
// the filter state, so it changes yaw only and the published pose stays
// continuous (the correction is rate-limited, never a snap).
void PoseEstimator::apply_mag_heading(const ImuSample& sample, float dt) {
    const bool fresh = !have_last_mag_ || sample.mag_ut.x != last_mag_raw_.x ||
                       sample.mag_ut.y != last_mag_raw_.y || sample.mag_ut.z != last_mag_raw_.z;
    last_mag_raw_ = sample.mag_ut;
    have_last_mag_ = true;
    // The lock's online hard-iron tracker may refine the stored offset (it
    // returns the stored one unchanged when tracking is disabled).
    MagCalibration calibration = cfg_.mag_calibration;
    calibration.hard_iron_ut = mag_lock_.track_hard_iron(sample, bias_degs_, cfg_.mag_calibration);
    const Vec3 head_field = map_mag(mag_to_package(sample.mag_ut, calibration));
    const Quat q = filter_.orientation();
    const Vec3 earth_field = quat_rotate_vec(q, head_field);
    const float yaw_rad = mag_lock_.update(earth_field, fresh, corrected_rate_degs_, dt);
    if (yaw_rad != 0.0f) {
        const float h = 0.5f * yaw_rad;
        const Quat rz{std::cos(h), 0.0f, 0.0f, std::sin(h)};
        filter_.set_orientation(quat_multiply(rz, q));
    }
}

void PoseEstimator::recenter() {
    // Reference the LIVE orientation and clear the correction together: taking the
    // reference from the corrected pose and then clearing the correction would
    // leave a jump equal to the old correction.
    set_reference(filter_.orientation());
    drift_correction_ = Quat{};
    have_prev_live_ = false;
}

float PoseEstimator::drift_correction_degs() const {
    return quat_angle_degs(drift_correction_);
}

Quat PoseEstimator::published_orientation() const {
    return quat_multiply(quat_conjugate(drift_correction_), filter_.orientation());
}

// The reference is split into its heading (a rotation about earth Z) and its
// tilt (the remainder, whose axis is horizontal): q_ref = H * T. The published
// relative rotation is conj(H) * q * conj(T):
//  - an earth-frame yaw after recenter stays a pure yaw (H commutes with it and
//    T cancels), so a tilted recenter cannot mix yaw into pitch/roll (bug 2);
//  - a head nod after recenter stays a pitch whatever direction the wearer
//    faced when recentering. The previous q * conj(q_ref) rotated every head
//    axis by the reference heading, so after recentering at heading psi a nod
//    became sin(psi) roll (bug 23: 14-27 deg of nod-to-roll coupling in the
//    field after successive recenters, ~2 deg before the first one).
// At startup the reference comes from the accelerometer alone (zero heading),
// so this is identical to the old formula until the first recenter.
void PoseEstimator::set_reference(const Quat& q) {
    ref_heading_ = quat_twist_about(q, 0.0f, 0.0f, 1.0f);
    ref_tilt_ = quat_multiply(quat_conjugate(ref_heading_), q);
    have_ref_ = true;
}

Euler PoseEstimator::euler() const {
    return quat_to_euler(quat());
}

Quat PoseEstimator::quat() const {
    return quat_multiply(quat_conjugate(ref_heading_),
                         quat_multiply(published_orientation(), quat_conjugate(ref_tilt_)));
}

}  // namespace gt
