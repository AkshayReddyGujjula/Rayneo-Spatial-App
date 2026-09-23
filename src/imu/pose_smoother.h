#pragma once

#include "imu/fusion.h"

namespace gt {

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
    };

    void configure(const Config& config) { config_ = config; }
    void reset();

    // Returns the smoothed pose. dt_s <= 0 or > 0.1 snaps to raw (gap or
    // discontinuity): call reset() explicitly on recenter.
    Quat update(const Quat& raw, float dt_s);

private:
    Config config_;
    Quat smoothed_{1.0f, 0.0f, 0.0f, 0.0f};
    float speed_degs_ = 0.0f;
    bool have_sample_ = false;
};

}  // namespace gt
