#include "imu/orientation_calibration.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gt {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMotionOnDegs = 2.5f;
constexpr float kMotionOffDegs = 1.5f;
constexpr float kMotionEndHoldSeconds = 0.45f;
constexpr float kMinMotionSeconds = 0.25f;
constexpr float kMinExcursionDeg = 12.0f;
constexpr float kMinDominance = 4.0f;
constexpr float kMaxPerpendicularRmsDegs = 4.0f;

struct Mat3 {
    float m[3][3]{};
};

struct EigenResult {
    std::array<float, 3> values{};
    std::array<Vec3, 3> vectors{};
};

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 add(const Vec3& a, const Vec3& b) {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 scale(const Vec3& v, float s) {
    return Vec3{v.x * s, v.y * s, v.z * s};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}

float norm(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(const Vec3& v) {
    const float length = norm(v);
    return length > 1e-8f ? scale(v, 1.0f / length) : Vec3{};
}

float clamp_unit(float v) {
    return std::max(-1.0f, std::min(1.0f, v));
}

float angle_deg(const Vec3& a, const Vec3& b) {
    return std::acos(clamp_unit(dot(normalized(a), normalized(b)))) * 180.0f / kPi;
}

Vec3 package_to_body(const Vec3& v) {
    return Vec3{v.x, -v.z, v.y};
}

Vec3 mean(const std::vector<Vec3>& values) {
    Vec3 total;
    for (const Vec3& value : values) {
        total = add(total, value);
    }
    return values.empty() ? Vec3{} : scale(total, 1.0f / static_cast<float>(values.size()));
}

EigenResult jacobi_eigen(Mat3 matrix) {
    Mat3 vectors{};
    vectors.m[0][0] = 1.0f;
    vectors.m[1][1] = 1.0f;
    vectors.m[2][2] = 1.0f;

    for (int sweep = 0; sweep < 64; ++sweep) {
        int p = 0;
        int q = 1;
        float largest = std::fabs(matrix.m[0][1]);
        if (std::fabs(matrix.m[0][2]) > largest) {
            p = 0;
            q = 2;
            largest = std::fabs(matrix.m[0][2]);
        }
        if (std::fabs(matrix.m[1][2]) > largest) {
            p = 1;
            q = 2;
            largest = std::fabs(matrix.m[1][2]);
        }
        if (largest < 1e-9f) {
            break;
        }

        const float angle = 0.5f * std::atan2(2.0f * matrix.m[p][q],
                                              matrix.m[q][q] - matrix.m[p][p]);
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        for (int k = 0; k < 3; ++k) {
            const float kp = matrix.m[k][p];
            const float kq = matrix.m[k][q];
            matrix.m[k][p] = c * kp - s * kq;
            matrix.m[k][q] = s * kp + c * kq;
        }
        for (int k = 0; k < 3; ++k) {
            const float pk = matrix.m[p][k];
            const float qk = matrix.m[q][k];
            matrix.m[p][k] = c * pk - s * qk;
            matrix.m[q][k] = s * pk + c * qk;
        }
        for (int k = 0; k < 3; ++k) {
            const float kp = vectors.m[k][p];
            const float kq = vectors.m[k][q];
            vectors.m[k][p] = c * kp - s * kq;
            vectors.m[k][q] = s * kp + c * kq;
        }
    }

    EigenResult result;
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return matrix.m[a][a] > matrix.m[b][b];
    });
    for (int i = 0; i < 3; ++i) {
        const int j = order[i];
        result.values[i] = matrix.m[j][j];
        result.vectors[i] = normalized(Vec3{vectors.m[0][j], vectors.m[1][j], vectors.m[2][j]});
    }
    return result;
}

float sample_dt(const ImuSample& previous, const ImuSample& current) {
    const uint32_t delta = current.tick_100us - previous.tick_100us;
    const float dt = static_cast<float>(delta) * 1e-4f;
    return dt > 1e-5f && dt < 0.05f ? dt : 1.0f / 476.0f;
}

bool finite_vec(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool valid_samples(const CalibrationPhaseData& phase) {
    for (const ImuSample& sample : phase.samples) {
        if (!finite_vec(sample.accel_mps2) || !finite_vec(sample.gyro_degs) ||
            !finite_vec(sample.mag_ut) || !std::isfinite(sample.temp_c)) {
            return false;
        }
    }
    return true;
}

float phase_duration(const CalibrationPhaseData& phase) {
    float duration = 0.0f;
    for (size_t i = 1; i < phase.samples.size(); ++i) {
        duration += sample_dt(phase.samples[i - 1], phase.samples[i]);
    }
    return duration;
}

OrientationCalibrationResult fail(std::string code, std::string message) {
    OrientationCalibrationResult result;
    result.code = std::move(code);
    result.message = std::move(message);
    return result;
}

bool extract_axis(const CalibrationPhaseData& phase, const Vec3& bias_body, float command_sign,
                  CalibrationAxisDiagnostics& output, std::string& code, std::string& message) {
    std::vector<Vec3> corrected;
    corrected.reserve(phase.samples.size());
    size_t first_active = phase.samples.size();
    size_t last_active = 0;
    for (size_t i = 0; i < phase.samples.size(); ++i) {
        const Vec3 gyro = subtract(package_to_body(phase.samples[i].gyro_degs), bias_body);
        corrected.push_back(gyro);
    }

    // Find one contiguous motion using on/off hysteresis. A short below-threshold
    // gap is retained so the zero crossing between the out and return halves does
    // not split a valid motion. If the user makes multiple separate motions, keep
    // the run with the most angular-rate energy instead of merging them.
    bool in_run = false;
    size_t run_start = 0;
    size_t run_last_above_off = 0;
    float below_off_seconds = 0.0f;
    float run_energy = 0.0f;
    float best_energy = -1.0f;
    auto finish_run = [&]() {
        if (in_run && run_last_above_off > run_start && run_energy > best_energy) {
            first_active = run_start;
            last_active = run_last_above_off;
            best_energy = run_energy;
        }
        in_run = false;
        below_off_seconds = 0.0f;
        run_energy = 0.0f;
    };
    for (size_t i = 0; i < corrected.size(); ++i) {
        const float magnitude = norm(corrected[i]);
        const float dt = i > 0 ? sample_dt(phase.samples[i - 1], phase.samples[i])
                               : 1.0f / 476.0f;
        if (!in_run && magnitude > kMotionOnDegs) {
            in_run = true;
            run_start = i;
            run_last_above_off = i;
        }
        if (!in_run) {
            continue;
        }
        run_energy += magnitude * magnitude * dt;
        if (magnitude > kMotionOffDegs) {
            run_last_above_off = i;
            below_off_seconds = 0.0f;
        } else {
            below_off_seconds += dt;
            if (below_off_seconds >= kMotionEndHoldSeconds) {
                finish_run();
            }
        }
    }
    finish_run();
    if (first_active >= phase.samples.size() || last_active <= first_active) {
        code = "NO_MOTION";
        message = "not enough motion detected - try the step again";
        return false;
    }

    float duration = 0.0f;
    for (size_t i = first_active + 1; i <= last_active; ++i) {
        duration += sample_dt(phase.samples[i - 1], phase.samples[i]);
    }
    if (duration < kMinMotionSeconds) {
        code = "NO_MOTION";
        message = "motion was too brief - move smoothly and try the step again";
        return false;
    }

    std::vector<Vec3> segment(corrected.begin() + static_cast<std::ptrdiff_t>(first_active),
                              corrected.begin() + static_cast<std::ptrdiff_t>(last_active + 1));
    const Vec3 centre = mean(segment);
    Mat3 covariance{};
    for (const Vec3& value : segment) {
        const Vec3 d = subtract(value, centre);
        const float c[3]{d.x, d.y, d.z};
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                covariance.m[row][col] += c[row] * c[col];
            }
        }
    }
    const float inverse_count = 1.0f / static_cast<float>(segment.size());
    for (auto& row : covariance.m) {
        for (float& value : row) {
            value *= inverse_count;
        }
    }
    const EigenResult eigen = jacobi_eigen(covariance);
    Vec3 axis = eigen.vectors[0];

    float first_half_projection = 0.0f;
    const size_t half = std::max<size_t>(1, segment.size() / 2);
    for (size_t i = 0; i < half; ++i) {
        first_half_projection += dot(segment[i], axis);
    }
    if (first_half_projection < 0.0f) {
        axis = scale(axis, -1.0f);
    }
    if (command_sign < 0.0f) {
        axis = scale(axis, -1.0f);
    }

    float accumulated = 0.0f;
    float minimum = 0.0f;
    float maximum = 0.0f;
    float perpendicular_sum = 0.0f;
    for (size_t i = first_active; i <= last_active; ++i) {
        const float dt = i > first_active ? sample_dt(phase.samples[i - 1], phase.samples[i])
                                          : 1.0f / 476.0f;
        accumulated += dot(corrected[i], axis) * dt;
        minimum = std::min(minimum, accumulated);
        maximum = std::max(maximum, accumulated);
        const Vec3 perpendicular = subtract(corrected[i], scale(axis, dot(corrected[i], axis)));
        perpendicular_sum += dot(perpendicular, perpendicular);
    }

    output.axis_body = axis;
    output.active_seconds = duration;
    output.excursion_deg = maximum - minimum;
    output.dominance = std::max(eigen.values[0], 1e-12f) /
                       (std::max(eigen.values[1], 1e-12f) + std::max(eigen.values[2], 1e-12f));
    output.perpendicular_rms_degs =
        std::sqrt(perpendicular_sum / static_cast<float>(segment.size()));
    output.active_samples = segment.size();

    if (output.excursion_deg < kMinExcursionDeg) {
        char text[192];
        std::snprintf(text, sizeof(text),
                      "only %.1f deg of motion was detected - move a little further (%.0f deg is "
                      "enough) and try this step again",
                      output.excursion_deg, kMinExcursionDeg);
        code = "SMALL_EXCURSION";
        message = text;
        return false;
    }
    if (output.dominance < kMinDominance) {
        char text[192];
        std::snprintf(text, sizeof(text),
                      "motion used more than one axis (dominance %.1f) - keep the other head axes "
                      "still and try this step again",
                      output.dominance);
        code = "AMBIGUOUS_AXIS";
        message = text;
        return false;
    }
    if (output.perpendicular_rms_degs > kMaxPerpendicularRmsDegs) {
        code = "OFF_AXIS";
        message = "too much off-axis motion - keep the other head axes still";
        return false;
    }
    return true;
}

}  // namespace

bool analyze_motion_phase(const CalibrationPhaseData& phase, const Vec3& bias_body,
                          float command_sign, CalibrationAxisDiagnostics& diagnostics,
                          std::string& code, std::string& message) {
    if (phase.samples.size() < 100) {
        code = "TOO_FEW_SAMPLES";
        message = "this step did not record enough IMU samples - try it again";
        return false;
    }
    if (!valid_samples(phase)) {
        code = "INVALID_SAMPLE";
        message = "the IMU stream contained a non-finite sensor value";
        return false;
    }
    return extract_axis(phase, bias_body, command_sign, diagnostics, code, message);
}

Vec3 estimate_still_bias(const CalibrationPhaseData& still, float& rms_degs) {
    std::vector<Vec3> still_gyro;
    still_gyro.reserve(still.samples.size());
    for (const ImuSample& sample : still.samples) {
        still_gyro.push_back(package_to_body(sample.gyro_degs));
    }
    rms_degs = std::numeric_limits<float>::max();
    if (still_gyro.empty()) {
        return Vec3{};
    }

    // A worn head is never perfectly motionless, and users settle a moment
    // after pressing Enter, so take the quietest contiguous 1.5 s window.
    constexpr float kQuietWindowSeconds = 1.5f;
    std::vector<float> dt(still_gyro.size(), 1.0f / 476.0f);
    for (size_t i = 1; i < dt.size(); ++i) {
        dt[i] = sample_dt(still.samples[i - 1], still.samples[i]);
    }

    size_t best_start = 0;
    size_t best_end = 0;
    float best_rms = std::numeric_limits<float>::max();
    size_t left = 0;
    float window_seconds = 0.0f;
    Vec3 window_sum;
    float window_square_sum = 0.0f;
    for (size_t right = 0; right < still_gyro.size(); ++right) {
        window_seconds += dt[right];
        window_sum = add(window_sum, still_gyro[right]);
        window_square_sum += dot(still_gyro[right], still_gyro[right]);
        while (left < right && window_seconds - dt[left] >= kQuietWindowSeconds) {
            window_seconds -= dt[left];
            window_sum = subtract(window_sum, still_gyro[left]);
            window_square_sum -= dot(still_gyro[left], still_gyro[left]);
            ++left;
        }
        if (window_seconds >= kQuietWindowSeconds) {
            const float count = static_cast<float>(right - left + 1);
            const Vec3 window_mean = scale(window_sum, 1.0f / count);
            const float variance =
                std::max(0.0f, window_square_sum / count - dot(window_mean, window_mean));
            const float rms = std::sqrt(variance);
            if (rms < best_rms) {
                best_rms = rms;
                best_start = left;
                best_end = right + 1;
            }
        }
    }
    if (best_end <= best_start) {
        rms_degs = std::numeric_limits<float>::max();
        return Vec3{};
    }

    Vec3 bias_sum;
    for (size_t i = best_start; i < best_end; ++i) {
        bias_sum = add(bias_sum, still_gyro[i]);
    }
    rms_degs = best_rms;
    return scale(bias_sum, 1.0f / static_cast<float>(best_end - best_start));
}

Vec3 apply_sensor_to_head(const std::array<float, 9>& matrix, const Vec3& value) {
    return Vec3{matrix[0] * value.x + matrix[1] * value.y + matrix[2] * value.z,
                matrix[3] * value.x + matrix[4] * value.y + matrix[5] * value.z,
                matrix[6] * value.x + matrix[7] * value.y + matrix[8] * value.z};
}

OrientationCalibrationResult calibrate_orientation(const CalibrationPhaseData& still,
                                                   const CalibrationPhaseData& yaw,
                                                   const CalibrationPhaseData& nod,
                                                   const CalibrationPhaseData& tilt) {
    if (still.phase != CalibrationPhase::Still || yaw.phase != CalibrationPhase::Yaw ||
        nod.phase != CalibrationPhase::Nod || tilt.phase != CalibrationPhase::Tilt) {
        return fail("WRONG_PHASE", "calibration phases were supplied in the wrong order");
    }
    if (still.samples.size() < 100 || yaw.samples.size() < 100 || nod.samples.size() < 100 ||
        tilt.samples.size() < 100) {
        return fail("TOO_FEW_SAMPLES", "each calibration phase needs at least 100 IMU samples");
    }
    if (!valid_samples(still) || !valid_samples(yaw) || !valid_samples(nod) ||
        !valid_samples(tilt)) {
        return fail("INVALID_SAMPLE", "the IMU stream contained a non-finite sensor value");
    }
    if (phase_duration(still) < 2.0f) {
        return fail("STILL_TOO_SHORT", "hold still for the full three-second calibration window");
    }

    std::vector<Vec3> still_accel;
    still_accel.reserve(still.samples.size());
    for (const ImuSample& sample : still.samples) {
        still_accel.push_back(package_to_body(sample.accel_mps2));
    }
    float still_rms = 0.0f;
    const Vec3 bias_body = estimate_still_bias(still, still_rms);
    if (still_rms > 1.5f) {
        char text[192];
        std::snprintf(text, sizeof(text),
                      "the glasses measured %.2f deg/s of head movement at best (limit 1.5) - rest "
                      "your head back and hold still for the whole window",
                      still_rms);
        OrientationCalibrationResult failed = fail("HOLD_STILL", text);
        failed.still_gyro_rms_degs = still_rms;
        return failed;
    }
    const Vec3 up_accel = normalized(mean(still_accel));
    if (norm(up_accel) < 0.9f) {
        return fail("BAD_ACCEL", "gravity direction could not be measured - reconnect the glasses and retry");
    }

    OrientationCalibrationResult result;
    result.gyro_bias_degs = bias_body;
    result.still_gyro_rms_degs = still_rms;
    std::string code;
    std::string message;
    if (!analyze_motion_phase(yaw, bias_body, +1.0f, result.yaw, code, message) ||
        !analyze_motion_phase(nod, bias_body, -1.0f, result.nod, code, message) ||
        !analyze_motion_phase(tilt, bias_body, +1.0f, result.tilt, code, message)) {
        result.code = code;
        result.message = message;
        return result;
    }

    result.up_vs_yaw_deg = angle_deg(up_accel, result.yaw.axis_body);
    if (result.up_vs_yaw_deg > 8.0f) {
        return fail("UP_MISMATCH", "gravity and turn axis disagree - keep your head level and retry");
    }
    result.nod_level_error_deg = std::fabs(90.0f - angle_deg(result.nod.axis_body, up_accel));
    if (result.nod_level_error_deg > 8.0f) {
        return fail("NOD_NOT_LEVEL", "the nod was not level - keep your head level while nodding");
    }
    result.nod_tilt_error_deg =
        std::fabs(90.0f - angle_deg(result.nod.axis_body, result.tilt.axis_body));
    if (result.nod_tilt_error_deg > 20.0f) {
        return fail("NON_ORTHO", "nod and tilt were mixed - move one axis at a time and retry");
    }
    result.handedness_error_deg =
        angle_deg(cross(result.yaw.axis_body, result.nod.axis_body), result.tilt.axis_body);
    if (result.handedness_error_deg > 15.0f) {
        return fail("HANDEDNESS", "tilt direction did not match turn and nod - retry the tilt step");
    }

    const Vec3 up = normalized(add(up_accel, result.yaw.axis_body));
    Vec3 right = subtract(result.nod.axis_body, scale(up, dot(result.nod.axis_body, up)));
    right = normalized(right);
    Vec3 forward = normalized(cross(up, right));
    if (dot(forward, result.tilt.axis_body) < 0.0f) {
        forward = scale(forward, -1.0f);
        right = scale(right, -1.0f);
    }
    // Re-average the redundant measured forward direction, then rebuild a
    // strictly right-handed orthonormal basis with right x forward = up.
    forward = normalized(add(forward, result.tilt.axis_body));
    forward = normalized(subtract(forward, scale(up, dot(forward, up))));
    right = normalized(cross(forward, up));
    forward = normalized(cross(up, right));

    result.up_body = up;
    result.right_body = right;
    result.forward_body = forward;

    // Head coordinates are (right, forward, up). Keeping up in row 2 is
    // essential: Madgwick's gravity reference is +Z. The previous design doc
    // accidentally emitted (up, right, forward), which would put gravity on X.
    const float body_to_head[3][3] = {
        {right.x, right.y, right.z},
        {forward.x, forward.y, forward.z},
        {up.x, up.y, up.z},
    };
    const float package_to_body_matrix[3][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, 1.0f, 0.0f},
    };
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            float value = 0.0f;
            for (int k = 0; k < 3; ++k) {
                value += body_to_head[row][k] * package_to_body_matrix[k][col];
            }
            result.sensor_to_head[static_cast<size_t>(row * 3 + col)] = value;
        }
    }
    result.ok = true;
    result.code = "OK";
    result.message = "orientation calibration completed";
    return result;
}

bool save_orientation_calibration(const std::string& path, const OrientationCalibrationResult& result,
                                  std::string& error) {
    if (!result.ok) {
        error = "cannot save a failed calibration";
        return false;
    }
    const std::filesystem::path destination(path);
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::out | std::ios::trunc);
    if (!output) {
        error = "could not open calibration file for writing";
        return false;
    }
    output << std::fixed << std::setprecision(8)
           << "{\n  \"version\": 1,\n  \"sensor_to_head\": [\n    ";
    for (size_t i = 0; i < result.sensor_to_head.size(); ++i) {
        if (i != 0) {
            output << (i % 3 == 0 ? ",\n    " : ", ");
        }
        output << result.sensor_to_head[i];
    }
    output << "\n  ]\n}\n";
    if (!output) {
        error = "failed while writing calibration file";
        return false;
    }
    output.close();
    if (!output) {
        error = "failed while flushing calibration file";
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return false;
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "could not replace calibration file (Windows error " +
                std::to_string(GetLastError()) + ")";
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return false;
    }
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary, destination, rename_error);
    if (rename_error) {
        error = "could not replace calibration file: " + rename_error.message();
        std::filesystem::remove(temporary, rename_error);
        return false;
    }
#endif
    return true;
}

bool load_orientation_calibration(const std::string& path, std::array<float, 9>& matrix,
                                  std::string& error) {
    std::ifstream input(path, std::ios::in);
    if (!input) {
        error = "could not open calibration file";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const size_t version_key = text.find("\"version\"");
    const size_t version_colon =
        version_key == std::string::npos ? std::string::npos : text.find(':', version_key);
    if (version_colon == std::string::npos) {
        error = "calibration file is missing version";
        return false;
    }
    const char* version_cursor = text.data() + version_colon + 1;
    while (version_cursor < text.data() + text.size() &&
           (*version_cursor == ' ' || *version_cursor == '\t' || *version_cursor == '\r' ||
            *version_cursor == '\n')) {
        ++version_cursor;
    }
    int version = 0;
    const auto version_parse = std::from_chars(version_cursor, text.data() + text.size(), version);
    if (version_parse.ec != std::errc{} || version != 1) {
        error = "unsupported calibration file version";
        return false;
    }
    const size_t key = text.find("\"sensor_to_head\"");
    const size_t begin = key == std::string::npos ? std::string::npos : text.find('[', key);
    const size_t end = begin == std::string::npos ? std::string::npos : text.find(']', begin);
    if (begin == std::string::npos || end == std::string::npos) {
        error = "calibration file is missing sensor_to_head";
        return false;
    }

    std::array<float, 9> parsed{};
    const char* cursor = text.data() + begin + 1;
    const char* finish = text.data() + end;
    for (float& value : parsed) {
        while (cursor < finish && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
                                   *cursor == '\n' || *cursor == ',')) {
            ++cursor;
        }
        if (cursor >= finish) {
            error = "calibration matrix must contain exactly 9 numbers";
            return false;
        }
        char* number_end = nullptr;
        value = std::strtof(cursor, &number_end);
        if (number_end == cursor || number_end > finish || !std::isfinite(value)) {
            error = "calibration matrix contains an invalid number";
            return false;
        }
        cursor = number_end;
    }
    while (cursor < finish && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
                               *cursor == '\n' || *cursor == ',')) {
        ++cursor;
    }
    if (cursor != finish) {
        error = "calibration matrix must contain exactly 9 numbers";
        return false;
    }

    const Vec3 x = apply_sensor_to_head(parsed, Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 y = apply_sensor_to_head(parsed, Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 z = apply_sensor_to_head(parsed, Vec3{0.0f, 0.0f, 1.0f});
    const float determinant = dot(x, cross(y, z));
    const bool unit = std::fabs(norm(x) - 1.0f) < 0.02f && std::fabs(norm(y) - 1.0f) < 0.02f &&
                      std::fabs(norm(z) - 1.0f) < 0.02f;
    const bool orthogonal = std::fabs(dot(x, y)) < 0.02f && std::fabs(dot(x, z)) < 0.02f &&
                            std::fabs(dot(y, z)) < 0.02f;
    if (!unit || !orthogonal || determinant < 0.98f || determinant > 1.02f) {
        error = "sensor_to_head must be a proper orthonormal rotation matrix";
        return false;
    }
    matrix = parsed;
    return true;
}

}  // namespace gt
