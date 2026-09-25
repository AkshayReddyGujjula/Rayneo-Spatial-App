#pragma once

#include "imu/fusion.h"

namespace gt {

// Rotation angle of a quaternion in degrees, accurate for tiny angles.
float quat_angle_deg(const Quat& q);

// Display-side rotational 1-euro smoothing (Casiez, Roussel, Vogel, CHI 2012)
// for the published head pose. Adaptive cutoff: heavy damping while still, so
// tremor and sensor noise vanish from the view, and vanishing lag during
// deliberate motion. Zero steady-state error: a held pose converges exactly,
// so pinned screens never drift. Deliberately OUTSIDE the estimator: the
// gyro-bias adaptation stays the single owner of steady error, and
// diagnostics and capture keep using the raw pose.
class PoseSmoother {
public:
    struct Config {
        float min_cutoff_hz = 0.6f;
        float beta = 0.05f;  // cutoff rises by beta Hz per deg/s of head speed
        float derivative_cutoff_hz = 1.0f;
        // Reading hold (a soft hysteresis deadband after the 1-euro stage,
        // like camera EIS): the displayed pose stays put until the head
        // pushes past hold_inner_deg, then follows with a trailing offset
        // that saturates smoothly (tanh knee) at hold_outer_deg, so there is
        // no velocity step when a deliberate turn begins. While held, the
        // offset decays with hold_settle_tau_s so the view still converges on
        // the true world-locked pose. hold_inner_deg <= 0 disables the hold.
        float hold_inner_deg = 0.0f;
        float hold_outer_deg = 0.0f;
        float hold_settle_tau_s = 0.0f;
    };

    void configure(const Config& config) { config_ = config; }
    void reset();

    // Returns the smoothed pose. dt_s <= 0 or > 0.1 snaps to raw (gap or
    // discontinuity): call reset() explicitly on recenter.
    Quat update(const Quat& raw, float dt_s);

private:
    Quat apply_hold(const Quat& target, float dt_s);

    Config config_;
    Quat smoothed_{1.0f, 0.0f, 0.0f, 0.0f};
    Quat held_{1.0f, 0.0f, 0.0f, 0.0f};
    float speed_degs_ = 0.0f;
    bool have_sample_ = false;
};

// Reading-stabilisation presets (the engine's --stabilise and Ctrl+Alt+S):
// 0 off, 1 low, 2 medium (default), 3 high, 4 ultra. Measured values are in AGENTS.md
// (imu_replay --smooth-eval on the 2026-09-25 worn session).
inline constexpr int kReadingHoldLevels = 5;
inline constexpr int kReadingHoldDefault = 2;
const char* reading_hold_name(int level);
// Sets only the hold fields of config to the preset for level (clamped).
void apply_reading_hold(int level, PoseSmoother::Config& config);

}  // namespace gt
