#pragma once

#include "imu/fusion.h"
#include "imu/gt_protocol.h"

#include <array>
#include <cstdint>

namespace gt {

// Bias-adaptation state, exposed for diagnostics. `calibrating` is the startup
// phase (no pose is published yet), `routine` is the locally bounded residual
// update, `escape` is the slow stale-estimate recovery, `idle` means the
// estimate already matches the observed rate.
enum class BiasAdaptState : int {
    idle = 0,
    routine = 1,
    escape = 2,
    calibrating = 3,
};

class PoseEstimator {
public:
    struct Config {
        float beta = 0.05f;
        float mag_weight = 0.0f;

        // --- startup calibration -------------------------------------------
        // Multi-second warmup: the first samples after stream-on are not
        // trustworthy, so they never reach the rest detector or the fusion.
        float warmup_s = 4.0f;
        // A single contiguous, high-confidence rest window of this length (and
        // at least `bias_samples` samples) captures the gyro bias. A window
        // broken by motion is discarded outright - fragments are never averaged
        // together, and a timeout never finalizes a fragmented mean.
        float calibration_window_s = 1.0f;
        // Diagnostic epoch only: reaching it resets the epoch counter but never
        // opens tracking with an uncalibrated zero bias. The app keeps asking the
        // wearer to hold still until one valid window exists.
        float calibration_timeout_s = 10.0f;
        // High-confidence gates, tighter than the rest-detector enter gates.
        // The GT's measured stationary gyro deviation is ~0.75 deg/s median
        // and 1.37 deg/s at p99 (Euclidean EMA magnitude). This threshold is
        // deliberately above that hardware noise floor; the separate runtime
        // residual cap remains much tighter so this does not create a motion
        // dead zone after startup.
        float calibration_gyro_dev_degs = 1.5f;
        float calibration_accel_dev_mps2 = 0.3f;
        // Legacy floors kept for existing callers/tools: the warmup must also
        // consume this many samples, and the calibration window must also hold
        // this many contiguous samples.
        int settle_samples = 150;
        int bias_samples = 600;

        // --- continuous rest detection (VQF-inspired) -----------------------
        // Low-passed gyro and accelerometer references; the raw signals must
        // deviate from their references by less than the enter gates for a
        // continuous dwell before the wearer counts as at rest, and either
        // signal crossing an exit gate resets the dwell. Because the
        // accelerometer takes part, a quiet gyro on a shaken package is still
        // motion (and so blocks both adaptation and the calibration window).
        float rest_gyro_dev_degs = 1.5f;
        float motion_dev_threshold_degs = 3.0f;
        float rest_accel_dev_mps2 = 0.5f;
        float motion_accel_dev_mps2 = 1.0f;
        float still_hold_s = 0.5f;
        float fast_ema_tau_s = 0.2f;
        float dev_ema_tau_s = 0.5f;
        // Falling-edge time constant for the deviation smoothing. The rising
        // edge keeps the full 0.5 s so noise flicker never validates rest,
        // but after a real pan the symmetric tail kept rest qualification -
        // and therefore all bias adaptation - starved through 2 s holds
        // (scenario 18: the estimate sat frozen for 80 s while the wearer
        // looked around). Draining fast recovers eligibility ~1 s after a
        // pan instead of ~2.5 s.
        float dev_ema_fall_tau_s = 0.08f;
        float accel_lpf_tau_s = 0.5f;

        // --- runtime bias ownership -----------------------------------------
        // The bias estimate is the single owner of steady error. Routine
        // adaptation is locally bounded: it may only chase a residual this
        // close to the current estimate, so a deliberate slow yaw (well above
        // this residual) is never learned wholesale. Steps are slew-limited.
        float adapt_residual_cap_degs = 0.35f;
        float bias_adapt_fast_tau_s = 1.0f;
        float bias_slew_degs_per_s = 0.15f;
        // Escape: an unexplained residual (above the routine cap) while at
        // rest accumulates recovery time across separate rest windows, so a
        // wearer who moves periodically cannot lock a stale estimate out. Once
        // `adapt_escape_s` is reached the estimate may follow the observed rate,
        // but only at the slow tau (deliberately slow: a 20 s ambiguous turn
        // keeps at least 80% of its travel). The pre-escape bias is snapshotted, and a
        // stable return of the observed rate close to that snapshot rolls the
        // estimate back - an ambiguous slow turn cannot leave a post-stop
        // reverse slide behind. A rate that persists is a genuine bias change
        // and is kept.
        float adapt_escape_s = 8.0f;
        float bias_adapt_slow_tau_s = 40.0f;
        float escape_return_tol_degs = 0.2f;
        float escape_return_dwell_s = 0.2f;
        float escape_excursion_min_degs = 0.05f;

        // After any detected motion the adaptation paths stay hands-off for
        // this long, so the slow ramp and settle of a deliberate head movement
        // are not mistaken for drift.
        float motion_holdoff_s = 1.0f;

        // Drift absorption is OPT-IN. When it was enabled it also swallowed
        // post-movement accelerometer settling, and its correction (which
        // nothing released) became a permanent workspace rotation - measured in
        // the field as 3.9 deg/min, i.e. the left screen drifting to centre in
        // ~10 minutes. With a single owner (the bias) no offset can accumulate.
        // Set this above the bias cap to re-enable it; the leak below then
        // bounds the correction.
        float drift_rate_cap_degs = 0.0f;
        // The correction must never become a permanent workspace rotation: the
        // bias adaptation owns the steady error, so the correction bleeds back
        // to identity (measured in the field: a stale correction grew
        // 3.9 deg/min), with a hard angle bound as a second line of defence.
        float drift_leak_tau_s = 20.0f;
        float drift_limit_degs = 2.0f;
        float bias_limit_degs = 1.5f;

        // Legacy flag, ignored: the pose path is always live (the published
        // pose integrates the corrected gyro every sample) and never freezes.
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
    // True when the startup bias came from one contiguous high-confidence rest
    // window. Tracking never opens while this is false.
    bool calibrated() const { return calibrated_; }
    // Instantaneous rest conditions (both deviation gates inside the enter
    // thresholds); `still()` is the dwell-qualified rest state.
    bool rest() const { return rest_now_; }
    bool still() const { return still_; }
    int adapt_state() const { return static_cast<int>(adapt_state_); }
    uint64_t samples_fused() const { return fused_; }
    Vec3 gyro_bias_degs() const { return bias_degs_; }
    float stillness_degs() const { return stillness_degs_; }
    float accel_dev_mps2() const { return accel_dev_mps2_; }
    float corrected_rate_degs() const { return corrected_rate_degs_; }
    uint32_t escape_rollbacks() const { return escape_rollbacks_; }
    float drift_correction_degs() const;
    Euler euler() const;
    Quat quat() const;

private:
    Vec3 map_gyro(const Vec3& v) const;
    Vec3 map_accel(const Vec3& v) const;
    Vec3 map_mag(const Vec3& v) const;
    Quat published_orientation() const;
    void update_rest_detector(const Vec3& raw, const Vec3& accel, float dt);
    void step_bias(const Vec3& target, float k, float max_step);
    void finish_calibration();

    Config cfg_;
    MadgwickFilter filter_;

    // Startup calibration.
    int warmup_samples_ = 0;
    float calibration_phase_s_ = 0.0f;
    float calibration_run_s_ = 0.0f;
    int calibration_run_samples_ = 0;
    Vec3 calibration_sum_;
    bool bias_done_ = false;
    bool calibrated_ = false;

    Vec3 bias_degs_;
    bool have_tick_ = false;
    uint32_t last_tick_ = 0;
    float time_s_ = 0.0f;

    // Continuous rest detection.
    Vec3 gyro_lpf_;
    Vec3 gyro_dev_ema_;
    Vec3 accel_lpf_;
    Vec3 accel_dev_ema_;
    bool rest_lpf_ready_ = false;
    bool rest_now_ = false;
    bool still_ = false;
    float still_time_ = 0.0f;
    float stillness_degs_ = 0.0f;
    float accel_dev_mps2_ = 0.0f;
    float holdoff_s_ = 0.0f;

    // Runtime adaptation state.
    BiasAdaptState adapt_state_ = BiasAdaptState::calibrating;
    float escape_dwell_s_ = 0.0f;
    bool escape_open_ = false;
    bool escape_armed_ = false;
    Vec3 escape_snapshot_;
    float escape_return_dwell_s_ = 0.0f;
    uint32_t escape_rollbacks_ = 0;
    float corrected_rate_degs_ = 0.0f;

    Quat q_ref_;
    Quat drift_correction_;
    Quat q_prev_live_;
    bool have_prev_live_ = false;
    bool have_ref_ = false;
    bool initialized_ = false;
    uint64_t fused_ = 0;
};

}  // namespace gt
