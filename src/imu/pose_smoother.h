#pragma once

#include "imu/fusion.h"

#include <cstdint>

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
        // Motion-to-photon prediction (applied to the output, after the
        // 1-euro and the hold): the shown pose is extrapolated by predict_s
        // along the head's raw angular velocity (frame-to-frame, low-passed
        // at predict_cutoff_hz). The
        // extrapolation fades in between predict_gate_lo_degs and
        // predict_gate_hi_degs of head speed, so it is exactly zero while
        // reading or still (no added jitter) and full during turns.
        // predict_s <= 0 disables it. 35 ms: on the 2026-09-25 worn session
        // (imu_replay --smooth-eval --latency L) it cuts the turning error
        // against the pose L later by 49-65% (rms 3.72 -> 1.91, 2.92 -> 1.19,
        // 2.44 -> 0.86 deg at L = 40/25/15 ms, medium stabilisation; the
        // 1-euro and reading hold add lag of their own, so the best horizon
        // is longer than the display latency) with every reading metric
        // unchanged to the printed digit.
        float predict_s = 0.035f;
        float predict_cutoff_hz = 8.0f;
        float predict_gate_lo_degs = 10.0f;
        float predict_gate_hi_degs = 30.0f;
    };

    void configure(const Config& config) { config_ = config; }
    void reset();

    // Returns the smoothed pose. dt_s <= 0 or > 0.1 snaps to raw (gap or
    // discontinuity): call reset() explicitly on recenter.
    Quat update(const Quat& raw, float dt_s);

    // The next update's input is a deliberate view step, not head motion
    // (yaw or pitch tracking switched on or off): restart the prediction's
    // velocity estimate from that input instead of reading the step as a
    // turn. The 1-euro and the reading hold are untouched.
    void restart_prediction() { restart_prediction_ = true; }

    // The pose source's session epoch for the next update's input
    // (ImuSource::orientation). A new epoch is a new IMU connection, whose
    // estimator publishes from a fresh reference: the view steps from the
    // last pose of the old session (frozen while the glasses reconnected and
    // recalibrated) to straight ahead. That step is not head motion, so the
    // prediction restarts exactly as for restart_prediction(); the 1-euro and
    // the reading hold still ease the step in as before.
    void note_source_epoch(uint32_t epoch) {
        if (have_source_epoch_ && epoch != source_epoch_) {
            restart_prediction_ = true;
        }
        source_epoch_ = epoch;
        have_source_epoch_ = true;
    }

private:
    Quat apply_hold(const Quat& target, float dt_s);
    Quat prediction_lead(const Quat& input, float dt_s);

    Config config_;
    Quat smoothed_{1.0f, 0.0f, 0.0f, 0.0f};
    Quat held_{1.0f, 0.0f, 0.0f, 0.0f};
    float speed_degs_ = 0.0f;
    bool have_sample_ = false;
    // Prediction: previous raw pose and the low-passed body-frame angular
    // velocity (deg/s).
    Quat prev_raw_{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 omega_degs_{};
    bool restart_prediction_ = false;
    // Last source epoch seen (note_source_epoch); kept across reset().
    uint32_t source_epoch_ = 0;
    bool have_source_epoch_ = false;
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
