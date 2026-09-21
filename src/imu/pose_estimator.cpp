#include "imu/pose_estimator.h"

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

float max_abs(const Vec3& v) {
    const float ax = std::fabs(v.x);
    const float ay = std::fabs(v.y);
    const float az = std::fabs(v.z);
    return ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
}

}  // namespace

void PoseEstimator::configure(const Config& cfg) {
    cfg_ = cfg;
    filter_.reset();
    filter_.set_beta(cfg.beta);
    bias_sum_still_ = Vec3{};
    bias_degs_ = Vec3{};
    settle_count_ = 0;
    still_count_ = 0;
    phase_samples_ = 0;
    bias_done_ = false;
    have_tick_ = false;
    last_tick_ = 0;
    fast_ema_ = Vec3{};
    dev_ema_ = Vec3{};
    ema_ready_ = false;
    still_ = false;
    still_time_ = 0.0f;
    stillness_degs_ = 0.0f;
    holdoff_s_ = 0.0f;
    still_since_motion_s_ = 0.0f;
    q_ref_ = Quat{};
    q_frozen_ = Quat{};
    drift_correction_ = Quat{};
    q_prev_live_ = Quat{};
    have_prev_live_ = false;
    have_ref_ = false;
    frozen_ = false;
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

bool PoseEstimator::add_sample(const ImuSample& sample) {
    if (settle_count_ < cfg_.settle_samples) {
        ++settle_count_;
        return false;
    }

    if (!std::isfinite(sample.gyro_degs.x) || !std::isfinite(sample.gyro_degs.y) ||
        !std::isfinite(sample.gyro_degs.z) || !std::isfinite(sample.accel_mps2.x) ||
        !std::isfinite(sample.accel_mps2.y) || !std::isfinite(sample.accel_mps2.z)) {
        return false;
    }

    const Vec3 raw = sample.gyro_degs;

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
            last_tick_ = tick;
            have_prev_live_ = false;
            return false;
        }
    }
    last_tick_ = tick;
    have_tick_ = true;

    if (!ema_ready_) {
        fast_ema_ = raw;
        dev_ema_ = Vec3{};
        ema_ready_ = true;
    } else {
        const float af = dt / (cfg_.fast_ema_tau_s + dt);
        fast_ema_.x += (raw.x - fast_ema_.x) * af;
        fast_ema_.y += (raw.y - fast_ema_.y) * af;
        fast_ema_.z += (raw.z - fast_ema_.z) * af;
        const Vec3 dev{std::fabs(raw.x - fast_ema_.x), std::fabs(raw.y - fast_ema_.y),
                       std::fabs(raw.z - fast_ema_.z)};
        const float ad = dt / (cfg_.dev_ema_tau_s + dt);
        dev_ema_.x += (dev.x - dev_ema_.x) * ad;
        dev_ema_.y += (dev.y - dev_ema_.y) * ad;
        dev_ema_.z += (dev.z - dev_ema_.z) * ad;
    }
    // Stillness is measured from how much the signal *varies*, never from its
    // absolute value - otherwise a stale bias estimate would look like motion
    // and the estimator could never correct itself.
    stillness_degs_ = max_abs(dev_ema_);
    if (!still_) {
        if (stillness_degs_ < cfg_.still_dev_threshold_degs) {
            still_time_ += dt;
            if (still_time_ >= cfg_.still_hold_s) {
                still_ = true;
            }
        } else {
            // Decay rather than reset, so a periodic tremor (which dips below the
            // enter threshold twice per cycle) can still accumulate stillness.
            still_time_ = std::max(0.0f, still_time_ - dt);
        }
    } else if (stillness_degs_ > cfg_.motion_dev_threshold_degs) {
        still_ = false;
        still_time_ = 0.0f;
    }

    // Drive the refractory counters from the deviation LEVEL, not from the still
    // flag: the flag flickers on noise, and resetting on every flicker would keep
    // adaptation permanently gated off after any motion.
    if (stillness_degs_ > cfg_.motion_dev_threshold_degs) {
        holdoff_s_ = cfg_.motion_holdoff_s;
        still_since_motion_s_ = 0.0f;
    } else {
        if (holdoff_s_ > 0.0f) {
            holdoff_s_ -= dt;
        }
        still_since_motion_s_ += dt;
    }
    const float raw_rate_magnitude = max_abs(fast_ema_);
    // Every gate is on the RAW rate: the corrected rate is a lag test (a slowly
    // ramping rotation keeps lagging the estimate by less than the cap), which is
    // how deliberate motion used to end up inside the bias.
    const bool slow_enough = raw_rate_magnitude < cfg_.adapt_rate_cap_degs;
    const bool escape = still_since_motion_s_ >= cfg_.adapt_escape_s;
    // The still flag is sticky, so it can stay true through the settling tail of a
    // movement; the absorption additionally requires a low *current* deviation so it
    // does not inflate on accelerometer settle (the bias adaptation keeps its own
    // rate and hold-off gates, which are enough there).
    const bool calib_eligible = still_ && raw_rate_magnitude < cfg_.calibration_rate_cap_degs;
    const bool adapt_eligible = still_ && holdoff_s_ <= 0.0f && (slow_enough || escape);
    const bool absorb_eligible =
        adapt_eligible && stillness_degs_ < cfg_.still_dev_threshold_degs;

    if (!bias_done_) {
        if (calib_eligible) {
            ++still_count_;
            bias_sum_still_.x += raw.x;
            bias_sum_still_.y += raw.y;
            bias_sum_still_.z += raw.z;
        }
        ++phase_samples_;
        const bool enough_still = still_count_ >= cfg_.bias_samples;
        const bool timed_out = phase_samples_ >= cfg_.bias_timeout_samples;
        if (enough_still || timed_out) {
            // Never fall back to the mean of moving samples: a contaminated bias
            // would gate out the very adaptation that could correct it, leaving a
            // permanent creep. If no still samples arrived, keep the current
            // estimate and let the continuous adaptation do the work.
            if (still_count_ > 0) {
                const float n = static_cast<float>(still_count_);
                bias_degs_ = Vec3{bias_sum_still_.x / n, bias_sum_still_.y / n, bias_sum_still_.z / n};
                bias_degs_.x = std::max(-cfg_.bias_limit_degs, std::min(cfg_.bias_limit_degs, bias_degs_.x));
                bias_degs_.y = std::max(-cfg_.bias_limit_degs, std::min(cfg_.bias_limit_degs, bias_degs_.y));
                bias_degs_.z = std::max(-cfg_.bias_limit_degs, std::min(cfg_.bias_limit_degs, bias_degs_.z));
            }
            bias_done_ = true;
            filter_.reset();
            have_tick_ = false;
            have_ref_ = false;
            initialized_ = false;
            have_prev_live_ = false;
            drift_correction_ = Quat{};
            fused_ = 0;
        }
        return false;
    }

    if (cfg_.freeze_when_still && still_) {
        if (!frozen_) {
            q_frozen_ = filter_.orientation();
            frozen_ = true;
        }
    } else if (frozen_) {
        const Quat held_relative = quat_multiply(q_frozen_, quat_conjugate(q_ref_));
        q_ref_ = quat_multiply(quat_conjugate(held_relative), filter_.orientation());
        frozen_ = false;
    }

    if (adapt_eligible) {
        // The escape path corrects slowly: it exists so a stale estimate can never
        // lock the estimator out, not to track motion.
        const float tau = (slow_enough || !escape) ? cfg_.bias_adapt_tau_s : cfg_.bias_adapt_slow_tau_s;
        const float k = dt / tau;
        bias_degs_.x += (fast_ema_.x - bias_degs_.x) * k;
        bias_degs_.y += (fast_ema_.y - bias_degs_.y) * k;
        bias_degs_.z += (fast_ema_.z - bias_degs_.z) * k;
        bias_degs_.x = std::max(-cfg_.bias_limit_degs, std::min(cfg_.bias_limit_degs, bias_degs_.x));
        bias_degs_.y = std::max(-cfg_.bias_limit_degs, std::min(cfg_.bias_limit_degs, bias_degs_.y));
        bias_degs_.z = std::max(-cfg_.bias_limit_degs, std::min(cfg_.bias_limit_degs, bias_degs_.z));
    }

    if (!initialized_) {
        filter_.set_orientation(accel_align_quat(map_accel(sample.accel_mps2)));
        initialized_ = true;
        q_ref_ = filter_.orientation();
        have_ref_ = true;
        q_frozen_ = q_ref_;
        have_prev_live_ = false;
    }

    const Vec3 corrected{raw.x - bias_degs_.x, raw.y - bias_degs_.y, raw.z - bias_degs_.z};
    Vec3 gyro = map_gyro(corrected);
    gyro.x *= kRadPerDeg;
    gyro.y *= kRadPerDeg;
    gyro.z *= kRadPerDeg;

    filter_.update(gyro, map_accel(sample.accel_mps2), map_mag(sample.mag_ut), cfg_.mag_weight, dt);

    if (!cfg_.freeze_when_still) {
        // Drift absorption: while the head is still, each *slow* incremental
        // rotation is absorbed fully into a correction subtracted from the
        // published pose. Deliberate movements exceed the rate gate (and the
        // hold-off keeps their settling tail out), so they pass through untouched,
        // while residual creep - a long run of sub-threshold increments - is
        // cancelled instead of accumulating into a visible offset.
        const Quat live = filter_.orientation();
        // The absorption gates on its own (still + hold-off + a slow raw rate):
        // rates this small are below anything a deliberate pan produces, so this
        // never needs the escape window that the bias adaptation uses.
        if (have_prev_live_ && absorb_eligible && cfg_.drift_rate_cap_degs > 0.0f &&
            raw_rate_magnitude < cfg_.drift_rate_cap_degs) {
            const Quat increment = quat_multiply(live, quat_conjugate(q_prev_live_));
            drift_correction_ = quat_multiply(increment, drift_correction_);
        }
        q_prev_live_ = live;
        have_prev_live_ = true;

        if (adapt_eligible) {
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
    }

    if (!have_ref_) {
        q_ref_ = filter_.orientation();
        have_ref_ = true;
    }
    ++fused_;
    return true;
}

void PoseEstimator::recenter() {
    // Reference the LIVE orientation and clear the correction together: taking the
    // reference from the corrected pose and then clearing the correction would
    // leave a jump equal to the old correction.
    q_ref_ = filter_.orientation();
    have_ref_ = true;
    q_frozen_ = q_ref_;
    frozen_ = false;
    drift_correction_ = Quat{};
    have_prev_live_ = false;
}

float PoseEstimator::drift_correction_degs() const {
    return quat_angle_degs(drift_correction_);
}

Quat PoseEstimator::published_orientation() const {
    if (frozen_) {
        return q_frozen_;
    }
    return quat_multiply(quat_conjugate(drift_correction_), filter_.orientation());
}

Euler PoseEstimator::euler() const {
    // Relative rotation expressed in the EARTH frame: q * q_ref^-1. Using the
    // body-frame order (q_ref^-1 * q) would mix yaw into pitch/roll whenever
    // the head was tilted when it was recentered.
    const Quat rel = quat_multiply(published_orientation(), quat_conjugate(q_ref_));
    return quat_to_euler(rel);
}

Quat PoseEstimator::quat() const {
    return quat_multiply(published_orientation(), quat_conjugate(q_ref_));
}

}  // namespace gt
