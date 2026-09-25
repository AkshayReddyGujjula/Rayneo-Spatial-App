#include "app_shell/engine_commands.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace gt {
namespace {

bool parse_uint64(const std::string& text, uint64_t& value) {
    if (text.empty() || text[0] == '-') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || end == nullptr || *end != '\0') {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool parse_int(const std::string& text, int& value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || end == nullptr || *end != '\0') {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool parse_double(const std::string& text, double& value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_flag(const std::string& text, bool& value) {
    if (text == "1" || text == "true" || text == "yes") {
        value = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "no") {
        value = false;
        return true;
    }
    return false;
}

std::string number_text(double value) {
    char buffer[32];
    if (std::fabs(value - std::floor(value + 0.5)) < 1e-9) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    }
    return std::string(buffer);
}

}  // namespace

std::string quote_command_line_argument(const std::string& value) {
    if (!value.empty() && value.find_first_of(" \t\n\v\"") == std::string::npos) {
        return value;
    }
    std::string out;
    out.push_back('"');
    size_t index = 0;
    for (;;) {
        size_t backslashes = 0;
        while (index < value.size() && value[index] == '\\') {
            ++backslashes;
            ++index;
        }
        if (index == value.size()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (value[index] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            ++index;
            continue;
        }
        out.append(backslashes, '\\');
        out.push_back(value[index]);
        ++index;
    }
    out.push_back('"');
    return out;
}

bool build_engine_launch_command(const std::string& engine_executable,
                                 const std::string& working_directory,
                                 const std::string& layout_path,
                                 const std::string& calibration_path,
                                 const std::string& status_path,
                                 const std::string& engine_log_path,
                                 const std::string& telemetry_path, LaunchMode mode,
                                 bool head_tracking, int monitor_index, float fov_deg,
                                 EngineLaunchCommand& command, std::string& error,
                                 const ViewComfort& comfort) {
    if (engine_executable.empty()) {
        error = "engine executable path is empty";
        return false;
    }
    if (layout_path.empty()) {
        error = "layout path is empty";
        return false;
    }
    if (head_tracking && calibration_path.empty()) {
        error = "calibration path is empty";
        return false;
    }
    if (mode != LaunchMode::Workspace && mode != LaunchMode::Preview) {
        error = "engine mode must be workspace or preview";
        return false;
    }
    if (monitor_index < -1 || monitor_index > 15) {
        error = "monitor index must be -1 (auto) or 0..15";
        return false;
    }
    if (fov_deg != 0.0f && (!std::isfinite(fov_deg) || fov_deg < 20.0f || fov_deg > 150.0f)) {
        error = "field of view override must be 0 (layout default) or 20..150 degrees";
        return false;
    }
    if (!validate_view_comfort(comfort, error)) {
        return false;
    }

    EngineLaunchCommand built;
    built.executable = engine_executable;
    built.working_directory = working_directory;
    built.status_path = status_path;
    built.engine_log_path = engine_log_path;
    built.telemetry_path = telemetry_path;
    built.mode = mode;
    built.head_tracking = head_tracking;

    built.arguments.push_back(kEngineArgLayout);
    built.arguments.push_back(layout_path);
    if (head_tracking) {
        built.arguments.push_back(kEngineArgCalibration);
        built.arguments.push_back(calibration_path);
    } else {
        built.arguments.push_back(kEngineArgNoImu);
    }
    if (mode == LaunchMode::Preview) {
        built.arguments.push_back(kEngineArgNoVirtualDisplays);
    }
    if (monitor_index >= 0) {
        built.arguments.push_back(kEngineArgMonitor);
        built.arguments.push_back(std::to_string(monitor_index));
    }
    if (fov_deg != 0.0f) {
        built.arguments.push_back(kEngineArgFov);
        built.arguments.push_back(number_text(static_cast<double>(fov_deg)));
    }
    append_view_comfort_args(comfort, built.arguments);
    if (!telemetry_path.empty()) {
        built.arguments.push_back(kEngineArgLog);
        built.arguments.push_back(telemetry_path);
    }
    if (!status_path.empty()) {
        built.arguments.push_back(kEngineArgStatus);
        built.arguments.push_back(status_path);
    }

    built.command_line = quote_command_line_argument(engine_executable);
    for (const std::string& argument : built.arguments) {
        built.command_line.push_back(' ');
        built.command_line += quote_command_line_argument(argument);
    }
    command = std::move(built);
    return true;
}

bool parse_engine_status(const std::string& text, EngineStatusFile& status, std::string& error) {
    EngineStatusFile parsed;
    bool saw_state = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        start = end + 1;
        if (line.empty()) {
            continue;
        }
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key == kStatusKeyState) {
            parsed.state = value;
            saw_state = true;
        } else if (key == kStatusKeyMode) {
            parsed.mode = value;
        } else if (key == kStatusKeyError) {
            parsed.error = value;
        } else if (key == kStatusKeyScreens) {
            parse_int(value, parsed.screens);
        } else if (key == kStatusKeyFps) {
            parse_double(value, parsed.fps);
        } else if (key == kStatusKeyMonitor) {
            parse_int(value, parsed.monitor);
        } else if (key == kStatusKeyMonitorWidth) {
            parse_int(value, parsed.monitor_width);
        } else if (key == kStatusKeyMonitorHeight) {
            parse_int(value, parsed.monitor_height);
        } else if (key == kStatusKeyVdd) {
            parse_flag(value, parsed.vdd);
        } else if (key == kStatusKeyImu) {
            parse_flag(value, parsed.imu);
        } else if (key == kStatusKeyDetached) {
            parse_flag(value, parsed.detached);
        } else if (key == kStatusKeyElapsed) {
            parse_double(value, parsed.elapsed_s);
        } else if (key == kStatusKeyUpdated) {
            parse_uint64(value, parsed.updated_unix);
        }
    }
    if (!saw_state) {
        error = "status file has no state key";
        return false;
    }
    parsed.parsed = true;
    status = parsed;
    return true;
}

bool engine_status_is_fresh(const EngineStatusFile& status, uint64_t now_unix, uint64_t max_age_s) {
    if (!status.parsed || status.updated_unix == 0 || status.updated_unix > now_unix) {
        return false;
    }
    return now_unix - status.updated_unix <= max_age_s;
}

std::string engine_status_summary(const EngineStatusFile& status) {
    if (!status.parsed) {
        return "no status yet";
    }
    std::string summary = status.state;
    if (!status.mode.empty()) {
        summary += " | ";
        summary += status.mode;
    }
    if (status.state == kStatusFailed && !status.error.empty()) {
        summary += " | ";
        summary += status.error;
        return summary;
    }
    if (status.screens > 0) {
        summary += " | " + std::to_string(status.screens) + " screens";
    }
    if (status.fps > 0.0) {
        summary += " | " + number_text(status.fps) + " fps";
    }
    if (status.monitor >= 0) {
        summary += " | monitor " + std::to_string(status.monitor);
        if (status.monitor_width > 0 && status.monitor_height > 0) {
            summary += " " + std::to_string(status.monitor_width) + "x" +
                       std::to_string(status.monitor_height);
        }
    }
    if (status.mode == kModeWorkspace) {
        summary += status.vdd ? " | VDD desktops" : " | VDD unavailable";
    }
    if (status.detached) {
        summary += " | laptop display detached";
    }
    if (!status.imu) {
        summary += " | no head tracking";
    }
    return summary;
}

}  // namespace gt
