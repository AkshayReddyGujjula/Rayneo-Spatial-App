#pragma once

// Live guidance for the guided orientation calibration (display only).
//
// While orientation_calibrate.exe waits for or records a step it can stream
// "@@live {json}" lines (--progress-json) so a front end can draw a guide: a
// line along the step's intended motion and a ball at the head's live
// position. Nothing here feeds the calibration maths, its gates or the saved
// file: the tracker only integrates the same samples for display.

#include "imu/gt_protocol.h"

#include <array>
#include <cstddef>
#include <deque>
#include <string>

namespace gt {

enum class LiveStepKind { Still, Turn, Nod, Tilt };

// Display constants. The gate is the real single-gesture excursion gate of
// calibrate_orientation (12 deg); the targets are recommendations (the axis
// quality score gives full travel marks from 25 deg; nod and tilt are kept
// smaller for comfort). still_gate is the real 1.5 deg/s stillness limit.
constexpr float kLiveExcursionGateDeg = 12.0f;
constexpr float kLiveStillGateDegs = 1.5f;
float live_target_deg(LiveStepKind kind);
const char* live_step_kind_name(LiveStepKind kind);

// Guide frame in the calibration "body" frame (package (x, -z, y)), the same
// frame estimate_still_bias and the axis analysis use. up = measured gravity;
// right/forward complete a right-handed basis (right x forward = up).
struct CalibrationLiveFrame {
    bool valid = false;
    Vec3 up_body;
    Vec3 right_body;
    Vec3 forward_body;
    // "file" (forward from the existing calibration file), "measured" (from
    // this run's nod axis), "nominal" (the typical GT mounting) or "gravity"
    // (up from the live accelerometer, before the still step has finished).
    std::string source = "nominal";
};

// Builds the frame from a measured up (any length; body frame) and a forward
// hint (body frame). The hint is projected onto the horizontal plane; a hint
// parallel to up falls back to the nominal mounting.
CalibrationLiveFrame make_live_frame(const Vec3& up_body, const Vec3& forward_hint_body, const char* source);
// Forward direction (body frame) from a sensor_to_head matrix (row 1 is head
// forward in package coordinates).
Vec3 live_forward_from_sensor_to_head(const std::array<float, 9>& sensor_to_head);
// Nominal GT mounting (AGENTS.md invariant 3): head-forward = package +Y.
Vec3 live_nominal_forward_body();
Vec3 live_package_to_body(const Vec3& package);

// One "@@live" line. Angles are degrees of head rotation since the step's
// reference pose (the pose at the start of waiting / recording), in the guide
// frame: yaw + = turned left, pitch + = nodded up, roll + = tilted right.
struct CalibrationLiveLine {
    int step = 1;
    LiveStepKind kind = LiveStepKind::Still;
    std::string state = "recording";  // "waiting" | "recording"
    float elapsed_s = 0.0f;
    float remaining_s = -1.0f;  // -1 unknown (waiting for ENTER)
    std::string frame = "nominal";
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
    float along_deg = 0.0f;  // signed, + = the commanded direction; 0 for still
    float off_deg = 0.0f;    // rotation perpendicular to the step's axis (still: total)
    float speed_degs = 0.0f;
    float still_rms_degs = 0.0f;  // gyro rms over the trailing 1.5 s
    float peak_deg = 0.0f;        // furthest along in the current gesture
    int repeats_done = 0;
    int repeats_required = 0;
    std::string hint = "GET_READY";
};

constexpr const char* kLivePrefix = "@@live ";
// Exactly:
// @@live {"step":N,"kind":"still"|"turn"|"nod"|"tilt","state":"waiting"|"recording",
//         "elapsed":S,"remaining":S,"frame":"file"|"measured"|"nominal"|"gravity",
//         "yaw":D,"pitch":D,"roll":D,"along":D,"off":D,"speed":D,"still_rms":D,
//         "still_gate":1.5,"gate":12,"target":D,"peak":D,"repeats":N,
//         "repeats_required":N,"hint":"..."}
std::string format_calibration_live(const CalibrationLiveLine& line);

// Hint codes: GET_READY (waiting), STEADY / HOLD (still step: fine / moving),
// GO (start the gesture), FURTHER (keep going / that one was too small),
// BACK_TO_CENTRE (far enough, return), SLOWER, OFF_AXIS (other axes mixed
// in), WRONG_WAY, DONE (all repeats seen; stay at centre).
class CalibrationLiveTracker {
public:
    // Starts a new reference pose. bias_body: the still step's gyro bias
    // (zero before it is known). frame.valid == false: up is taken from the
    // live accelerometer ("gravity").
    void begin(LiveStepKind kind, int step, const char* state, int repeats_required, const Vec3& bias_body,
               const CalibrationLiveFrame& frame);
    void add(const ImuSample& sample);
    CalibrationLiveLine snapshot(float elapsed_s, float remaining_s) const;
    // Rotation since the reference as a rotation vector in degrees (body frame).
    Vec3 rotation_vector_body_deg() const;

private:
    LiveStepKind kind_ = LiveStepKind::Still;
    int step_ = 1;
    std::string state_ = "waiting";
    int repeats_required_ = 0;
    Vec3 bias_body_;
    CalibrationLiveFrame frame_;
    bool have_previous_ = false;
    ImuSample previous_;
    // Orientation since the reference: (w, x, y, z).
    double qw_ = 1.0, qx_ = 0.0, qy_ = 0.0, qz_ = 0.0;
    float speed_degs_ = 0.0f;
    float peak_deg_ = 0.0f;
    int repeats_done_ = 0;
    bool returning_ = false;
    float valley_deg_ = 0.0f;
    Vec3 gravity_sum_;
    size_t gravity_count_ = 0;
    // Trailing 1.5 s stillness window.
    struct WindowSample {
        float dt;
        Vec3 gyro;
    };
    std::deque<WindowSample> window_;
    double window_seconds_ = 0.0;
    double sum_x_ = 0.0, sum_y_ = 0.0, sum_z_ = 0.0, sum_sq_ = 0.0;
};

// Hint decision (pure). along/peak/off in degrees, speed in deg/s.
const char* live_hint(LiveStepKind kind, const std::string& state, float along_deg, float peak_deg, float off_deg,
                      float speed_degs, float still_rms_degs, int repeats_done, int repeats_required);

}  // namespace gt
