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

// How the repeated gestures of one motion step were combined (--repeats).
struct AxisRepeatStats {
    int detected = 0;   // out-and-back gestures found in the recording
    int usable = 0;     // gestures that passed the single-gesture gates
    int used = 0;       // usable gestures kept after outlier rejection
    int rejected = 0;   // usable gestures dropped as outliers
    float spread_deg = 0.0f;        // rms angle of the used gestures about their mean (0 for one)
    float max_rejected_deg = 0.0f;  // largest deviation of a rejected gesture from the medoid
};

struct OrientationCalibrationOptions {
    // 1 (default): each motion step is one gesture, analysed exactly as before
    // (the most energetic motion in the recording). N > 1: the recording holds
    // up to N out-and-back gestures; every usable gesture is measured and the
    // axes are combined robustly (medoid outlier rejection, chordal mean).
    int repeats = 1;
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
    // Repeat analysis (repeats_requested > 1 only; otherwise left at defaults).
    int repeats_requested = 0;
    AxisRepeatStats yaw_repeats;
    AxisRepeatStats nod_repeats;
    AxisRepeatStats tilt_repeats;
    float repeat_spread_deg = -1.0f;        // rms over axes with >= 2 used gestures
    float axis_error_estimate_deg = -1.0f;  // rms standard error of the averaged axes
    float consistency_quality = -1.0f;      // 0..1 from the spread; -1 without repeats
};

OrientationCalibrationResult calibrate_orientation(const CalibrationPhaseData& still,
                                                   const CalibrationPhaseData& yaw,
                                                   const CalibrationPhaseData& nod,
                                                   const CalibrationPhaseData& tilt);
OrientationCalibrationResult calibrate_orientation(const CalibrationPhaseData& still,
                                                   const CalibrationPhaseData& yaw,
                                                   const CalibrationPhaseData& nod,
                                                   const CalibrationPhaseData& tilt,
                                                   const OrientationCalibrationOptions& options);

// Repeated-gesture analysis of one motion step: splits the recording into
// out-and-back gestures (pauses at centre optional), applies the single-
// gesture gates to each, rejects outliers and combines the rest. Fails only
// when no gesture is usable (code/message of the first failing gesture).
// combined.axis_body is the robust mean axis; excursion/dominance/perp are
// means over the used gestures. gestures (optional) receives every usable one.
bool analyze_motion_repeats(const CalibrationPhaseData& phase, const Vec3& bias_body, float command_sign,
                            CalibrationAxisDiagnostics& combined, AxisRepeatStats& stats,
                            std::string& code, std::string& message,
                            std::vector<CalibrationAxisDiagnostics>* gestures = nullptr);

// Robust mean direction of unit axes. With three or more, gestures further
// than max(8 deg, 3 x median deviation) from the medoid are rejected (never
// the majority); the rest are averaged on the sphere (normalised sum). Fills
// usable/used/rejected/spread_deg/max_rejected_deg; kept marks the survivors.
Vec3 combine_axis_repeats(const std::vector<Vec3>& axes, AxisRepeatStats& stats,
                          std::vector<bool>* kept = nullptr);

// 0..1 consistency of one step's repeats (1 - spread / 8 deg); -1 with fewer
// than two used gestures.
float repeat_consistency_quality(const AxisRepeatStats& stats);

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

// Front-end control over a piped stdin in --auto mode (the in-app wizard has
// no console to send Ctrl+C to): "q" cancels the run through the normal path
// (stream off, a final "failed"/"cancelled" line, exit 1), an empty line
// starts the current waiting step now instead of after the countdown. Other
// lines are ignored. CR/LF and surrounding blanks are ignored.
enum class CalibrationStdinCommand { None, StartNow, Cancel };
CalibrationStdinCommand parse_calibration_stdin_line(const std::string& line);

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
// What the tool reports on "done" and stores: orientation_quality, and with
// repeats the weaker of that and the repeat consistency. Without repeats it
// equals orientation_quality exactly.
float calibration_quality(const OrientationCalibrationResult& result);
// Plain-language band for a 0..1 quality: "Good" (>= 0.7), "Fair" (>= 0.4),
// "Retry recommended" (below), "" when unknown (< 0).
const char* calibration_quality_label(float quality);

// Optional orientation metadata in the calibration file (version stays 1;
// absent keys load as unknown): "orientation_quality", "orientation_repeats",
// "orientation_spread_deg".
struct OrientationCalibrationInfo {
    float quality = -1.0f;    // 0..1, -1 unknown (files written before 2026-11-13)
    int repeats = 0;          // gestures requested per motion step; 0 = not recorded (single)
    float spread_deg = -1.0f; // repeat spread, -1 when no repeats were measured
};
bool load_orientation_calibration_info(const std::string& path, OrientationCalibrationInfo& info,
                                       std::string& error);

// Optional magnetometer calibration stored in the same file. A missing key is
// not an error (returns true with mag.valid == false).
bool load_mag_calibration(const std::string& path, MagCalibration& mag, std::string& error);
// Rewrites the file keeping its sensor_to_head and adding/replacing the mag keys.
bool save_mag_calibration(const std::string& path, const MagCalibration& mag, std::string& error);

}  // namespace gt
