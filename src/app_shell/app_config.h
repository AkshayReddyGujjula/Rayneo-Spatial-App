#pragma once

// Controller preferences (config/app.json).
//
// The file is the controller's own state, not the layout: layouts stay in
// config/layouts/*.json and keep the gt::Layout schema so spatial_desk.exe can
// keep loading them unchanged. Everything here is validated before it is
// written, and writes go through a temporary file plus MoveFileEx so a crash or
// a full disk can never leave a half-written preference file behind.

#include <cstdint>
#include <filesystem>
#include <string>

namespace gt {

enum class LaunchMode {
    None,
    Workspace,
    Preview,
};

struct WindowPlacement {
    int x = 120;
    int y = 90;
    int width = 1180;
    int height = 860;
    bool maximized = false;
};

struct LogRotationPolicy {
    uint64_t max_bytes = 1u << 20;  // 1 MiB
    int max_files = 3;              // rotated generations kept (1..9)
};

struct AppConfig {
    int version = 1;
    std::string layout_path = "config/layouts/default.json";
    std::string calibration_path = "config/orientation.json";
    std::string log_dir = "logs";
    int monitor_index = -1;  // -1 = let the engine pick the glasses display
    LaunchMode last_mode = LaunchMode::None;
    bool preview_without_head_tracking = false;
    bool close_to_tray = true;
    int health_poll_ms = 1000;  // clamped to kMinHealthPollMs (2 Hz) or slower
    LogRotationPolicy engine_log_rotation{1u << 20, 3};
    LogRotationPolicy telemetry_rotation{4u << 20, 3};
    std::string last_engine_error;
    WindowPlacement window;
};

inline constexpr int kMinHealthPollMs = 500;
inline constexpr int kMaxHealthPollMs = 10000;
inline constexpr uint64_t kMinLogBytes = 64u << 10;
inline constexpr uint64_t kMaxLogBytes = 64u << 20;
inline constexpr int kMaxLogFiles = 9;

const char* launch_mode_text(LaunchMode mode);
bool parse_launch_mode(const std::string& text, LaunchMode& mode);

bool validate_app_config(const AppConfig& config, std::string& error);
std::string app_config_to_json_text(const AppConfig& config);

// Missing files load as defaults and report success; malformed or invalid files
// fail with a reason and leave the destination untouched.
bool load_app_config(const std::filesystem::path& path, AppConfig& config, std::string& error);
bool save_app_config(const std::filesystem::path& path, const AppConfig& config, std::string& error);

// Rotation is decided before a launch, never while the engine holds the file
// open: spatial_desk.exe appends to its CSV, so the controller rotates the
// previous run's file out of the way and the engine opens a fresh one.
struct RotationDecision {
    bool rotate = false;
    int generations = 0;
    std::string reason;
};

RotationDecision decide_rotation(uint64_t current_bytes, const LogRotationPolicy& policy);
std::filesystem::path rotated_log_path(const std::filesystem::path& path, int generation);
bool rotate_log_file(const std::filesystem::path& path, const LogRotationPolicy& policy,
                     std::string& error);

std::string format_bytes(uint64_t bytes);

}  // namespace gt
