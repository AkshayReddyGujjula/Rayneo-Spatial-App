#pragma once

#include "imu/gt_protocol.h"
#include "imu/mag_heading.h"

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

// Analyses a single motion phase so a tool can retry one step without
// repeating the whole session. command_sign is +1 for yaw and tilt, -1 for nod,
// matching calibrate_orientation.
bool analyze_motion_phase(const CalibrationPhaseData& phase, const Vec3& bias_body,
                          float command_sign, CalibrationAxisDiagnostics& diagnostics,
                          std::string& code, std::string& message);

// Mean body-frame gyro over the quietest contiguous window of the still phase,
// with that window's RMS (deg/s) reported through rms_degs.
Vec3 estimate_still_bias(const CalibrationPhaseData& still, float& rms_degs);

Vec3 apply_sensor_to_head(const std::array<float, 9>& matrix, const Vec3& value);
bool save_orientation_calibration(const std::string& path, const OrientationCalibrationResult& result,
                                  std::string& error);
bool load_orientation_calibration(const std::string& path, std::array<float, 9>& matrix,
                                  std::string& error);
// Machine-readable progress for a front end driving orientation_calibrate.exe
// (--progress-json). One line per state change, exactly:
//   @@progress {"flow":"orientation"|"mag","step":N,"steps":M,
//               "state":"waiting"|"recording"|"ok"|"retry"|"failed"|"done",
//               "prompt":"...","detail":"...","quality":Q}
// with Q in [0, 1] or -1 when not applicable. The final line of a run has
// state "done" (file written) or "failed". The shape is a contract with the
// controller's onboarding wizard; extend it only by adding new state values
// in agreement with that parser, never by changing the existing keys.
struct CalibrationProgress {
    std::string flow;   // "orientation" or "mag"
    int step = 0;       // 1-based
    int steps = 0;
    std::string state;  // see above
    std::string prompt;
    std::string detail;
    float quality = -1.0f;
};
constexpr const char* kProgressPrefix = "@@progress ";
std::string format_calibration_progress(const CalibrationProgress& progress);

// Heuristic 0..1 quality scores for guidance (not acceptance gates; the gates
// in calibrate_orientation / fit_mag_hard_iron decide pass/fail).
// Still: 1 at a motionless head, 0 at the 1.5 deg/s rejection limit.
float still_quality(float gyro_rms_degs);
// Motion step: geometric mean of axis purity (dominance against its 4.0 gate)
// and excursion (12 deg gate, full marks from 25 deg).
float axis_quality(const CalibrationAxisDiagnostics& axis);
// Whole calibration: mean headroom of the four cross-axis consistency checks
// against their rejection limits (1 = perfectly consistent axes).
float orientation_quality(const OrientationCalibrationResult& result);

// Optional magnetometer calibration stored in the same file. A missing key is
// not an error (returns true with mag.valid == false).
bool load_mag_calibration(const std::string& path, MagCalibration& mag, std::string& error);
// Rewrites the file keeping its sensor_to_head and adding/replacing the mag keys.
bool save_mag_calibration(const std::string& path, const MagCalibration& mag, std::string& error);

}  // namespace gt
