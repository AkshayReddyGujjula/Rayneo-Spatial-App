#pragma once

// Engine command line construction and status-file parsing.
//
// Kept free of windows.h so the app-model selftest can check the exact contract
// the engine sees: which switches appear in workspace mode, in renderer-only
// preview, and with and without head tracking. The controller never starts the
// engine in a mode it cannot describe: preview adds --no-virtual-displays and
// never pretends a Parsec-backed desktop exists.

#include "app/engine_protocol.h"
#include "app_shell/app_config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gt {

struct EngineLaunchCommand {
    std::string executable;         // UTF-8 path to spatial_desk.exe
    std::string working_directory;  // UTF-8 directory of the engine executable
    std::vector<std::string> arguments;
    std::string command_line;  // executable plus arguments, quoted for CreateProcessW
    std::string status_path;
    std::string engine_log_path;
    std::string telemetry_path;
    LaunchMode mode = LaunchMode::None;
    bool head_tracking = true;
};

// Win32 quoting (the CommandLineToArgvW rules): backslash runs before a quote
// are doubled, and the whole argument is wrapped when it contains whitespace.
std::string quote_command_line_argument(const std::string& value);

bool build_engine_launch_command(const std::string& engine_executable,
                                 const std::string& working_directory,
                                 const std::string& layout_path,
                                 const std::string& calibration_path,
                                 const std::string& status_path,
                                 const std::string& engine_log_path,
                                 const std::string& telemetry_path, LaunchMode mode,
                                 bool head_tracking, int monitor_index, float fov_deg,
                                 EngineLaunchCommand& command, std::string& error);

struct EngineStatusFile {
    bool parsed = false;
    std::string state;
    std::string mode;
    std::string error;
    int screens = 0;
    double fps = 0.0;
    int monitor = -1;
    int monitor_width = 0;
    int monitor_height = 0;
    bool vdd = false;
    bool imu = false;
    bool detached = false;
    double elapsed_s = 0.0;
    uint64_t updated_unix = 0;
};

bool parse_engine_status(const std::string& text, EngineStatusFile& status, std::string& error);
std::string engine_status_summary(const EngineStatusFile& status);

// Freshness gate for the status file: the engine rewrites it every second,
// so content older than max_age_s (or from the future, or missing its stamp)
// is a stale file from a previous run, not live state.
inline constexpr uint64_t kEngineStatusMaxAgeS = 3;
bool engine_status_is_fresh(const EngineStatusFile& status, uint64_t now_unix, uint64_t max_age_s);

}  // namespace gt
