#pragma once

#include "imu/fusion.h"
#include "imu/gt_protocol.h"

namespace gt {

class PoseEstimator {
public:
    struct Config {
        float beta = 0.05f;
        float mag_weight = 0.0f;
        int settle_samples = 150;
        int bias_samples = 600;
        int bias_timeout_samples = 4000;
        float still_dev_threshold_degs = 1.2f;
        float motion_dev_threshold_degs = 2.0f;
        float still_rate_cap_degs = 5.0f;
        float still_hold_s = 0.5f;
        float fast_ema_tau_s = 0.2f;
        float dev_ema_tau_s = 0.5f;
        float bias_adapt_tau_s = 3.0f;
        bool freeze_when_still = false;
        bool map_package_axes = true;
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
    Vec3 bias_sum_all_;
    Vec3 bias_degs_;
    int settle_count_ = 0;
    int still_count_ = 0;
    int all_count_ = 0;
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

    Quat q_ref_;
    Quat q_frozen_;
    bool have_ref_ = false;
    bool frozen_ = false;
    bool initialized_ = false;
    uint64_t fused_ = 0;
};

}  // namespace gt
