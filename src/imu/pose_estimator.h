#pragma once

#include "imu/fusion.h"
#include "imu/gt_protocol.h"
#include "imu/mag_heading.h"

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
        // Startup confirmation. Tracking opens on the first window, but one
        // ~1.3 s window on a worn head is a weak estimate: replaying the
        // 2026-09-25 session from 870 start points, 20% of startup biases
        // were more than 0.35 deg/s off on the yaw axis (outside routine
        // authority), and the slow escape then let the world turn tens of
        // degrees in the first minutes (the recorded session's own start:
        // 1.4 deg/s off, ~35 deg of gyro-only drift in 5 min). For
        // startup_confirm_s after tracking opens the same calibration-grade
        // windows keep being collected and averaged per rest episode (a
        // detected motion separates episodes). The first two episodes whose
        // means agree within startup_confirm_agree_degs form a consensus;
        // one continuous slow turn is a single episode, so it can never vote
        // twice. A consensus outside the routine residual cap replaces the
        // startup bias in one step (the pose stays continuous; only its rate
        // changes) and the escape state is cleared; the replaced bias is
        // snapshotted, and a later episode agreeing with the snapshot rather
        // than the consensus reverts it in one step (two separate steady slow
        // turns at matching rates had outvoted a correct startup). The first
        // decisive episode, or the end of the period, ends confirmation for
        // good. <= 0 disables it.
        float startup_confirm_s = 60.0f;
        float startup_confirm_agree_degs = 0.3f;
        // Gravity-verified single episode. One long rest episode cannot vote
        // twice, so a contaminated startup followed by one continuous rest
        // (the recorded 2026-09-25 start: 28 windows at the true bias) was
        // left to the slow escape. A gyro alone cannot tell that rest from a
        // steady slow turn, but the accelerometer can for every axis except
        // the vertical: any real rotation turns gravity in the sensor frame
        // exactly as the gyro says, unless it is about gravity itself. So
        // when the episode's mean disagrees with the current bias by more
        // than the routine cap, its horizontal part (perpendicular to
        // gravity) is at least startup_episode_tilt_degs and is carried by
        // the rotation the bias in use integrates over every sample of the
        // episode (at least half of it), that rotation would swing gravity by
        // at least startup_episode_predict_min_deg, and the observed gravity
        // direction moved by at most startup_episode_observed_ratio of that
        // prediction, the startup bias is proven wrong by more than routine
        // authority and the episode (startup window excluded) replaces it.
        // Measured on the 2026-09-25 replay (870 starts): 60 s gyro-only
        // drift mean 7.13 -> 6.52 deg, 52 starts better / 4 worse by > 3 deg;
        // the exposure (a contaminated startup AND a coincident slow level
        // turn in the same episode) is pose_scenarios 27.
        // A pure yaw turn, at any head tilt, predicts gravity rotation only
        // through its tilted axis - and then really rotates gravity - so a
        // single slow turn can never pass. The vertical part rides on that
        // evidence; the snapshot for a revert is the startup bias with only
        // its horizontal part corrected. <= 0 span disables the rule.
        float startup_episode_min_s = 10.0f;
        float startup_episode_tilt_degs = 0.35f;
        float startup_episode_predict_min_deg = 5.0f;
        float startup_episode_observed_ratio = 0.3f;
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
        // Raw-rate ceiling for rest. The deviation gates measure fluctuation,
        // not rotation, so a smooth steady turn (a chair turn, a slow pan to a
        // side screen) passed them: measured 2026-09-25, a 360 deg chair turn
        // at ~15 deg/s qualified as rest and the escape walked the yaw bias
        // from 0.43 to the 1.5 clamp and then to -0.49 deg/s (a 0.9 deg/s
        // error, ~54 deg/min drift, after one turn). The true bias is bounded
        // by bias_limit_degs per axis (norm <= 2.6 deg/s), so a raw rate above
        // this cannot be rest. This is a RAW gate, deliberately permissive, so
        // a stale estimate can never lock adaptation out (bug 3/7). It applies
        // after startup calibration only: the startup window has its own tight
        // gates, and an out-of-band bias must still finish calibrating.
        float rest_raw_rate_limit_degs = 3.0f;
        float motion_raw_rate_degs = 4.0f;
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
        // Magnetometer heading lock (the absolute yaw reference). Active only
        // with a valid per-device calibration: without the hard-iron offset
        // the raw field is meaningless (bias of ~25 uT on a ~51 uT field).
        MagCalibration mag_calibration;
        bool mag_heading_lock = true;
        MagHeadingLock::Config mag_lock;

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
    // Startup confirmation (see Config): how many times a consensus replaced
    // the startup bias, and how many times a later episode reverted it (each
    // 0 or 1 per startup).
    uint32_t startup_reseeds() const { return startup_reseeds_; }
    uint32_t startup_reverts() const { return startup_reverts_; }
    // How many of startup_reseeds() came from one gravity-verified episode.
    uint32_t startup_episode_reseeds() const { return startup_episode_reseeds_; }
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
    bool mag_lock_active() const { return mag_lock_active_; }
    const MagHeadingLock& mag_lock() const { return mag_lock_; }
    float drift_correction_degs() const;
    Euler euler() const;
    Quat quat() const;
    // The fusion state itself (calibrated head frame -> earth, Z up), before
    // the recenter reference. Diagnostic: replay tools use it to place the
    // magnetometer field in the estimate's earth frame.
    const Quat& fused_orientation() const { return filter_.orientation(); }

private:
    Vec3 map_gyro(const Vec3& v) const;
    Vec3 map_accel(const Vec3& v) const;
    Vec3 map_mag(const Vec3& v) const;
    Quat published_orientation() const;
    void update_rest_detector(const Vec3& raw, const Vec3& accel, float dt);
    void step_bias(const Vec3& target, float k, float max_step);
    void finish_calibration();
    void reset_calibration_window();
    bool accumulate_calibration_window(const Vec3& raw, const Vec3& accel, float dt, bool qualifies);
    void confirm_startup_window(const Vec3& mean, const Vec3& accel_mean);
    bool confirm_single_episode(int current, const Vec3& accel_mean);
    void reset_escape();
    Vec3 confirm_episode_mean(int index) const;
    void set_reference(const Quat& q);

    void apply_mag_heading(const ImuSample& sample, float dt);

    Config cfg_;
    MadgwickFilter filter_;
    MagHeadingLock mag_lock_;
    bool mag_lock_active_ = false;
    Vec3 last_mag_raw_;
    bool have_last_mag_ = false;

    // Startup calibration.
    int warmup_samples_ = 0;
    float calibration_phase_s_ = 0.0f;
    float calibration_run_s_ = 0.0f;
    int calibration_run_samples_ = 0;
    Vec3 calibration_sum_;
    Vec3 calibration_accel_sum_;
    // Startup confirmation: per rest episode (index 0 opens with the startup
    // window), the sum and count of its calibration-grade window means.
    static constexpr int kMaxConfirmEpisodes = 8;
    std::array<Vec3, kMaxConfirmEpisodes> confirm_sums_{};
    std::array<int, kMaxConfirmEpisodes> confirm_windows_{};
    std::array<uint32_t, kMaxConfirmEpisodes> confirm_episodes_{};
    int confirm_count_ = 0;
    bool confirm_active_ = false;
    float confirm_time_s_ = 0.0f;
    // After a reseed: the replaced bias, the consensus, and the newest
    // episode of the consensus pair (later episodes verify it).
    bool confirm_reseeded_ = false;
    Vec3 confirm_snapshot_;
    Vec3 confirm_consensus_;
    int confirm_pair_last_ = -1;
    uint32_t startup_reseeds_ = 0;
    uint32_t startup_reverts_ = 0;
    uint32_t startup_episode_reseeds_ = 0;
    // Gravity-verified single episode: the current rest episode's windows
    // after the startup window (episode id, sum and count of window means,
    // the first window's gravity direction and completion time).
    bool single_active_ = false;
    uint32_t single_episode_ = 0;
    Vec3 single_sum_;
    int single_windows_ = 0;
    Vec3 single_first_gravity_;
    float single_first_s_ = 0.0f;
    // Sensor rotation since that first window, integrated sample by sample
    // with the bias in use: the motion the current bias claims happened.
    Quat single_rotation_;
    // Counts detected motion events; a window's rest episode is the count at
    // its completion (motion breaks any open window, so a window never spans
    // two episodes).
    uint32_t motion_events_ = 0;
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

    // Recenter reference split as q_ref = ref_heading_ * ref_tilt_.
    Quat ref_heading_;
    Quat ref_tilt_;
    Quat drift_correction_;
    Quat q_prev_live_;
    bool have_prev_live_ = false;
    bool have_ref_ = false;
    bool initialized_ = false;
    uint64_t fused_ = 0;
};

}  // namespace gt
