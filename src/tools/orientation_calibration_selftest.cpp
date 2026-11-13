#include "imu/orientation_calibration.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <process.h>
#include <string>
#include <vector>

namespace {

using gt::CalibrationPhase;
using gt::CalibrationPhaseData;
using gt::ImuSample;
using gt::Vec3;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRateHz = 476.0f;

struct Mat3 {
    float m[3][3]{};
};

Vec3 mat_vec(const Mat3& matrix, const Vec3& value) {
    return Vec3{matrix.m[0][0] * value.x + matrix.m[0][1] * value.y + matrix.m[0][2] * value.z,
                matrix.m[1][0] * value.x + matrix.m[1][1] * value.y + matrix.m[1][2] * value.z,
                matrix.m[2][0] * value.x + matrix.m[2][1] * value.y + matrix.m[2][2] * value.z};
}

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 result;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            for (int k = 0; k < 3; ++k) {
                result.m[row][col] += a.m[row][k] * b.m[k][col];
            }
        }
    }
    return result;
}

Mat3 rotation(float ax, float ay, float az, float degrees) {
    const float length = std::sqrt(ax * ax + ay * ay + az * az);
    ax /= length;
    ay /= length;
    az /= length;
    const float angle = degrees * kPi / 180.0f;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float t = 1.0f - c;
    return Mat3{{{t * ax * ax + c, t * ax * ay - s * az, t * ax * az + s * ay},
                 {t * ax * ay + s * az, t * ay * ay + c, t * ay * az - s * ax},
                 {t * ax * az - s * ay, t * ay * az + s * ax, t * az * az + c}}};
}

Vec3 body_to_package(const Vec3& body) {
    return Vec3{body.x, body.z, -body.y};
}

CalibrationPhaseData make_still(const Mat3& head_to_body, const Vec3& bias) {
    CalibrationPhaseData phase;
    phase.phase = CalibrationPhase::Still;
    const Vec3 accel_package = body_to_package(mat_vec(head_to_body, Vec3{0.0f, 0.0f, 9.81f}));
    const Vec3 gyro_package = body_to_package(bias);
    uint32_t tick = 100000;
    for (int i = 0; i < static_cast<int>(3.0f * kRateHz); ++i) {
        ImuSample sample;
        sample.accel_mps2 = accel_package;
        sample.gyro_degs = gyro_package;
        sample.tick_100us = tick += 21;
        phase.samples.push_back(sample);
    }
    return phase;
}

CalibrationPhaseData make_motion(CalibrationPhase kind, const Mat3& head_to_body,
                                 const Vec3& head_axis, const Vec3& bias, float peak) {
    CalibrationPhaseData phase;
    phase.phase = kind;
    const Vec3 accel_package = body_to_package(mat_vec(head_to_body, Vec3{0.0f, 0.0f, 9.81f}));
    const Vec3 axis_body = mat_vec(head_to_body, head_axis);
    uint32_t tick = 500000 + static_cast<uint32_t>(kind) * 100000;
    const int count = static_cast<int>(4.5f * kRateHz);
    for (int i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / kRateHz;
        float rate = 0.0f;
        if (t >= 0.75f && t < 3.75f) {
            rate = peak * std::sin(2.0f * kPi * (t - 0.75f) / 3.0f);
        }
        const Vec3 gyro_body{axis_body.x * rate + bias.x, axis_body.y * rate + bias.y,
                             axis_body.z * rate + bias.z};
        ImuSample sample;
        sample.accel_mps2 = accel_package;
        sample.gyro_degs = body_to_package(gyro_body);
        sample.tick_100us = tick += 21;
        phase.samples.push_back(sample);
    }
    return phase;
}

// --- Noisy, repeated gestures (deterministic) --------------------------------

// xorshift64* + Box-Muller: identical numbers on every compiler/library.
struct Rng {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1u) {}
    float uniform() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const uint64_t value = state * 0x2545F4914F6CDD1Dull;
        return static_cast<float>((value >> 40) + 1u) / 16777217.0f;  // (0, 1)
    }
    float uniform(float lo, float hi) { return lo + (hi - lo) * uniform(); }
    float gauss() {
        const float u1 = uniform();
        const float u2 = uniform();
        return std::sqrt(-2.0f * std::log(u1)) * std::cos(2.0f * kPi * u2);
    }
};

Vec3 add3(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale3(const Vec3& v, float s) { return Vec3{v.x * s, v.y * s, v.z * s}; }
float dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross3(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 unit3(const Vec3& v) { return scale3(v, 1.0f / std::sqrt(dot3(v, v))); }
float angle3_deg(const Vec3& a, const Vec3& b) {
    return std::atan2(std::sqrt(dot3(cross3(a, b), cross3(a, b))), dot3(a, b)) * 180.0f / kPi;
}

// The head axis a single gesture actually rotates about: the commanded axis
// tilted by a random angle (rms sigma_deg) in a random perpendicular direction.
Vec3 perturbed_axis(const Vec3& axis, float sigma_deg, Rng& rng) {
    const Vec3 helper = std::fabs(axis.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 e1 = unit3(cross3(axis, helper));
    const Vec3 e2 = cross3(axis, e1);
    const float s = sigma_deg * kPi / 180.0f / std::sqrt(2.0f);
    return unit3(add3(axis, add3(scale3(e1, s * rng.gauss()), scale3(e2, s * rng.gauss()))));
}

struct GestureSpec {
    Vec3 head_axis;  // the axis this gesture rotates about (head frame)
    float peak = 0.0f;
    float period_s = 3.0f;
    float pause_after_s = 0.0f;
};

// One recording: 0.75 s lead-in, the gestures (each a full sine of rate, i.e.
// out and back to centre), 0.75 s tail; white gyro noise per body axis.
CalibrationPhaseData make_gestures(CalibrationPhase kind, const Mat3& head_to_body,
                                   const std::vector<GestureSpec>& gestures, const Vec3& bias,
                                   float gyro_noise_degs, Rng& rng) {
    CalibrationPhaseData phase;
    phase.phase = kind;
    const Vec3 accel_package = body_to_package(mat_vec(head_to_body, Vec3{0.0f, 0.0f, 9.81f}));
    uint32_t tick = 500000 + static_cast<uint32_t>(kind) * 100000;
    const auto push = [&](const Vec3& rate_body) {
        ImuSample sample;
        sample.accel_mps2 = accel_package;
        const Vec3 noise{gyro_noise_degs * rng.gauss(), gyro_noise_degs * rng.gauss(),
                         gyro_noise_degs * rng.gauss()};
        sample.gyro_degs = body_to_package(add3(add3(rate_body, bias), noise));
        sample.tick_100us = tick += 21;
        phase.samples.push_back(sample);
    };
    const int lead = static_cast<int>(0.75f * kRateHz);
    for (int i = 0; i < lead; ++i) {
        push(Vec3{});
    }
    for (const GestureSpec& g : gestures) {
        const Vec3 axis_body = mat_vec(head_to_body, g.head_axis);
        const int count = static_cast<int>(g.period_s * kRateHz);
        for (int i = 0; i < count; ++i) {
            const float t = static_cast<float>(i) / kRateHz;
            push(scale3(axis_body, g.peak * std::sin(2.0f * kPi * t / g.period_s)));
        }
        for (int i = 0; i < static_cast<int>(g.pause_after_s * kRateHz); ++i) {
            push(Vec3{});
        }
    }
    for (int i = 0; i < lead; ++i) {
        push(Vec3{});
    }
    return phase;
}

CalibrationPhaseData make_noisy_still(const Mat3& head_to_body, const Vec3& bias, float gyro_noise_degs,
                                      Rng& rng) {
    CalibrationPhaseData phase;
    phase.phase = CalibrationPhase::Still;
    const Vec3 gravity_body = mat_vec(head_to_body, Vec3{0.0f, 0.0f, 9.81f});
    uint32_t tick = 100000;
    for (int i = 0; i < static_cast<int>(3.0f * kRateHz); ++i) {
        ImuSample sample;
        sample.accel_mps2 = body_to_package(
            add3(gravity_body, Vec3{0.02f * rng.gauss(), 0.02f * rng.gauss(), 0.02f * rng.gauss()}));
        sample.gyro_degs = body_to_package(add3(
            bias, Vec3{gyro_noise_degs * rng.gauss(), gyro_noise_degs * rng.gauss(),
                       gyro_noise_degs * rng.gauss()}));
        sample.tick_100us = tick += 21;
        phase.samples.push_back(sample);
    }
    return phase;
}

// The true package -> head matrix for a head_to_body mount.
std::array<float, 9> true_sensor_to_head(const Mat3& head_to_body) {
    std::array<float, 9> m{};
    const Vec3 columns[3] = {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
    for (int col = 0; col < 3; ++col) {
        // package basis vector -> body -> head (head_to_body is orthonormal).
        const Vec3 body{columns[col].x, -columns[col].z, columns[col].y};
        const Vec3 head{head_to_body.m[0][0] * body.x + head_to_body.m[1][0] * body.y + head_to_body.m[2][0] * body.z,
                        head_to_body.m[0][1] * body.x + head_to_body.m[1][1] * body.y + head_to_body.m[2][1] * body.z,
                        head_to_body.m[0][2] * body.x + head_to_body.m[1][2] * body.y + head_to_body.m[2][2] * body.z};
        m[static_cast<size_t>(col)] = head.x;
        m[static_cast<size_t>(3 + col)] = head.y;
        m[static_cast<size_t>(6 + col)] = head.z;
    }
    return m;
}

// Rotation angle of a * b^T (deg), small-angle safe (atan2, not acos).
float matrix_angle_deg(const std::array<float, 9>& a, const std::array<float, 9>& b) {
    float e[3][3]{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k < 3; ++k) {
                e[r][c] += a[static_cast<size_t>(r * 3 + k)] * b[static_cast<size_t>(c * 3 + k)];
            }
        }
    }
    const float sx = e[2][1] - e[1][2];
    const float sy = e[0][2] - e[2][0];
    const float sz = e[1][0] - e[0][1];
    const float sine = 0.5f * std::sqrt(sx * sx + sy * sy + sz * sz);
    const float cosine = 0.5f * (e[0][0] + e[1][1] + e[2][2] - 1.0f);
    return std::atan2(sine, cosine) * 180.0f / kPi;
}

float determinant(const std::array<float, 9>& m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

// Gestures for one step: per-gesture axis wobble sigma_deg, varied speed and
// size, and a pause at centre after about half of them.
std::vector<GestureSpec> gesture_set(const Vec3& head_axis, float peak, int count, float sigma_deg, Rng& rng) {
    std::vector<GestureSpec> gestures;
    for (int i = 0; i < count; ++i) {
        GestureSpec g;
        g.head_axis = perturbed_axis(head_axis, sigma_deg, rng);
        g.peak = peak * rng.uniform(0.8f, 1.2f);
        g.period_s = 3.0f * rng.uniform(0.8f, 1.2f);
        g.pause_after_s = rng.uniform() < 0.5f ? 0.0f : rng.uniform(0.3f, 0.9f);
        gestures.push_back(g);
    }
    return gestures;
}

int failures = 0;

void check(bool condition, const char* label, float value = 0.0f, float limit = 0.0f) {
    std::printf("  [%s] %s", condition ? "PASS" : "FAIL", label);
    if (limit != 0.0f) {
        std::printf(" (value %.5f, limit %.5f)", value, limit);
    }
    std::printf("\n");
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main() {
    const Mat3 head_to_body = multiply(rotation(1.0f, 0.0f, 0.0f, 4.0f),
                                       rotation(0.0f, 0.0f, 1.0f, 37.0f));
    const Vec3 bias{0.18f, -0.27f, 0.11f};
    const CalibrationPhaseData still = make_still(head_to_body, bias);
    const CalibrationPhaseData yaw =
        make_motion(CalibrationPhase::Yaw, head_to_body, Vec3{0.0f, 0.0f, 1.0f}, bias, 90.0f);
    const CalibrationPhaseData nod =
        make_motion(CalibrationPhase::Nod, head_to_body, Vec3{1.0f, 0.0f, 0.0f}, bias, -80.0f);
    const CalibrationPhaseData tilt =
        make_motion(CalibrationPhase::Tilt, head_to_body, Vec3{0.0f, 1.0f, 0.0f}, bias, 65.0f);

    std::printf("orientation_calibration_selftest: recover a known sensor mount\n");
    const gt::OrientationCalibrationResult result = gt::calibrate_orientation(still, yaw, nod, tilt);
    check(result.ok, result.message.c_str());
    std::printf("  sensor_to_head = {%.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g, %.9g}\n",
                result.sensor_to_head[0], result.sensor_to_head[1], result.sensor_to_head[2],
                result.sensor_to_head[3], result.sensor_to_head[4], result.sensor_to_head[5],
                result.sensor_to_head[6], result.sensor_to_head[7], result.sensor_to_head[8]);
    if (result.ok) {
        const Vec3 gravity_head = gt::apply_sensor_to_head(result.sensor_to_head,
                                                           still.samples.front().accel_mps2);
        check(std::fabs(gravity_head.x) < 0.01f, "gravity has no head-right component",
              gravity_head.x, 0.01f);
        check(std::fabs(gravity_head.y) < 0.01f, "gravity has no head-forward component",
              gravity_head.y, 0.01f);
        check(std::fabs(gravity_head.z - 9.81f) < 0.01f, "gravity remains on head +Z",
              gravity_head.z, 0.01f);

        const Vec3 yaw_head = gt::apply_sensor_to_head(
            result.sensor_to_head, body_to_package(mat_vec(head_to_body, Vec3{0.0f, 0.0f, 42.0f})));
        const Vec3 nod_head = gt::apply_sensor_to_head(
            result.sensor_to_head, body_to_package(mat_vec(head_to_body, Vec3{37.0f, 0.0f, 0.0f})));
        const Vec3 tilt_head = gt::apply_sensor_to_head(
            result.sensor_to_head, body_to_package(mat_vec(head_to_body, Vec3{0.0f, 31.0f, 0.0f})));
        check(std::fabs(yaw_head.z - 42.0f) < 0.02f && std::fabs(yaw_head.x) < 0.02f &&
                  std::fabs(yaw_head.y) < 0.02f,
              "turn maps only to head Z");
        check(std::fabs(nod_head.x - 37.0f) < 0.02f && std::fabs(nod_head.y) < 0.02f &&
                  std::fabs(nod_head.z) < 0.02f,
              "nod maps only to head X");
        check(std::fabs(tilt_head.y - 31.0f) < 0.02f && std::fabs(tilt_head.x) < 0.02f &&
                  std::fabs(tilt_head.z) < 0.02f,
              "tilt maps only to head Y");

        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("rayneo-orientation-selftest-" + std::to_string(_getpid()) + ".json");
        std::string error;
        check(gt::save_orientation_calibration(path.string(), result, error), "calibration saves");
        std::array<float, 9> loaded{};
        check(gt::load_orientation_calibration(path.string(), loaded, error), "calibration loads");
        float maximum_difference = 0.0f;
        for (size_t i = 0; i < loaded.size(); ++i) {
            maximum_difference = std::max(maximum_difference,
                                          std::fabs(loaded[i] - result.sensor_to_head[i]));
        }
        check(maximum_difference < 1e-6f, "saved matrix round-trips", maximum_difference, 1e-6f);

        // Magnetometer calibration: optional, round-trips, and survives a
        // later re-save of the orientation calibration.
        gt::MagCalibration mag;
        check(gt::load_mag_calibration(path.string(), mag, error) && !mag.valid,
              "missing mag calibration is not an error");
        gt::MagCalibration stored;
        stored.valid = true;
        stored.hard_iron_ut = gt::Vec3{-16.97f, -18.64f, 1.82f};
        stored.field_ut = 51.47f;
        check(gt::save_mag_calibration(path.string(), stored, error), "mag calibration saves");
        check(gt::load_mag_calibration(path.string(), mag, error) && mag.valid &&
                  std::fabs(mag.hard_iron_ut.x + 16.97f) < 1e-3f &&
                  std::fabs(mag.hard_iron_ut.y + 18.64f) < 1e-3f &&
                  std::fabs(mag.hard_iron_ut.z - 1.82f) < 1e-3f && std::fabs(mag.field_ut - 51.47f) < 1e-3f,
              "mag calibration round-trips");
        check(gt::load_orientation_calibration(path.string(), loaded, error),
              "orientation still loads with mag keys present");
        check(gt::save_orientation_calibration(path.string(), result, error) &&
                  gt::load_mag_calibration(path.string(), mag, error) && mag.valid &&
                  std::fabs(mag.hard_iron_ut.y + 18.64f) < 1e-3f,
              "re-saving the orientation keeps the mag calibration");
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
    }

    std::printf("orientation_calibration_selftest: reject insufficient motion\n");
    CalibrationPhaseData no_motion = yaw;
    for (ImuSample& sample : no_motion.samples) {
        sample.gyro_degs = body_to_package(bias);
    }
    const gt::OrientationCalibrationResult rejected =
        gt::calibrate_orientation(still, no_motion, nod, tilt);
    check(!rejected.ok && rejected.code == "NO_MOTION", "missing motion is rejected");

    std::printf("orientation_calibration_selftest: reject invalid sensor values\n");
    CalibrationPhaseData invalid_still = still;
    invalid_still.samples[10].gyro_degs.x = std::numeric_limits<float>::quiet_NaN();
    const gt::OrientationCalibrationResult invalid =
        gt::calibrate_orientation(invalid_still, yaw, nod, tilt);
    check(!invalid.ok && invalid.code == "INVALID_SAMPLE", "NaN sensor data is rejected");

    std::printf("orientation_calibration_selftest: ignore a separate off-axis false start\n");
    CalibrationPhaseData false_start_yaw = yaw;
    const Vec3 off_axis_body = mat_vec(head_to_body, Vec3{50.0f, 0.0f, 0.0f});
    for (size_t i = 0; i < 90; ++i) {
        false_start_yaw.samples[i].gyro_degs = body_to_package(
            Vec3{off_axis_body.x + bias.x, off_axis_body.y + bias.y, off_axis_body.z + bias.z});
    }
    const gt::OrientationCalibrationResult recovered =
        gt::calibrate_orientation(still, false_start_yaw, nod, tilt);
    check(recovered.ok && recovered.up_vs_yaw_deg < 0.1f,
          "contiguous-window selection ignores the false start");

    std::printf("orientation_calibration_selftest: default path is unchanged (golden matrix)\n");
    {
        // Captured from the pre-repeats build (6b64660) for this exact mount.
        const std::array<float, 9> golden{0.798635542f, 0.041980803f, -0.600349128f,
                                          -0.601815045f, 0.0557103865f, -0.796689987f,
                                          0.0f, 0.997564018f, 0.0697569698f};
        float worst = 0.0f;
        for (size_t i = 0; i < golden.size(); ++i) {
            worst = std::max(worst, std::fabs(result.sensor_to_head[i] - golden[i]));
        }
        check(result.ok && worst < 1e-6f, "default calibration matches the pre-repeats matrix", worst, 1e-6f);
        const gt::OrientationCalibrationResult explicit_one =
            gt::calibrate_orientation(still, yaw, nod, tilt, gt::OrientationCalibrationOptions{});
        check(explicit_one.ok && explicit_one.sensor_to_head == result.sensor_to_head &&
                  explicit_one.repeats_requested == 0 && explicit_one.consistency_quality < 0.0f,
              "repeats = 1 is the default path exactly");
        check(gt::calibration_quality(result) == gt::orientation_quality(result),
              "without repeats the reported quality is the old orientation quality");
        const float truth_error = matrix_angle_deg(result.sensor_to_head, true_sensor_to_head(head_to_body));
        check(truth_error < 0.05f, "error metric: exact synthetic mount recovered", truth_error, 0.05f);
    }

    std::printf("orientation_calibration_selftest: repeats on a single-gesture recording\n");
    {
        gt::OrientationCalibrationOptions three;
        three.repeats = 3;
        const gt::OrientationCalibrationResult single_take = gt::calibrate_orientation(still, yaw, nod, tilt, three);
        float worst = 0.0f;
        for (size_t i = 0; i < 9; ++i) {
            worst = std::max(worst, std::fabs(single_take.sensor_to_head[i] - result.sensor_to_head[i]));
        }
        check(single_take.ok && worst < 1e-6f, "one gesture per step gives the same matrix", worst, 1e-6f);
        check(single_take.yaw_repeats.used == 1 && single_take.nod_repeats.used == 1 &&
                  single_take.tilt_repeats.used == 1,
              "one gesture detected per step");
        check(single_take.consistency_quality == 0.6f && gt::calibration_quality(single_take) <= 0.6f,
              "unverified consistency (no repeats found) caps quality at Fair",
              gt::calibration_quality(single_take), 0.6f);
    }

    // Synthetic worn head: every gesture rotates about the commanded head axis
    // tilted by a random angle (rms kWobbleDeg), with gyro noise, varied
    // speed/size and random pauses at centre. Single-shot uses gesture 1 only;
    // the repeat runs record gestures 1..N in one recording.
    const Vec3 kYawAxis{0.0f, 0.0f, 1.0f};
    const Vec3 kNodAxis{1.0f, 0.0f, 0.0f};
    const Vec3 kTiltAxis{0.0f, 1.0f, 0.0f};
    constexpr float kYawPeak = 31.0f;    // ~30 deg out and back in 3 s
    constexpr float kNodPeak = -22.0f;   // ~21 deg down and back
    constexpr float kTiltPeak = 24.0f;   // ~23 deg toward the right shoulder
    constexpr float kGyroNoise = 0.45f;  // per axis: ~0.75 deg/s magnitude, as measured on the GT
    const std::array<float, 9> truth = true_sensor_to_head(head_to_body);

    std::printf("orientation_calibration_selftest: repeat averaging accuracy (Monte Carlo)\n");
    {
        constexpr int kTrials = 150;
        constexpr float kWobbleDeg = 3.0f;
        const int modes[3] = {1, 3, 5};
        std::vector<float> errors[3];
        int rejected_runs[3] = {0, 0, 0};
        int outliers_dropped = 0;
        int split_steps = 0;
        int split_exact = 0;
        bool proper = true;
        float worst_gravity_deg = 0.0f;
        for (int trial = 0; trial < kTrials; ++trial) {
            Rng rng(static_cast<uint64_t>(1000 + trial));
            const CalibrationPhaseData s = make_noisy_still(head_to_body, bias, kGyroNoise, rng);
            const std::vector<GestureSpec> yaw_set = gesture_set(kYawAxis, kYawPeak, 5, kWobbleDeg, rng);
            const std::vector<GestureSpec> nod_set = gesture_set(kNodAxis, kNodPeak, 5, kWobbleDeg, rng);
            const std::vector<GestureSpec> tilt_set = gesture_set(kTiltAxis, kTiltPeak, 5, kWobbleDeg, rng);
            for (int m = 0; m < 3; ++m) {
                const int n = modes[m];
                const auto take = [&](const std::vector<GestureSpec>& set) {
                    std::vector<GestureSpec> first(set.begin(), set.begin() + n);
                    first.back().pause_after_s = 0.0f;
                    return first;
                };
                const CalibrationPhaseData y =
                    make_gestures(CalibrationPhase::Yaw, head_to_body, take(yaw_set), bias, kGyroNoise, rng);
                const CalibrationPhaseData d =
                    make_gestures(CalibrationPhase::Nod, head_to_body, take(nod_set), bias, kGyroNoise, rng);
                const CalibrationPhaseData r =
                    make_gestures(CalibrationPhase::Tilt, head_to_body, take(tilt_set), bias, kGyroNoise, rng);
                gt::OrientationCalibrationOptions options;
                options.repeats = n;
                const gt::OrientationCalibrationResult out = gt::calibrate_orientation(s, y, d, r, options);
                if (!out.ok) {
                    ++rejected_runs[m];
                    continue;
                }
                errors[m].push_back(matrix_angle_deg(out.sensor_to_head, truth));
                outliers_dropped += out.yaw_repeats.rejected + out.nod_repeats.rejected + out.tilt_repeats.rejected;
                if (n > 1) {
                    for (const gt::AxisRepeatStats* st : {&out.yaw_repeats, &out.nod_repeats, &out.tilt_repeats}) {
                        ++split_steps;
                        split_exact += st->detected == n && st->usable == n ? 1 : 0;
                    }
                }
                const float det = determinant(out.sensor_to_head);
                proper = proper && std::fabs(det - 1.0f) < 1e-3f;
                const Vec3 g = gt::apply_sensor_to_head(out.sensor_to_head, s.samples.front().accel_mps2);
                worst_gravity_deg = std::max(worst_gravity_deg, angle3_deg(g, Vec3{0.0f, 0.0f, 1.0f}));
            }
        }
        float mean_error[3] = {0.0f, 0.0f, 0.0f};
        float p95_error[3] = {0.0f, 0.0f, 0.0f};
        for (int m = 0; m < 3; ++m) {
            std::vector<float> sorted = errors[m];
            std::sort(sorted.begin(), sorted.end());
            for (const float e : sorted) {
                mean_error[m] += e / static_cast<float>(sorted.size());
            }
            p95_error[m] = sorted.empty() ? 99.0f : sorted[(sorted.size() * 95) / 100];
            std::printf("  gestures/step %d: mean %.3f deg, p95 %.3f deg, rejected sessions %d of %d\n",
                        modes[m], mean_error[m], p95_error[m], rejected_runs[m], kTrials);
        }
        std::printf("  (wobble %.1f deg rms per gesture, gyro noise %.2f deg/s per axis; outlier gestures "
                    "dropped in total: %d; worst gravity-to-+Z %.2f deg)\n",
                    kWobbleDeg, kGyroNoise, outliers_dropped, worst_gravity_deg);
        check(mean_error[1] < 0.75f * mean_error[0], "3 gestures/step: mean error < 75% of single-shot",
              mean_error[1] / mean_error[0], 0.75f);
        check(mean_error[2] < 0.65f * mean_error[0], "5 gestures/step: mean error < 65% of single-shot",
              mean_error[2] / mean_error[0], 0.65f);
        check(p95_error[1] < p95_error[0], "3 gestures/step: lower p95 than single-shot", p95_error[1],
              p95_error[0]);
        std::printf("  gesture split: %d of %d repeat steps found exactly N usable gestures (pauses at centre "
                    "random, half with none)\n",
                    split_exact, split_steps);
        check(split_exact * 100 >= split_steps * 98, "gestures are split with and without a pause at centre",
              static_cast<float>(split_exact) / static_cast<float>(split_steps > 0 ? split_steps : 1), 0.98f);
        check(proper, "every averaged result is a proper rotation (det +1)");
        check(worst_gravity_deg < 5.0f, "gravity stays on head +Z (within the up-axis error)",
              worst_gravity_deg, 5.0f);
    }

    std::printf("orientation_calibration_selftest: an inconsistent repeat is rejected\n");
    {
        Rng rng(77);
        const CalibrationPhaseData s = make_noisy_still(head_to_body, bias, kGyroNoise, rng);
        const auto clean_set = [&](const Vec3& axis, float peak) {
            std::vector<GestureSpec> set = gesture_set(axis, peak, 3, 0.5f, rng);
            set.back().pause_after_s = 0.0f;
            return set;
        };
        std::vector<GestureSpec> nod_set = clean_set(kNodAxis, kNodPeak);
        // The second nod rolls 15 deg toward the shoulder (nod mixed with tilt,
        // the bug-20 coupling) - a valid single-axis gesture on its own.
        nod_set[1].head_axis = unit3(add3(kNodAxis, scale3(kTiltAxis, std::tan(15.0f * kPi / 180.0f))));
        const CalibrationPhaseData y =
            make_gestures(CalibrationPhase::Yaw, head_to_body, clean_set(kYawAxis, kYawPeak), bias, kGyroNoise, rng);
        const CalibrationPhaseData d =
            make_gestures(CalibrationPhase::Nod, head_to_body, nod_set, bias, kGyroNoise, rng);
        const CalibrationPhaseData r =
            make_gestures(CalibrationPhase::Tilt, head_to_body, clean_set(kTiltAxis, kTiltPeak), bias, kGyroNoise, rng);
        gt::OrientationCalibrationOptions options;
        options.repeats = 3;
        const gt::OrientationCalibrationResult out = gt::calibrate_orientation(s, y, d, r, options);
        check(out.ok, "three nods with one outlier still calibrate");
        std::vector<gt::CalibrationAxisDiagnostics> nods;
        gt::AxisRepeatStats nod_stats;
        gt::CalibrationAxisDiagnostics combined;
        std::string code;
        std::string message;
        gt::analyze_motion_repeats(d, out.gyro_bias_degs, -1.0f, combined, nod_stats, code, message, &nods);
        // What a plain (non-robust) mean of the three nods would have given.
        Vec3 plain{};
        for (const auto& n : nods) {
            plain = add3(plain, n.axis_body);
        }
        plain = unit3(plain);
        const Vec3 true_nod_body = mat_vec(head_to_body, kNodAxis);
        const float robust_axis_error = angle3_deg(combined.axis_body, true_nod_body);
        const float plain_axis_error = angle3_deg(plain, true_nod_body);
        const float error = out.ok ? matrix_angle_deg(out.sensor_to_head, truth) : 99.0f;
        std::printf("  nods detected %d, used %d, rejected %d (%.1f deg off); nod axis error robust %.2f deg "
                    "vs plain mean %.2f deg; calibration error %.3f deg\n",
                    out.nod_repeats.detected, out.nod_repeats.used, out.nod_repeats.rejected,
                    out.nod_repeats.max_rejected_deg, robust_axis_error, plain_axis_error, error);
        check(out.nod_repeats.detected == 3 && out.nod_repeats.rejected == 1 && out.nod_repeats.used == 2,
              "the 15 deg outlier nod is rejected");
        check(robust_axis_error < 1.0f && plain_axis_error > 4.0f,
              "the robust nod axis ignores it; a plain mean would not", robust_axis_error, 1.0f);
        check(error < 1.0f, "calibration error stays below 1 deg", error, 1.0f);
        check(out.yaw_repeats.rejected == 0 && out.tilt_repeats.rejected == 0,
              "consistent steps lose no gestures");
    }

    std::printf("orientation_calibration_selftest: quality drops for inconsistent repeats\n");
    {
        float quality[2] = {0.0f, 0.0f};
        float spread[2] = {0.0f, 0.0f};
        const float wobble[2] = {1.0f, 5.0f};
        for (int k = 0; k < 2; ++k) {
            Rng rng(4242);
            const CalibrationPhaseData s = make_noisy_still(head_to_body, bias, kGyroNoise, rng);
            const auto set = [&](const Vec3& axis, float peak) {
                std::vector<GestureSpec> g = gesture_set(axis, peak, 5, wobble[k], rng);
                g.back().pause_after_s = 0.0f;
                return g;
            };
            const CalibrationPhaseData y =
                make_gestures(CalibrationPhase::Yaw, head_to_body, set(kYawAxis, kYawPeak), bias, kGyroNoise, rng);
            const CalibrationPhaseData d =
                make_gestures(CalibrationPhase::Nod, head_to_body, set(kNodAxis, kNodPeak), bias, kGyroNoise, rng);
            const CalibrationPhaseData r = make_gestures(CalibrationPhase::Tilt, head_to_body,
                                                         set(kTiltAxis, kTiltPeak), bias, kGyroNoise, rng);
            gt::OrientationCalibrationOptions options;
            options.repeats = 5;
            const gt::OrientationCalibrationResult out = gt::calibrate_orientation(s, y, d, r, options);
            quality[k] = out.ok ? gt::calibration_quality(out) : -1.0f;
            spread[k] = out.repeat_spread_deg;
            std::printf("  wobble %.1f deg: ok=%d spread %.2f deg, est. axis error %.2f deg, consistency %.2f, "
                        "quality %.2f (%s), true error %.3f deg\n",
                        wobble[k], out.ok ? 1 : 0, out.repeat_spread_deg, out.axis_error_estimate_deg,
                        out.consistency_quality, quality[k], gt::calibration_quality_label(quality[k]),
                        out.ok ? matrix_angle_deg(out.sensor_to_head, truth) : -1.0f);
        }
        check(quality[0] >= 0.7f, "consistent repeats score Good", quality[0], 0.7f);
        check(quality[1] >= 0.0f && quality[1] < quality[0] - 0.3f,
              "inconsistent repeats score at least 0.3 lower", quality[1], quality[0] - 0.3f);
        check(spread[1] > 2.0f * spread[0], "spread reflects the inconsistency", spread[1], 2.0f * spread[0]);
        check(std::string(gt::calibration_quality_label(0.85f)) == "Good" &&
                  std::string(gt::calibration_quality_label(0.5f)) == "Fair" &&
                  std::string(gt::calibration_quality_label(0.1f)) == "Retry recommended" &&
                  std::string(gt::calibration_quality_label(-1.0f)).empty(),
              "plain-language quality bands");
    }

    std::printf("orientation_calibration_selftest: combine_axis_repeats edge cases\n");
    {
        gt::AxisRepeatStats stats;
        const Vec3 a = unit3(Vec3{1.0f, 0.02f, 0.0f});
        const Vec3 b = unit3(Vec3{1.0f, -0.02f, 0.0f});
        const Vec3 far = unit3(Vec3{1.0f, 0.0f, 0.5f});
        const Vec3 two = gt::combine_axis_repeats({a, far}, stats);
        check(stats.used == 2 && stats.rejected == 0 && stats.spread_deg > 10.0f && two.x > 0.9f,
              "two disagreeing gestures: both kept, large spread (cannot tell which is wrong)");
        gt::combine_axis_repeats({a, b, far, far}, stats);
        check(stats.rejected <= 2 && stats.used >= 2, "never rejects the majority");
        gt::combine_axis_repeats({a}, stats);
        check(stats.used == 1 && stats.spread_deg == 0.0f &&
                  gt::repeat_consistency_quality(stats) < 0.0f,
              "one gesture: no spread evidence");
    }

    std::printf("orientation_calibration_selftest: --progress-json line contract\n");
    {
        gt::CalibrationProgress p;
        p.flow = "orientation";
        p.step = 2;
        p.steps = 4;
        p.state = "retry";
        p.prompt = "Turn \"LEFT\"\nslowly";
        p.detail = "C:\\path\ttab";
        p.quality = 0.12345f;
        const std::string line = gt::format_calibration_progress(p);
        std::printf("  %s\n", line.c_str());
        check(line ==
                  "@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"retry\","
                  "\"prompt\":\"Turn \\\"LEFT\\\"\\nslowly\",\"detail\":\"C:\\\\path\\ttab\","
                  "\"quality\":0.123}",
              "exact line shape with JSON escaping");
        check(line.find('\n') == std::string::npos, "one physical line");
        p.quality = -1.0f;
        p.flow = "mag";
        p.state = "done";
        p.prompt = "x";
        p.detail.clear();
        p.detail.push_back('\x01');
        const std::string na = gt::format_calibration_progress(p);
        std::printf("  %s\n", na.c_str());
        check(na == "@@progress {\"flow\":\"mag\",\"step\":2,\"steps\":4,\"state\":\"done\",\"prompt\":\"x\","
                    "\"detail\":\"\\u0001\",\"quality\":-1}",
              "not-applicable quality is -1 and control bytes are escaped");
        p.quality = 7.0f;
        check(gt::format_calibration_progress(p).find("\"quality\":1.000}") != std::string::npos,
              "quality is clamped to 1");
        p.quality = std::numeric_limits<float>::quiet_NaN();
        check(gt::format_calibration_progress(p).find("\"quality\":-1}") != std::string::npos,
              "non-finite quality reports -1");
    }

    std::printf("orientation_calibration_selftest: piped stdin control (--auto)\n");
    check(gt::parse_calibration_stdin_line("q") == gt::CalibrationStdinCommand::Cancel &&
              gt::parse_calibration_stdin_line("Q\r") == gt::CalibrationStdinCommand::Cancel &&
              gt::parse_calibration_stdin_line(" q \r\n") == gt::CalibrationStdinCommand::Cancel,
          "q cancels (CR/LF and blanks ignored)");
    check(gt::parse_calibration_stdin_line("") == gt::CalibrationStdinCommand::StartNow &&
              gt::parse_calibration_stdin_line("\r") == gt::CalibrationStdinCommand::StartNow,
          "an empty line starts the waiting step now");
    check(gt::parse_calibration_stdin_line("quit") == gt::CalibrationStdinCommand::None &&
              gt::parse_calibration_stdin_line("x") == gt::CalibrationStdinCommand::None,
          "anything else is ignored");

    std::printf("orientation_calibration_selftest: guidance quality scores\n");
    {
        check(gt::still_quality(0.0f) == 1.0f && gt::still_quality(1.5f) == 0.0f &&
                  gt::still_quality(0.75f) > 0.49f && gt::still_quality(0.75f) < 0.51f,
              "still quality spans the 1.5 deg/s gate");
        gt::CalibrationAxisDiagnostics clean;
        clean.dominance = 400.0f;
        clean.excursion_deg = 30.0f;
        gt::CalibrationAxisDiagnostics marginal;
        marginal.dominance = 4.0f;
        marginal.excursion_deg = 12.0f;
        check(gt::axis_quality(clean) > 0.95f, "clean motion scores high", gt::axis_quality(clean), 0.95f);
        check(gt::axis_quality(marginal) < 0.05f, "motion at the gates scores near zero",
              gt::axis_quality(marginal), 0.05f);
        check(gt::orientation_quality(result) > 0.95f, "synthetic exact calibration scores high",
              gt::orientation_quality(result), 0.95f);
        check(gt::orientation_quality(rejected) == 0.0f, "a rejected calibration scores zero");
    }

    std::printf("orientation_calibration_selftest: mag metadata round-trips\n");
    {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("rayneo-orientation-mag-meta-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        std::string error;
        if (result.ok && gt::save_orientation_calibration(path.string(), result, error)) {
            gt::MagCalibration stored;
            stored.valid = true;
            stored.hard_iron_ut = gt::Vec3{-16.8f, -17.4f, 3.0f};
            stored.field_ut = 51.47f;
            stored.earbuds = true;
            stored.quality = 0.82f;
            gt::MagCalibration loaded;
            check(gt::save_mag_calibration(path.string(), stored, error) &&
                      gt::load_mag_calibration(path.string(), loaded, error) && loaded.valid && loaded.earbuds &&
                      std::fabs(loaded.quality - 0.82f) < 1e-3f,
                  "earbuds flag and quality round-trip");
            stored.earbuds = false;
            stored.quality = -1.0f;
            check(gt::save_mag_calibration(path.string(), stored, error) &&
                      gt::load_mag_calibration(path.string(), loaded, error) && loaded.valid && !loaded.earbuds &&
                      loaded.quality < 0.0f,
                  "absent quality loads as unknown");
            {
                std::ofstream legacy(path);
                legacy << "{\"version\":1,\"sensor_to_head\":[1,0,0,0,1,0,0,0,1],"
                          "\"mag_hard_iron_ut\":[-16.81,-17.41,3.0],\"mag_field_ut\":51.47}\n";
            }
            check(gt::load_mag_calibration(path.string(), loaded, error) && loaded.valid && !loaded.earbuds &&
                      loaded.quality < 0.0f && std::fabs(loaded.hard_iron_ut.z - 3.0f) < 1e-3f,
                  "a pre-metadata file still loads");
        } else {
            check(false, "could not write the metadata test file");
        }
        std::error_code ignored_error;
        std::filesystem::remove(path, ignored_error);
    }

    std::printf("orientation_calibration_selftest: orientation quality metadata in the file\n");
    {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("rayneo-orientation-quality-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        std::string error;
        gt::OrientationCalibrationInfo info;
        check(result.ok && gt::save_orientation_calibration(path.string(), result, error) &&
                  gt::load_orientation_calibration_info(path.string(), info, error) &&
                  std::fabs(info.quality - gt::orientation_quality(result)) < 1e-3f && info.repeats == 0 &&
                  info.spread_deg < 0.0f,
              "a default calibration stores its quality (no repeat keys)");
        gt::OrientationCalibrationResult repeated = result;
        repeated.repeats_requested = 3;
        repeated.consistency_quality = 0.5f;
        repeated.repeat_spread_deg = 4.0f;
        check(gt::save_orientation_calibration(path.string(), repeated, error) &&
                  gt::load_orientation_calibration_info(path.string(), info, error) &&
                  std::fabs(info.quality - 0.5f) < 1e-3f && info.repeats == 3 &&
                  std::fabs(info.spread_deg - 4.0f) < 1e-3f,
              "repeats, spread and the combined quality round-trip");
        gt::MagCalibration mag;
        mag.valid = true;
        mag.hard_iron_ut = gt::Vec3{-16.8f, -17.4f, 3.0f};
        mag.field_ut = 51.5f;
        std::array<float, 9> loaded{};
        check(gt::save_mag_calibration(path.string(), mag, error) &&
                  gt::load_orientation_calibration_info(path.string(), info, error) && info.repeats == 3 &&
                  std::fabs(info.quality - 0.5f) < 1e-3f &&
                  gt::load_orientation_calibration(path.string(), loaded, error) &&
                  std::fabs(loaded[1] - result.sensor_to_head[1]) < 1e-6f,
              "a later mag calibration keeps the orientation metadata");
        {
            std::ofstream legacy(path);
            legacy << "{\"version\":1,\"sensor_to_head\":[1,0,0,0,1,0,0,0,1],\"mag_quality\":0.9}\n";
        }
        check(gt::load_orientation_calibration_info(path.string(), info, error) && info.quality < 0.0f &&
                  info.repeats == 0 && info.spread_deg < 0.0f &&
                  gt::load_orientation_calibration(path.string(), loaded, error),
              "an older file loads with unknown quality (mag_quality is not mistaken for it)");
        std::error_code ignored_error;
        std::filesystem::remove(path, ignored_error);
    }

    std::printf("orientation_calibration_selftest: reject unsupported file versions\n");
    const std::filesystem::path bad_version_path =
        std::filesystem::temp_directory_path() /
        ("rayneo-orientation-bad-version-" + std::to_string(_getpid()) + ".json");
    {
        std::ofstream bad_version(bad_version_path);
        bad_version << "{\"version\":2,\"sensor_to_head\":[1,0,0,0,1,0,0,0,1]}\n";
    }
    std::array<float, 9> ignored{};
    std::string version_error;
    check(!gt::load_orientation_calibration(bad_version_path.string(), ignored, version_error) &&
              version_error == "unsupported calibration file version",
          "unsupported calibration version is rejected");
    std::error_code remove_error;
    std::filesystem::remove(bad_version_path, remove_error);

    std::printf("orientation_calibration_selftest: %s (%d failures)\n",
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
