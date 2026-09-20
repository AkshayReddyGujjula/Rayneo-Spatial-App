#pragma once

#include "imu/gt_protocol.h"

#include <array>
#include <string>
#include <vector>

namespace gt {

enum class CalibrationPhase {
    Still,
    Yaw,
    Nod,
    Tilt,
};

struct CalibrationPhaseData {
    CalibrationPhase phase = CalibrationPhase::Still;
    std::vector<ImuSample> samples;
};

struct CalibrationAxisDiagnostics {
    Vec3 axis_body;
    float active_seconds = 0.0f;
    float excursion_deg = 0.0f;
    float dominance = 0.0f;
    float perpendicular_rms_degs = 0.0f;
    size_t active_samples = 0;
};

struct OrientationCalibrationResult {
    bool ok = false;
    std::string code;
    std::string message;
    Vec3 gyro_bias_degs;
    Vec3 up_body;
    Vec3 right_body;
    Vec3 forward_body;
    std::array<float, 9> sensor_to_head{
        1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f,
        0.0f, 1.0f, 0.0f,
    };
    CalibrationAxisDiagnostics yaw;
    CalibrationAxisDiagnostics nod;
    CalibrationAxisDiagnostics tilt;
    float still_gyro_rms_degs = 0.0f;
    float up_vs_yaw_deg = 0.0f;
    float nod_level_error_deg = 0.0f;
    float nod_tilt_error_deg = 0.0f;
    float handedness_error_deg = 0.0f;
};

OrientationCalibrationResult calibrate_orientation(const CalibrationPhaseData& still,
                                                   const CalibrationPhaseData& yaw,
                                                   const CalibrationPhaseData& nod,
                                                   const CalibrationPhaseData& tilt);

Vec3 apply_sensor_to_head(const std::array<float, 9>& matrix, const Vec3& value);
bool save_orientation_calibration(const std::string& path, const OrientationCalibrationResult& result,
                                  std::string& error);
bool load_orientation_calibration(const std::string& path, std::array<float, 9>& matrix,
                                  std::string& error);

}  // namespace gt
