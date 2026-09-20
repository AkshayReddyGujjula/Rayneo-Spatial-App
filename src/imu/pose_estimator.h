#pragma once

#include "imu/fusion.h"
#include "imu/gt_protocol.h"

#include <array>

namespace gt {

class PoseEstimator {
public:
    struct Config {
        float beta = 0.05f;
        float mag_weight = 0.0f;
        int settle_samples = 150;
        int bias_samples = 600;
        int bias_timeout_samples = 4000;
        // Enter threshold must sit below the exit threshold but high enough that a
        // mild tremor can still reach it - a band with no way back to stillness
        // would freeze both adaptation paths (and so let creep run unchecked).
        float still_dev_threshold_degs = 1.0f;
        float motion_dev_threshold_degs = 1.2f;
        // With the magnetometer off, a slow yaw rotation and a yaw bias are
        // physically indistinguishable, so every gate here is deliberately
        // conservative: the startup calibration captures the bulk of the bias
        // (a real bias stays under ~1.5 deg/s), the continuous adaptation only
        // touches rates a real bias could explain, the estimate can never exceed
        // bias_limit_degs, and a long still period always re-opens adaptation so a
        // stale estimate can recover without any runaway.
        float calibration_rate_cap_degs = 1.5f;
        float adapt_rate_cap_degs = 0.3f;
        float drift_rate_cap_degs = 0.5f;
        float bias_limit_degs = 1.5f;
        // Long enough that a deliberate slow pan (a few seconds) never trips it,
        // short enough that a stale estimate recovers within seconds of stillness.
        float adapt_escape_s = 8.0f;
        float bias_adapt_slow_tau_s = 10.0f;
        float still_hold_s = 0.8f;
        float fast_ema_tau_s = 0.2f;
        float dev_ema_tau_s = 0.5f;
        float bias_adapt_tau_s = 3.0f;
        // After any detected motion the adaptation paths stay hands-off for this
        // long, so the slow ramp and settle of a deliberate head movement are not
        // mistaken for drift.
        float motion_holdoff_s = 1.0f;
        bool freeze_when_still = false;
        bool map_package_axes = true;
        std::array<float, 9> sensor_to_head{
            1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, -1.0f,
            0.0f, 1.0f, 0.0f,
        };
    };

    PoseEstimator() = default;
    explicit PoseEstimator(const Config& cfg) { configure(cfg); }

    void configure(const Config& cfg);

    bool add_sample(const ImuSample& sample);
    void recenter();

    bool bias_done() const { return bias_done_; }
    bool still() const { return still_; }
    bool frozen() const { return frozen_; }
    uint64_t samples_fused() const { return fused_; }
    Vec3 gyro_bias_degs() const { return bias_degs_; }
    float stillness_degs() const { return stillness_degs_; }
    float drift_correction_degs() const;
    Euler euler() const;
    Quat quat() const;

private:
    Vec3 map_gyro(const Vec3& v) const;
    Vec3 map_accel(const Vec3& v) const;
    Vec3 map_mag(const Vec3& v) const;
    Quat published_orientation() const;

    Config cfg_;
    MadgwickFilter filter_;

    Vec3 bias_sum_still_;
    Vec3 bias_degs_;
    int settle_count_ = 0;
    int still_count_ = 0;
    int phase_samples_ = 0;
    bool bias_done_ = false;

    bool have_tick_ = false;
    uint32_t last_tick_ = 0;

    Vec3 fast_ema_;
    Vec3 dev_ema_;
    bool ema_ready_ = false;
    bool still_ = false;
    float still_time_ = 0.0f;
    float stillness_degs_ = 0.0f;
    float holdoff_s_ = 0.0f;
    float still_since_motion_s_ = 0.0f;

    Quat q_ref_;
    Quat q_frozen_;
    Quat drift_correction_;
    Quat q_prev_live_;
    bool have_prev_live_ = false;
    bool have_ref_ = false;
    bool frozen_ = false;
    bool initialized_ = false;
    uint64_t fused_ = 0;
};

}  // namespace gt
