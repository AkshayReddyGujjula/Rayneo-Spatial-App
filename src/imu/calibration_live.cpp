#include "imu/calibration_live.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace gt {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kStillWindowSeconds = 1.5f;
constexpr float kSpeedTauSeconds = 0.1f;
// Display-only gesture counting, mirroring the repeat splitter's return rule
// (a gesture ends once it is back within 30% of its peak).
constexpr float kReturnFraction = 0.3f;
// Hint thresholds (guidance only; none of these are acceptance gates).
constexpr float kSteadyRmsDegs = 1.0f;     // two thirds of the 1.5 deg/s still gate
constexpr float kSlowerDegs = 80.0f;       // a smooth head turn stays well below this
constexpr float kOffAxisFloorDeg = 2.5f;
constexpr float kOffAxisFraction = 0.12f;  // ~7 deg of axis tilt per gesture
constexpr float kWrongWayDeg = 4.0f;
constexpr float kStartedDeg = 4.0f;
constexpr float kReturningDeg = 1.5f;
constexpr float kTargetSlackDeg = 0.5f;  // within half a degree counts as reaching the target

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 scale(const Vec3& v, float s) {
    return Vec3{v.x * s, v.y * s, v.z * s};
}
Vec3 subtract(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float norm(const Vec3& v) {
    return std::sqrt(dot(v, v));
}
bool finite(float v) {
    return std::isfinite(v);
}
float finite_or_zero(float v) {
    return finite(v) ? v : 0.0f;
}

float sample_dt(const ImuSample& previous, const ImuSample& current) {
    const uint32_t delta = current.tick_100us - previous.tick_100us;
    const float dt = static_cast<float>(delta) * 1e-4f;
    return dt > 1e-5f && dt < 0.05f ? dt : 1.0f / 476.0f;
}

void append_number(std::string& out, const char* key, float value, const char* format) {
    out += ",\"";
    out += key;
    out += "\":";
    char number[32];
    std::snprintf(number, sizeof(number), format, static_cast<double>(finite_or_zero(value)));
    // "-0.00" reads oddly in a guide; print plain zero.
    if (std::strcmp(number, "-0.00") == 0 || std::strcmp(number, "-0.0") == 0) {
        std::snprintf(number, sizeof(number), format, 0.0);
    }
    out += number;
}

}  // namespace

float live_target_deg(LiveStepKind kind) {
    switch (kind) {
        case LiveStepKind::Turn:
            return 25.0f;
        case LiveStepKind::Nod:
        case LiveStepKind::Tilt:
            return 20.0f;
        case LiveStepKind::Still:
            break;
    }
    return 0.0f;
}

const char* live_step_kind_name(LiveStepKind kind) {
    switch (kind) {
        case LiveStepKind::Still:
            return "still";
        case LiveStepKind::Turn:
            return "turn";
        case LiveStepKind::Nod:
            return "nod";
        case LiveStepKind::Tilt:
            return "tilt";
    }
    return "still";
}

Vec3 live_package_to_body(const Vec3& package) {
    return Vec3{package.x, -package.z, package.y};
}

Vec3 live_nominal_forward_body() {
    return live_package_to_body(Vec3{0.0f, 1.0f, 0.0f});
}

Vec3 live_forward_from_sensor_to_head(const std::array<float, 9>& m) {
    return live_package_to_body(Vec3{m[3], m[4], m[5]});
}

CalibrationLiveFrame make_live_frame(const Vec3& up_raw, const Vec3& forward_hint, const char* source) {
    CalibrationLiveFrame frame;
    frame.source = source != nullptr ? source : "nominal";
    const float up_length = norm(up_raw);
    if (!(up_length > 1e-6f) || !finite(up_length)) {
        return frame;
    }
    const Vec3 up = scale(up_raw, 1.0f / up_length);
    auto horizontal = [&](const Vec3& hint) {
        return subtract(hint, scale(up, dot(hint, up)));
    };
    Vec3 forward = horizontal(forward_hint);
    if (norm(forward) < 0.2f * std::max(norm(forward_hint), 1e-6f)) {
        // Hint nearly vertical: fall back to the nominal mounting, then to any
        // horizontal direction.
        forward = horizontal(live_nominal_forward_body());
        if (norm(forward) < 0.2f) {
            forward = horizontal(Vec3{1.0f, 0.0f, 0.0f});
        }
        if (frame.source == "file" || frame.source == "measured") {
            frame.source = "nominal";
        }
    }
    forward = scale(forward, 1.0f / norm(forward));
    // right x forward = up, so right = forward x up.
    Vec3 right = cross(forward, up);
    right = scale(right, 1.0f / norm(right));
    frame.up_body = up;
    frame.right_body = right;
    frame.forward_body = cross(up, right);
    frame.valid = true;
    return frame;
}

std::string format_calibration_live(const CalibrationLiveLine& line) {
    std::string out = kLivePrefix;
    out += "{\"step\":" + std::to_string(line.step);
    out += ",\"kind\":\"";
    out += live_step_kind_name(line.kind);
    out += "\",\"state\":\"";
    out += line.state == "waiting" ? "waiting" : "recording";
    out += '"';
    append_number(out, "elapsed", line.elapsed_s, "%.2f");
    append_number(out, "remaining", line.remaining_s < 0.0f ? -1.0f : line.remaining_s, "%.2f");
    out += ",\"frame\":\"";
    // The frame source is one of four fixed words; anything else is reported
    // as nominal so the line never needs escaping.
    const std::string& f = line.frame;
    out += (f == "file" || f == "measured" || f == "gravity") ? f : std::string("nominal");
    out += '"';
    append_number(out, "yaw", line.yaw_deg, "%.2f");
    append_number(out, "pitch", line.pitch_deg, "%.2f");
    append_number(out, "roll", line.roll_deg, "%.2f");
    append_number(out, "along", line.along_deg, "%.2f");
    append_number(out, "off", line.off_deg, "%.2f");
    append_number(out, "speed", line.speed_degs, "%.1f");
    append_number(out, "still_rms", line.still_rms_degs, "%.2f");
    append_number(out, "still_gate", kLiveStillGateDegs, "%.1f");
    append_number(out, "gate", kLiveExcursionGateDeg, "%.0f");
    append_number(out, "target", live_target_deg(line.kind), "%.0f");
    append_number(out, "peak", line.peak_deg, "%.2f");
    out += ",\"repeats\":" + std::to_string(line.repeats_done);
    out += ",\"repeats_required\":" + std::to_string(line.repeats_required);
    out += ",\"hint\":\"";
    for (char c : line.hint) {
        // Hint codes are upper-case words with underscores.
        if ((c >= 'A' && c <= 'Z') || c == '_') {
            out += c;
        }
    }
    out += "\"}";
    return out;
}

const char* live_hint(LiveStepKind kind, const std::string& state, float along, float peak, float off, float speed,
                      float still_rms, int repeats_done, int repeats_required) {
    if (state == "waiting") {
        return "GET_READY";
    }
    if (kind == LiveStepKind::Still) {
        return still_rms < kSteadyRmsDegs ? "STEADY" : "HOLD";
    }
    if (speed > kSlowerDegs) {
        return "SLOWER";
    }
    const float reach = std::max(std::fabs(along), peak);
    if (off > kOffAxisFloorDeg && off > kOffAxisFraction * reach) {
        return "OFF_AXIS";
    }
    if (along < -kWrongWayDeg) {
        return "WRONG_WAY";
    }
    if (repeats_required > 0 && repeats_done >= repeats_required && peak < kLiveExcursionGateDeg) {
        return "DONE";
    }
    if (peak < kStartedDeg) {
        return "GO";
    }
    const bool returning = along < peak - kReturningDeg;
    if (!returning) {
        return peak >= live_target_deg(kind) - kTargetSlackDeg ? "BACK_TO_CENTRE" : "FURTHER";
    }
    return peak >= kLiveExcursionGateDeg ? "BACK_TO_CENTRE" : "FURTHER";
}

void CalibrationLiveTracker::begin(LiveStepKind kind, int step, const char* state, int repeats_required,
                                   const Vec3& bias_body, const CalibrationLiveFrame& frame) {
    kind_ = kind;
    step_ = step;
    state_ = state != nullptr ? state : "recording";
    repeats_required_ = kind == LiveStepKind::Still ? 0 : std::max(1, repeats_required);
    bias_body_ = bias_body;
    frame_ = frame;
    have_previous_ = false;
    qw_ = 1.0;
    qx_ = qy_ = qz_ = 0.0;
    speed_degs_ = 0.0f;
    peak_deg_ = 0.0f;
    repeats_done_ = 0;
    returning_ = false;
    valley_deg_ = 0.0f;
    gravity_sum_ = Vec3{};
    gravity_count_ = 0;
    window_.clear();
    window_seconds_ = 0.0;
    sum_x_ = sum_y_ = sum_z_ = sum_sq_ = 0.0;
}

void CalibrationLiveTracker::add(const ImuSample& sample) {
    const Vec3 gyro = live_package_to_body(sample.gyro_degs);
    const Vec3 accel = live_package_to_body(sample.accel_mps2);
    if (!finite(gyro.x) || !finite(gyro.y) || !finite(gyro.z) || !finite(accel.x) || !finite(accel.y) ||
        !finite(accel.z)) {
        return;
    }
    gravity_sum_ = Vec3{gravity_sum_.x + accel.x, gravity_sum_.y + accel.y, gravity_sum_.z + accel.z};
    ++gravity_count_;
    if (!have_previous_) {
        previous_ = sample;
        have_previous_ = true;
        return;
    }
    const float dt = sample_dt(previous_, sample);
    previous_ = sample;

    // Stillness: rms deviation of the raw gyro vector over the trailing
    // window (the same statistic as the still step's quietest window).
    window_.push_back(WindowSample{dt, gyro});
    window_seconds_ += dt;
    sum_x_ += gyro.x;
    sum_y_ += gyro.y;
    sum_z_ += gyro.z;
    sum_sq_ += static_cast<double>(dot(gyro, gyro));
    while (window_.size() > 1 && window_seconds_ - window_.front().dt >= kStillWindowSeconds) {
        const WindowSample& old = window_.front();
        window_seconds_ -= old.dt;
        sum_x_ -= old.gyro.x;
        sum_y_ -= old.gyro.y;
        sum_z_ -= old.gyro.z;
        sum_sq_ -= static_cast<double>(dot(old.gyro, old.gyro));
        window_.pop_front();
    }

    const Vec3 rate = subtract(gyro, bias_body_);
    const float rate_norm = norm(rate);
    const float alpha = dt / (kSpeedTauSeconds + dt);
    speed_degs_ += alpha * (rate_norm - speed_degs_);

    // q <- q * exp(omega dt / 2), omega in the current body frame.
    const double angle = static_cast<double>(rate_norm) * kPi / 180.0 * static_cast<double>(dt);
    if (angle > 0.0) {
        const double half = 0.5 * angle;
        const double s = std::sin(half) / static_cast<double>(rate_norm);
        const double dw = std::cos(half);
        const double dx = rate.x * s;
        const double dy = rate.y * s;
        const double dz = rate.z * s;
        const double w = qw_ * dw - qx_ * dx - qy_ * dy - qz_ * dz;
        const double x = qw_ * dx + qx_ * dw + qy_ * dz - qz_ * dy;
        const double y = qw_ * dy - qx_ * dz + qy_ * dw + qz_ * dx;
        const double z = qw_ * dz + qx_ * dy - qy_ * dx + qz_ * dw;
        const double length = std::sqrt(w * w + x * x + y * y + z * z);
        qw_ = w / length;
        qx_ = x / length;
        qy_ = y / length;
        qz_ = z / length;
    }

    if (kind_ != LiveStepKind::Still && state_ == "recording") {
        const CalibrationLiveLine now = snapshot(0.0f, -1.0f);
        if (returning_) {
            // Follow the return down until the head starts out again, so the
            // next gesture's peak starts from where this one ended.
            valley_deg_ = std::min(valley_deg_, now.along_deg);
            peak_deg_ = std::max(0.0f, now.along_deg);
            if (now.along_deg > valley_deg_ + kReturningDeg) {
                returning_ = false;
            }
        } else {
            peak_deg_ = std::max(peak_deg_, now.along_deg);
            if (peak_deg_ >= kLiveExcursionGateDeg && now.along_deg <= kReturnFraction * peak_deg_) {
                ++repeats_done_;
                returning_ = true;
                valley_deg_ = now.along_deg;
                peak_deg_ = std::max(0.0f, now.along_deg);
            }
        }
    }
}

Vec3 CalibrationLiveTracker::rotation_vector_body_deg() const {
    double w = qw_, x = qx_, y = qy_, z = qz_;
    if (w < 0.0) {
        w = -w;
        x = -x;
        y = -y;
        z = -z;
    }
    const double v = std::sqrt(x * x + y * y + z * z);
    if (v < 1e-12) {
        return Vec3{};
    }
    const double angle_deg = 2.0 * std::atan2(v, w) * 180.0 / kPi;
    const double k = angle_deg / v;
    return Vec3{static_cast<float>(x * k), static_cast<float>(y * k), static_cast<float>(z * k)};
}

CalibrationLiveLine CalibrationLiveTracker::snapshot(float elapsed_s, float remaining_s) const {
    CalibrationLiveLine line;
    line.step = step_;
    line.kind = kind_;
    line.state = state_;
    line.elapsed_s = elapsed_s;
    line.remaining_s = remaining_s;
    line.repeats_done = repeats_done_;
    line.repeats_required = repeats_required_;
    line.speed_degs = speed_degs_;
    line.peak_deg = kind_ == LiveStepKind::Still ? 0.0f : peak_deg_;

    CalibrationLiveFrame frame = frame_;
    if (!frame.valid) {
        const Vec3 up = gravity_count_ > 0 ? scale(gravity_sum_, 1.0f / static_cast<float>(gravity_count_))
                                           : live_package_to_body(Vec3{0.0f, 0.0f, 1.0f});
        frame = make_live_frame(up, live_nominal_forward_body(), "gravity");
        if (gravity_count_ == 0) {
            frame.source = "nominal";
        }
    }
    line.frame = frame.source;

    const Vec3 r = rotation_vector_body_deg();
    line.yaw_deg = dot(r, frame.up_body);
    line.pitch_deg = dot(r, frame.right_body);
    line.roll_deg = dot(r, frame.forward_body);
    const float total = std::sqrt(line.yaw_deg * line.yaw_deg + line.pitch_deg * line.pitch_deg +
                                  line.roll_deg * line.roll_deg);
    switch (kind_) {
        case LiveStepKind::Turn:
            line.along_deg = line.yaw_deg;
            break;
        case LiveStepKind::Nod:
            line.along_deg = -line.pitch_deg;  // nodding down is the commanded direction
            break;
        case LiveStepKind::Tilt:
            line.along_deg = line.roll_deg;
            break;
        case LiveStepKind::Still:
            line.along_deg = 0.0f;
            break;
    }
    line.off_deg = std::sqrt(std::max(0.0f, total * total - line.along_deg * line.along_deg));

    if (window_.size() >= 2) {
        const double n = static_cast<double>(window_.size());
        const double mx = sum_x_ / n;
        const double my = sum_y_ / n;
        const double mz = sum_z_ / n;
        const double variance = std::max(0.0, sum_sq_ / n - (mx * mx + my * my + mz * mz));
        line.still_rms_degs = static_cast<float>(std::sqrt(variance));
    }
    line.hint = live_hint(kind_, state_, line.along_deg, line.peak_deg, line.off_deg, line.speed_degs,
                          line.still_rms_degs, repeats_done_, repeats_required_);
    return line;
}

}  // namespace gt
