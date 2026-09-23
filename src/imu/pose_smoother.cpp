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

void PoseSmoother::reset() {
    smoothed_ = Quat{1.0f, 0.0f, 0.0f, 0.0f};
    speed_degs_ = 0.0f;
    have_sample_ = false;
}

Quat PoseSmoother::update(const Quat& raw, float dt_s) {
    const Quat input = quat_normalize(raw);
    if (!have_sample_ || !(dt_s > 0.0f) || dt_s > 0.1f) {
        smoothed_ = input;
        speed_degs_ = 0.0f;
        have_sample_ = true;
        return smoothed_;
    }
    Quat target = input;
    float cosine = quat_dot(smoothed_, target);
    if (cosine < 0.0f) {
        target.w = -target.w;
        target.x = -target.x;
        target.y = -target.y;
        target.z = -target.z;
        cosine = -cosine;
    }
    cosine = std::clamp(cosine, -1.0f, 1.0f);
    const float angle_degs = 2.0f * std::acos(cosine) * 180.0f / kPi;
    const float speed = angle_degs / dt_s;
    const float speed_alpha = lowpass_alpha(config_.derivative_cutoff_hz, dt_s);
    speed_degs_ += speed_alpha * (speed - speed_degs_);
    const float cutoff = config_.min_cutoff_hz + config_.beta * speed_degs_;
    const float alpha = lowpass_alpha(cutoff, dt_s);
    const Quat delta = quat_multiply(quat_conjugate(smoothed_), target);
    smoothed_ = quat_normalize(quat_multiply(smoothed_, quat_scaled(delta, alpha)));
    return smoothed_;
}

}  // namespace gt
