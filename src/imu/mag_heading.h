#pragma once

#include "imu/fusion.h"
#include "imu/gt_protocol.h"

#include <array>
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
    const char* code = "";
};

// Linear least-squares hard-iron fit. The earth field is constant in the world
// frame, so with R_t the gyro-integrated package attitude:
//   R_t * P * (m_t - c) = b   ->   [R_t P, -I] [c; b] = R_t P m_t
// Six unknowns, exact linear system; the gyro makes it well-conditioned even
// for partial rotations (a sphere fit on the same data is not). The gyro bias
// must already be removed from `samples` (gyro_degs). Samples must be one
// contiguous recording: a gap breaks the attitude integration.
MagFitResult fit_mag_hard_iron(const std::vector<ImuSample>& samples, const Vec3& gyro_bias_degs);

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
        float tau_s = 20.0f;
        // Integral term (removes the steady lag a residual gyro yaw bias
        // would otherwise leave). Ki = scale * Kp^2 / 4: at 1.0 the loop is
        // critically damped but both modes are slow (tau 40 s). 0.5 (measured
        // in mag_heading_selftest): a 0.1 deg/s stale bias peaks at 1.6 deg and
        // ends at 0.04 deg after 10 min; a 5 deg step settles with ~8%
        // overshoot. 0.25 overshoots less but leaves 0.3 deg lag at 10 min.
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
    };

    enum class State : int { off = 0, acquiring = 1, locked = 2, disturbed = 3 };

    void configure(const Config& cfg);
    void reset();

    // earth_field_ut: calibrated field rotated into the earth frame by the
    // current orientation estimate. fresh: the mag reading changed since the
    // previous sample (the magnetometer updates at ~51 Hz, the gyro at 476).
    // Returns the yaw correction to apply this sample, in radians about +Z
    // (earth frame, premultiplied onto the orientation).
    float update(const Vec3& earth_field_ut, bool fresh, float head_rate_degs, float dt);

    State state() const { return state_; }
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
};

}  // namespace gt
