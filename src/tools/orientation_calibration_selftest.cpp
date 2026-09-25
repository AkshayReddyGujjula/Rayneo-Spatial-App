#include "imu/orientation_calibration.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>

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
            std::filesystem::temp_directory_path() / "rayneo-orientation-selftest.json";
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

    std::printf("orientation_calibration_selftest: reject unsupported file versions\n");
    const std::filesystem::path bad_version_path =
        std::filesystem::temp_directory_path() / "rayneo-orientation-bad-version.json";
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
