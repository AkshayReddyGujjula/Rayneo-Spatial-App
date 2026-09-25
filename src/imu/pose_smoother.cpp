#include "imu/pose_smoother.h"

#include <algorithm>
#include <cmath>

namespace gt {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float lowpass_alpha(float cutoff_hz, float dt_s) {
    if (!(cutoff_hz > 0.0f) || !(dt_s > 0.0f)) {
        return 1.0f;
    }
    const float tau = 1.0f / (2.0f * kPi * cutoff_hz);
    return 1.0f / (1.0f + tau / dt_s);
}

float quat_dot(const Quat& a, const Quat& b) {
    return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
}



}  // namespace

// atan2 of the vector part keeps full precision for small angles; 2 * acos(w)
// in float cannot resolve anything below ~0.04 deg (w rounds to 1), which
// quantised the 1-euro speed estimate in 2.4 deg/s steps at 60 Hz.
float quat_angle_deg(const Quat& q) {
    const float v = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    return 2.0f * std::atan2(v, std::fabs(q.w)) * 180.0f / kPi;
}

const char* reading_hold_name(int level) {
    static const char* const kNames[kReadingHoldLevels] = {"off", "low", "medium", "high"};
    return kNames[std::clamp(level, 0, kReadingHoldLevels - 1)];
}

void apply_reading_hold(int level, PoseSmoother::Config& config) {
    // inner, outer (deg), settle tau (s)
    static const float kPresets[kReadingHoldLevels][3] = {
        {0.0f, 0.0f, 0.0f},
        {0.1f, 0.25f, 1.5f},
        {0.2f, 0.45f, 3.0f},
        {0.3f, 0.6f, 4.0f},
    };
    const float* p = kPresets[std::clamp(level, 0, kReadingHoldLevels - 1)];
    config.hold_inner_deg = p[0];
    config.hold_outer_deg = p[1];
    config.hold_settle_tau_s = p[2];
}

void PoseSmoother::reset() {
    smoothed_ = Quat{1.0f, 0.0f, 0.0f, 0.0f};
    held_ = smoothed_;
    speed_degs_ = 0.0f;
    have_sample_ = false;
}

// Soft hysteresis. With off = conj(held) * target of angle d, the new held
// pose keeps the same axis at angle e(d): e = d inside the inner radius (the
// view does not move), then R_in + (R_out - R_in) * tanh((d - R_in) / (R_out -
// R_in)), whose slope is 1 at R_in (no kink) and which saturates at R_out, so a
// turn is followed 1:1 with a bounded trailing offset. It depends only on
// positions, never on the frame rate; only the settle decay uses dt.
Quat PoseSmoother::apply_hold(const Quat& target, float dt_s) {
    const float inner = config_.hold_inner_deg;
    const float outer = config_.hold_outer_deg;
    if (!(inner > 0.0f) || !(outer > inner)) {
        held_ = target;
        return held_;
    }
    Quat off = quat_multiply(quat_conjugate(held_), target);
    if (off.w < 0.0f) {
        off = Quat{-off.w, -off.x, -off.y, -off.z};
    }
    const float d = quat_angle_deg(off);
    if (d < 1e-6f) {
        return held_;
    }
    float e = d;
    if (d > inner) {
        e = inner + (outer - inner) * std::tanh((d - inner) / (outer - inner));
    }
    if (config_.hold_settle_tau_s > 0.0f) {
        e *= std::exp(-dt_s / config_.hold_settle_tau_s);
    }
    held_ = quat_normalize(quat_multiply(target, quat_conjugate(quat_scaled(off, e / d))));
    return held_;
}

Quat PoseSmoother::update(const Quat& raw, float dt_s) {
    const Quat input = quat_normalize(raw);
    if (!have_sample_ || !(dt_s > 0.0f) || dt_s > 0.1f) {
        smoothed_ = input;
        held_ = input;
        speed_degs_ = 0.0f;
        have_sample_ = true;
        return smoothed_;
    }
    Quat target = input;
    if (quat_dot(smoothed_, target) < 0.0f) {
        target.w = -target.w;
        target.x = -target.x;
        target.y = -target.y;
        target.z = -target.z;
    }
    const Quat delta = quat_multiply(quat_conjugate(smoothed_), target);
    const float angle_degs = quat_angle_deg(delta);
    const float speed = angle_degs / dt_s;
    const float speed_alpha = lowpass_alpha(config_.derivative_cutoff_hz, dt_s);
    speed_degs_ += speed_alpha * (speed - speed_degs_);
    const float cutoff = config_.min_cutoff_hz + config_.beta * speed_degs_;
    const float alpha = lowpass_alpha(cutoff, dt_s);
    smoothed_ = quat_normalize(quat_multiply(smoothed_, quat_scaled(delta, alpha)));
    return apply_hold(smoothed_, dt_s);
}

}  // namespace gt
