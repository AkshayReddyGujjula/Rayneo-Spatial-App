#pragma once

#include "imu/fusion.h"
#include "imu/gt_protocol.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gt {

// The GT magnetometer axes are rotated 90 deg about Z relative to the gyro and
// accelerometer package axes: package = (mag.y, -mag.x, mag.z). Measured on
// 2026-09-25 by a gyro-constrained fit over a 180 s worn recording (full 360
// deg turn, nods, tilts): the free rotation fit landed within 2 deg of this
// permutation. Feeding the raw mag axes as package axes was what produced the
// historical ~4.8 deg/s yaw spin.
constexpr std::array<float, 9> kMagToPackage{
    0.0f, 1.0f, 0.0f,
    -1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f,
};

// Per-device magnetometer calibration. The hard-iron offset (the glasses' own
// magnetised parts, constant in the mag frame) is in raw mag axes; measured
// ~(-17, -18.7, 1.8) uT on this unit, which is why |raw| read ~71 uT on top
// of the ~51 uT local earth field.
struct MagCalibration {
    bool valid = false;
    Vec3 hard_iron_ut;
    float field_ut = 0.0f;  // |B| seen during calibration (diagnostic only)
    // Metadata for the UI (optional keys in the file): recorded with in-ear
    // earbuds worn, and the calibration tool's 0..1 guidance score (-1 when
    // unknown, e.g. files written before it existed).
    bool earbuds = false;
    float quality = -1.0f;
};

Vec3 mag_to_package(const Vec3& raw_ut, const MagCalibration& cal);

// Result of fitting a hard-iron offset from a gyro-tracked rotation sequence.
struct MagFitResult {
    bool ok = false;
    Vec3 hard_iron_ut;
    float field_ut = 0.0f;
    float residual_rms_ut = 0.0f;
    float rotation_coverage_deg = 0.0f;  // largest attitude change from the start
    float tilt_coverage_deg = 0.0f;      // largest change of the gravity direction
    // How well the recording pinned each hard-iron component down: the
    // eigenvalues of I - Mbar^T Mbar, where Mbar is the mean of the
    // gyro-tracked rotations R_t P over the used samples. 0 = that direction
    // never rotated (unobservable), 1 = rotations spread uniformly over all
    // orientations. `observability` is the smallest one, `weak_axis_package`
    // the package-frame direction it belongs to (rotate about other axes to
    // improve it).
    float observability = 0.0f;
    Vec3 weak_axis_package;
    const char* code = "";
};

// Heuristic 0..1 guidance score for a fit: residual headroom against the
// 1.5 uT acceptance limit times the weakest-direction observability (full
// marks from 0.15, which a full turn plus about +/-25 deg of nod and tilt
// reaches). Not an acceptance gate.
float mag_fit_quality(const MagFitResult& fit);

// Eigen-decomposition of a symmetric 3x3 matrix (cyclic Jacobi): values in
// ascending order, vectors[k] the unit eigenvector of values[k].
void symmetric3_eigen(const double a[3][3], double values[3], double vectors[3][3]);

// Linear least-squares hard-iron fit. The earth field is constant in the world
// frame, so with R_t the gyro-integrated package attitude:
//   R_t * P * (m_t - c) = b   ->   [R_t P, -I] [c; b] = R_t P m_t
// Six unknowns, exact linear system; the gyro makes it well-conditioned even
// for partial rotations (a sphere fit on the same data is not). The gyro bias
// must already be removed from `samples` (gyro_degs). Samples must be one
// contiguous recording: a gap breaks the attitude integration.
MagFitResult fit_mag_hard_iron(const std::vector<ImuSample>& samples, const Vec3& gyro_bias_degs);

// Online hard-iron tracking (opt-in, see Config::enabled). The stored offset
// goes stale whenever a magnet that moves WITH the head changes: in-ear
// earbuds inserted or removed (driver and case-retention magnets a few cm
// from the temple), or a phone held to the ear. Such a magnet is a dipole,
// |B| = (mu0 / 4 pi) m / r^3 (x2 on its axis): m ~ 0.02-0.05 A m^2 at 8-15 cm
// gives roughly 1-10 uT at the sensor (mag_heading_selftest models it). That
// is a step in the offset c. Replayed on the 2026-09-25 worn session
// (imu_replay --inject-hard-iron), a 3 uT step drags the whole workspace by
// up to ~7 deg rms with the lock alone: its reference heading predates the
// step, the field's magnitude and dip barely change (both gates pass), and it
// pulls the view onto the shifted heading.
//
// A free per-window re-fit of c is deliberately NOT used: in desk wear the
// head covers too few orientations (observability 0.02-0.2) and on that
// session the re-fit chased +/-2-3 uT of model noise and moved the view
// 3.1 deg rms with nothing changed. Instead:
//
// 1. Step detection. Two rotation-compensated estimates of the field in the
//    package frame (the gyro rotates them as the head turns, so an earth-fixed
//    field stays put): fast (tau 0.15 s) and slow (tau 2 s). Once they differ
//    by step_watch_ut the slow one freezes and the lock holds (no correction,
//    so a suspect field cannot pull the view). Falling back below step_off_ut
//    is a transient (a magnet carried past); a difference that stays stable
//    for step_confirm_s and is at least step_on_ut is a step; anything else
//    is handed back to the lock's own disturbance gates.
// 2. Provisional adoption. A step from an undisturbed, plausible field is
//    folded into c at once, so the earth-frame field - and the heading the
//    lock sees - is continuous across it: the lock's reference stays valid.
// 3. Verification. A step is head-fixed (keep) or earth-fixed (a magnet put
//    down near the desk: revert). Rotation separates them. On staggered
//    windows of the gyro-constrained model M_t (m_t - c) = b, the offset is
//    fitted along the step direction only (one degree of freedom: c_before +
//    a h): a = 1 head-fixed as measured, a = 0 earth-fixed. Once enough
//    rotation evidence has accumulated, a < revert_below_scale reverts and
//    otherwise c becomes c_before + a h (which also corrects the step's
//    measured size).
// Steps and the departure from the stored calibration are bounded; windows
// whose field is not constant (residual) or of foreign magnitude are skipped.
class MagHardIronTracker {
public:
    struct Config {
        // Off by default: proven on synthetic dipole steps and on injected
        // steps replayed through a recorded session, not yet on live
        // glasses with real earbuds.
        bool enabled = false;
        // Step detector.
        float fast_tau_s = 0.15f;
        float slow_tau_s = 2.0f;
        // Watch: the slow estimate freezes (and the lock holds) as soon as
        // the difference passes step_watch_ut, so the slow estimate cannot
        // absorb the rising edge of a step; below step_off_ut the watch ends
        // as a transient; a settled difference of at least step_on_ut is a
        // step.
        float step_watch_ut = 1.2f;
        float step_on_ut = 2.0f;
        float step_off_ut = 0.8f;
        // Long enough that a magnet carried past (a phone, a hand with a
        // watch) is over before it counts; the lock holds while a step is
        // being confirmed, so waiting costs no view motion.
        float step_confirm_s = 10.0f;
        // A difference that never settles for this long is not a step.
        float max_armed_s = 30.0f;
        // The step estimate must stay this stable (fast-estimate scatter)
        // during confirmation.
        float step_stability_ut = 1.0f;
        // Above this head rate the detector neither confirms nor learns
        // (mag/gyro latency, and any residual offset error, grow with rate).
        float max_rate_degs = 60.0f;
        // Only earbud-sized head-fixed steps from an undisturbed field are
        // candidates: |step| bound, and the field magnitude before / after
        // the step relative to the reference. Larger or foreign-field steps
        // stay with the lock's disturbance gates.
        float max_step_ut = 8.0f;
        float pre_step_field_tolerance = 0.08f;
        float post_step_field_tolerance = 0.12f;
        // Hypothesis test windows.
        float window_s = 15.0f;
        float hop_s = 5.0f;
        float min_fill = 0.5f;  // of window_s at 50 Hz fresh rows
        float max_residual_ut = 1.5f;
        float field_tolerance = 0.12f;
        // Evidence (sum of h^T S h over windows, uT^2) needed to decide; the
        // least-squares scale a along the step (1 = head-fixed as measured,
        // 0 = earth-fixed) is shrunk toward 1 by scale_prior_ut2, clamped to
        // [0, max_scale], and below revert_below_scale the step is reverted.
        float decide_separation_ut2 = 3.0f;
        float scale_prior_ut2 = 0.5f;
        float max_scale = 1.5f;
        float revert_below_scale = 0.35f;
        // A pending hypothesis is dropped after this long without a decision.
        float hypothesis_timeout_s = 600.0f;
        // Bound on |c - stored calibration|.
        float max_offset_ut = 15.0f;
    };

    enum class Event : int { none = 0, step = 1, adopted = 2, rejected = 3, expired = 4 };

    void configure(const Config& cfg, const Vec3& calibrated_hard_iron_ut);
    // Clears all state and returns to the stored calibration.
    void reset();

    // gyro_bias_degs: the estimator's current bias (package frame).
    // reference_field_ut: expected |B| (0 = unknown, gate skipped).
    // Returns the event this sample produced (step / adopted need the lock to
    // re-reference).
    Event add_sample(const ImuSample& sample, const Vec3& gyro_bias_degs, float reference_field_ut);

    const Vec3& hard_iron_ut() const { return hard_iron_; }
    Vec3 offset_from_calibration_ut() const;
    bool hypothesis_pending() const { return have_hypothesis_; }
    // A field step is being confirmed: the lock must not correct meanwhile.
    bool step_pending() const { return step_armed_; }
    // |fast - slow| field estimate difference (uT): the detector's input.
    float step_signal_ut() const {
        const double dx = fast_[0] - slow_[0];
        const double dy = fast_[1] - slow_[1];
        const double dz = fast_[2] - slow_[2];
        return static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    const Vec3& hypothesis_ut() const { return hypothesis_; }
    // Accumulated evidence of the pending hypothesis and its current
    // least-squares scale; last_scale() is the scale of the last decision.
    float evidence_separation_ut2() const { return static_cast<float>(evidence_sep_); }
    float hypothesis_scale() const;
    float last_scale() const { return last_scale_; }
    // Last detected step (raw mag axes).
    const Vec3& last_step_ut() const { return last_step_; }
    uint32_t steps_detected() const { return steps_; }
    uint32_t transients_rejected() const { return transients_; }
    uint32_t foreign_steps() const { return foreign_steps_; }
    uint32_t hypotheses_adopted() const { return adopted_; }
    uint32_t hypotheses_rejected() const { return rejected_; }
    uint32_t windows_evaluated() const { return windows_used_; }
    const Config& config() const { return cfg_; }

private:
    struct Window {
        bool active = false;
        double elapsed_s = 0.0;
        double att[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        double m_sum[3][3] = {};
        double mty[3] = {};
        double y_sum[3] = {};
        double yy = 0.0;
        int rows = 0;
    };

    void reset_windows();
    void revert_hypothesis();
    void close_window(const Window& w, float reference_field_ut);
    Event decide();

    Config cfg_;
    Vec3 calibration_;
    Vec3 hard_iron_;
    bool have_tick_ = false;
    uint32_t last_tick_ = 0;
    Vec3 last_mag_;
    bool have_mag_ = false;

    // Step detector state (package frame, offset removed).
    bool have_estimates_ = false;
    double since_fresh_s_ = 0.0;
    double fast_[3] = {};
    double slow_[3] = {};
    bool step_armed_ = false;
    double step_time_s_ = 0.0;
    double armed_total_s_ = 0.0;
    double step_ref_[3] = {};   // the jump at the start of confirmation
    Vec3 last_step_;
    float last_scale_ = 1.0f;

    // Hypothesis test.
    static constexpr int kMaxWindows = 4;
    Window windows_[kMaxWindows];
    double since_start_s_ = 0.0;
    bool have_hypothesis_ = false;
    Vec3 hypothesis_;
    double hypothesis_age_s_ = 0.0;
    double evidence_sep_ = 0.0;
    double evidence_gain_ = 0.0;

    uint32_t steps_ = 0;
    uint32_t transients_ = 0;
    uint32_t foreign_steps_ = 0;
    uint32_t adopted_ = 0;
    uint32_t rejected_ = 0;
    uint32_t windows_used_ = 0;
};

// Magnetometer heading lock: a slow, disturbance-gated PI loop that pins the
// fused orientation's yaw (rotation about earth Z) to the local magnetic field
// direction. It never touches pitch/roll (the Madgwick accelerometer term owns
// those), never snaps, and is rate-limited, so a transient field disturbance
// can at worst produce a slow bounded slide, never a jump. Without it yaw is
// the open-loop integral of the gyro and any residual bias error accumulates
// without bound (the workspace drift).
class MagHeadingLock {
public:
    struct Config {
        // Proportional time constant: a heading error decays with this tau.
        // 20 -> 10 s after the 2026-09-25 worn session (imu_replay, 10 s mean
        // of the lock error): lag rms 1.07 -> 0.59 deg, p95 1.77 -> 0.86 deg
        // (the "1-2 deg after looking left" the wearer saw), with unchanged
        // reading jitter (31.0 px/s either way). 6 s halves it again but
        // follows any in-gate field distortion twice as fast.
        float tau_s = 10.0f;
        // Integral term (removes the steady lag a residual gyro yaw bias
        // would otherwise leave). Ki = scale * Kp^2 / 4: at 1.0 the loop is
        // critically damped. 0.5 at tau 10 (mag_heading_selftest): a 0.1
        // deg/s stale bias peaks at 0.81 deg and ends at 0.001 deg after
        // 10 min; a 5 deg step overshoots by 0.18 deg.
        float integral_gain_scale = 0.5f;
        float integral_limit_degs = 0.15f;
        // Hard cap on the applied correction rate; far below perception of a
        // pan, well above any realistic gyro bias error.
        float max_correction_degs = 0.5f;
        // Reference acquisition: average this much accepted data before
        // locking (the reference heading is the current estimate, so locking
        // never moves the view).
        float acquire_s = 2.0f;
        // Disturbance gates relative to the acquired reference field.
        float field_tolerance = 0.08f;   // fraction of |B|
        float dip_tolerance_deg = 4.0f;
        float min_horizontal_ut = 5.0f;
        // Above this head rate the mag/gyro latency mismatch dominates.
        float max_turn_rate_degs = 90.0f;
        // A field that is consistently different for this long (a new room, a
        // moved desk) is re-acquired as the new reference instead of being
        // rejected forever.
        float reacquire_s = 8.0f;
        float reacquire_stability_deg = 2.0f;
        // Slow tracking of the reference magnitude/dip while locked.
        float reference_tau_s = 120.0f;
        // An error this large while the field passes every gate means the
        // reference is stale; re-acquire rather than slide.
        float max_plausible_error_deg = 25.0f;
        // Diagnostic: measure the heading error but apply no correction.
        bool observe_only = false;
        // Online hard-iron tracking (see MagHardIronTracker).
        MagHardIronTracker::Config hard_iron;
    };

    // relearning: locked, while the hard-iron tracker tests whether a
    // detected field step moved with the head (see MagHardIronTracker).
    enum class State : int { off = 0, acquiring = 1, locked = 2, disturbed = 3, relearning = 4 };

    void configure(const Config& cfg);
    void reset();

    // Feeds the online hard-iron tracker with one raw sample and returns the
    // hard-iron offset to use for it (the stored calibration when tracking is
    // disabled). Call before update() for the same sample.
    Vec3 track_hard_iron(const ImuSample& sample, const Vec3& gyro_bias_degs, const MagCalibration& calibration);
    const MagHardIronTracker& hard_iron_tracker() const { return tracker_; }
    // Head-fixed field steps folded into the offset (diagnostic).
    uint32_t hard_iron_steps() const { return rebases_; }

    // earth_field_ut: calibrated field rotated into the earth frame by the
    // current orientation estimate. fresh: the mag reading changed since the
    // previous sample (the magnetometer updates at ~51 Hz, the gyro at 476).
    // Returns the yaw correction to apply this sample, in radians about +Z
    // (earth frame, premultiplied onto the orientation).
    float update(const Vec3& earth_field_ut, bool fresh, float head_rate_degs, float dt);

    State state() const {
        return state_ == State::locked && (tracker_.hypothesis_pending() || tracker_.step_pending())
                   ? State::relearning
                   : state_;
    }
    float error_deg() const { return error_deg_; }
    float integral_degs() const { return integral_degs_; }
    float field_ut() const { return field_ut_; }
    float dip_deg() const { return dip_deg_; }
    float reference_field_ut() const { return ref_field_ut_; }
    float reference_dip_deg() const { return ref_dip_deg_; }
    uint32_t reacquisitions() const { return reacquisitions_; }
    // Accumulated correction since reset (deg); diagnostic.
    float total_correction_deg() const { return total_correction_deg_; }

private:
    void begin_acquire();

    Config cfg_;
    State state_ = State::off;

    // Acquisition accumulators.
    float acq_time_s_ = 0.0f;
    float acq_hx_ = 0.0f;
    float acq_hy_ = 0.0f;
    float acq_field_ = 0.0f;
    float acq_dip_ = 0.0f;
    int acq_n_ = 0;

    float ref_heading_rad_ = 0.0f;
    float ref_field_ut_ = 0.0f;
    float ref_dip_deg_ = 0.0f;

    // Disturbance tracking for re-acquisition.
    float rejected_s_ = 0.0f;
    float rej_field_ = 0.0f;
    float rej_dip_ = 0.0f;
    float rej_heading_ = 0.0f;
    bool rej_stable_ = false;

    float error_deg_ = 0.0f;
    bool have_error_ = false;
    float integral_degs_ = 0.0f;
    float field_ut_ = 0.0f;
    float dip_deg_ = 0.0f;
    float since_fresh_s_ = 0.0f;
    uint32_t reacquisitions_ = 0;
    float total_correction_deg_ = 0.0f;

    MagHardIronTracker tracker_;
    bool tracker_ready_ = false;
    uint32_t rebases_ = 0;
};

// Lower-case name of a lock state for status files and logs
// ("off", "acquiring", "locked", "disturbed", "relearning").
const char* mag_state_name(MagHeadingLock::State state);

}  // namespace gt
