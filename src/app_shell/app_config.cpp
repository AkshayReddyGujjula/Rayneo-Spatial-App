#include "app_shell/app_config.h"

#include "app_shell/app_layout.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gt {
namespace {

using Json = nlohmann::json;

constexpr size_t kMaxTextLength = 512;

Json window_to_json(const WindowPlacement& window) {
    return Json{
        {"x", window.x},
        {"y", window.y},
        {"width", window.width},
        {"height", window.height},
        {"maximized", window.maximized},
    };
}

Json policy_to_json(const LogRotationPolicy& policy) {
    return Json{
        {"max_bytes", policy.max_bytes},
        {"max_files", policy.max_files},
    };
}

bool valid_text(const std::string& value, size_t max_length) {
    if (value.empty() || value.size() > max_length) {
        return false;
    }
    for (const char character : value) {
        const unsigned char code = static_cast<unsigned char>(character);
        if (code < 0x20 || code == 0x7f) {
            return false;
        }
    }
    return true;
}

bool read_string(const Json& object, const char* key, std::string& out, std::string& error) {
    if (!object.contains(key)) {
        return true;
    }
    if (!object.at(key).is_string()) {
        error = std::string("field '") + key + "' must be a string";
        return false;
    }
    out = object.at(key).get<std::string>();
    return true;
}

bool read_int(const Json& object, const char* key, int& out, std::string& error) {
    if (!object.contains(key)) {
        return true;
    }
    if (!object.at(key).is_number_integer()) {
        error = std::string("field '") + key + "' must be an integer";
        return false;
    }
    int64_t wide = 0;
    try {
        wide = object.at(key).get<int64_t>();
    } catch (const std::exception&) {
        error = std::string("field '") + key + "' is out of range";
        return false;
    }
    if (wide < INT_MIN || wide > INT_MAX) {
        error = std::string("field '") + key + "' is out of range";
        return false;
    }
    out = static_cast<int>(wide);
    return true;
}

bool read_bool(const Json& object, const char* key, bool& out, std::string& error) {
    if (!object.contains(key)) {
        return true;
    }
    if (!object.at(key).is_boolean()) {
        error = std::string("field '") + key + "' must be true or false";
        return false;
    }
    out = object.at(key).get<bool>();
    return true;
}

bool read_policy(const Json& object, const char* key, LogRotationPolicy& out, std::string& error) {
    if (!object.contains(key)) {
        return true;
    }
    const Json& value = object.at(key);
    if (!value.is_object()) {
        error = std::string("field '") + key + "' must be an object";
        return false;
    }
    if (value.contains("max_bytes")) {
        if (!value.at("max_bytes").is_number_integer()) {
            error = std::string("field '") + key + ".max_bytes' must be an integer";
            return false;
        }
        int64_t bytes = 0;
        try {
            bytes = value.at("max_bytes").get<int64_t>();
        } catch (const std::exception&) {
            error = std::string("field '") + key + ".max_bytes' is out of range";
            return false;
        }
        if (bytes < 0) {
            error = std::string("field '") + key + ".max_bytes' must not be negative";
            return false;
        }
        out.max_bytes = static_cast<uint64_t>(bytes);
    }
    return read_int(value, "max_files", out.max_files, error);
}

bool clamp_loaded(AppConfig& config, std::string& error) {
    WindowPlacement& window = config.window;
    if (window.width < 320 || window.width > 32768) {
        error = "window width must be 320..32768";
        return false;
    }
    if (window.height < 240 || window.height > 32768) {
        error = "window height must be 240..32768";
        return false;
    }
    return true;
}

}  // namespace

const char* launch_mode_text(LaunchMode mode) {
    switch (mode) {
        case LaunchMode::Workspace:
            return "workspace";
        case LaunchMode::Preview:
            return "preview";
        case LaunchMode::None:
        default:
            return "none";
    }
}

bool parse_launch_mode(const std::string& text, LaunchMode& mode) {
    if (text == "workspace") {
        mode = LaunchMode::Workspace;
        return true;
    }
    if (text == "preview") {
        mode = LaunchMode::Preview;
        return true;
    }
    if (text == "none" || text.empty()) {
        mode = LaunchMode::None;
        return true;
    }
    return false;
}

bool validate_app_config(const AppConfig& config, std::string& error) {
    if (config.version != 1) {
        error = "unsupported config version";
        return false;
    }
    if (!valid_text(config.layout_path, kMaxTextLength)) {
        error = "layout_path must be 1..512 printable characters";
        return false;
    }
    if (!layout_preset_name_valid(config.active_preset, error)) {
        return false;
    }
    if (!valid_text(config.calibration_path, kMaxTextLength)) {
        error = "calibration_path must be 1..512 printable characters";
        return false;
    }
    if (!valid_text(config.log_dir, kMaxTextLength)) {
        error = "log_dir must be 1..512 printable characters";
        return false;
    }
    if (config.monitor_index < -1 || config.monitor_index > 15) {
        error = "monitor_index must be -1 (auto) or 0..15";
        return false;
    }
    if (config.health_poll_ms < kMinHealthPollMs || config.health_poll_ms > kMaxHealthPollMs) {
        error = "health_poll_ms must be 500..10000 (polling is capped at 2 Hz)";
        return false;
    }
    const LogRotationPolicy policies[2] = {config.engine_log_rotation, config.telemetry_rotation};
    for (const LogRotationPolicy& policy : policies) {
        if (policy.max_bytes < kMinLogBytes || policy.max_bytes > kMaxLogBytes) {
            error = "log max_bytes must be 65536..67108864";
            return false;
        }
        if (policy.max_files < 1 || policy.max_files > kMaxLogFiles) {
            error = "log max_files must be 1..9";
            return false;
        }
    }
    if (config.last_engine_error.size() > 2048) {
        error = "last_engine_error must be at most 2048 characters";
        return false;
    }
    if (config.window.width < 320 || config.window.width > 32768 ||
        config.window.height < 240 || config.window.height > 32768) {
        error = "window dimensions are out of range";
        return false;
    }
    return true;
}

std::string app_config_to_json_text(const AppConfig& config) {
    const Json document{
        {"version", config.version},
        {"layout_path", config.layout_path},
        {"active_preset", config.active_preset},
        {"calibration_path", config.calibration_path},
        {"log_dir", config.log_dir},
        {"monitor_index", config.monitor_index},
        {"last_mode", launch_mode_text(config.last_mode)},
        {"preview_without_head_tracking", config.preview_without_head_tracking},
        {"close_to_tray", config.close_to_tray},
        {"health_poll_ms", config.health_poll_ms},
        {"engine_log", policy_to_json(config.engine_log_rotation)},
        {"telemetry_log", policy_to_json(config.telemetry_rotation)},
        {"last_engine_error", config.last_engine_error},
        {"window", window_to_json(config.window)},
    };
    return document.dump(2) + "\n";
}

bool load_app_config(const std::filesystem::path& path, AppConfig& config, std::string& error) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        config = AppConfig{};
        return true;
    }
    std::ifstream input(path);
    if (!input) {
        error = "could not open config file";
        return false;
    }
    Json document = Json::parse(input, nullptr, false);
    if (document.is_discarded()) {
        error = "config file is not valid JSON";
        return false;
    }
    if (!document.is_object()) {
        error = "config root must be an object";
        return false;
    }

    AppConfig parsed;
    if (!read_int(document, "version", parsed.version, error) ||
        !read_string(document, "layout_path", parsed.layout_path, error) ||
        !read_string(document, "calibration_path", parsed.calibration_path, error) ||
        !read_string(document, "log_dir", parsed.log_dir, error) ||
        !read_int(document, "monitor_index", parsed.monitor_index, error) ||
        !read_bool(document, "preview_without_head_tracking",
                   parsed.preview_without_head_tracking, error) ||
        !read_bool(document, "close_to_tray", parsed.close_to_tray, error) ||
        !read_int(document, "health_poll_ms", parsed.health_poll_ms, error) ||
        !read_policy(document, "engine_log", parsed.engine_log_rotation, error) ||
        !read_policy(document, "telemetry_log", parsed.telemetry_rotation, error) ||
        !read_string(document, "last_engine_error", parsed.last_engine_error, error)) {
        return false;
    }
    if (document.contains("last_mode")) {
        if (!document.at("last_mode").is_string()) {
            error = "field 'last_mode' must be a string";
            return false;
        }
        if (!parse_launch_mode(document.at("last_mode").get<std::string>(), parsed.last_mode)) {
            error = "field 'last_mode' must be none, workspace or preview";
            return false;
        }
    }
    if (document.contains("active_preset")) {
        if (!document.at("active_preset").is_string()) {
            error = "field 'active_preset' must be a string";
            return false;
        }
        parsed.active_preset = document.at("active_preset").get<std::string>();
    }
    if (document.contains("window")) {
        const Json& window = document.at("window");
        if (!window.is_object()) {
            error = "field 'window' must be an object";
            return false;
        }
        if (!read_int(window, "x", parsed.window.x, error) ||
            !read_int(window, "y", parsed.window.y, error) ||
            !read_int(window, "width", parsed.window.width, error) ||
            !read_int(window, "height", parsed.window.height, error) ||
            !read_bool(window, "maximized", parsed.window.maximized, error)) {
            return false;
        }
    }
    if (!clamp_loaded(parsed, error) || !validate_app_config(parsed, error)) {
        return false;
    }
    config = parsed;
    return true;
}

bool save_app_config(const std::filesystem::path& path, const AppConfig& config, std::string& error) {
    if (!validate_app_config(config, error)) {
        return false;
    }
    std::error_code directory_error;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, directory_error);
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!output) {
            error = "could not open temporary config file";
            return false;
        }
        output << app_config_to_json_text(config);
        output.flush();
        if (!output) {
            error = "failed while writing config file";
            output.close();
            std::error_code remove_error;
            std::filesystem::remove(temporary, remove_error);
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "could not replace config file (Windows error " +
                std::to_string(GetLastError()) + ")";
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return false;
    }
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        error = "could not replace config file: " + rename_error.message();
        std::filesystem::remove(temporary, rename_error);
        return false;
    }
#endif
    return true;
}

RotationDecision decide_rotation(uint64_t current_bytes, const LogRotationPolicy& policy) {
    RotationDecision decision;
    int max_files = policy.max_files;
    if (max_files < 1) {
        max_files = 1;
    }
    if (max_files > kMaxLogFiles) {
        max_files = kMaxLogFiles;
    }
    decision.generations = max_files;
    if (current_bytes == 0) {
        decision.reason = "log file is empty";
        return decision;
    }
    if (current_bytes < policy.max_bytes) {
        decision.reason = "under the rotation threshold";
        return decision;
    }
    decision.rotate = true;
    decision.reason = "reached the rotation threshold";
    return decision;
}

std::filesystem::path rotated_log_path(const std::filesystem::path& path, int generation) {
    std::filesystem::path rotated = path;
    rotated += "." + std::to_string(generation);
    return rotated;
}

bool rotate_log_file(const std::filesystem::path& path, const LogRotationPolicy& policy,
                     std::string& error) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error) {
        return true;
    }
    const uint64_t size = std::filesystem::file_size(path, exists_error);
    if (exists_error) {
        return true;
    }
    const RotationDecision decision = decide_rotation(size, policy);
    if (!decision.rotate) {
        return true;
    }
    std::error_code ignored;
    std::filesystem::remove(rotated_log_path(path, decision.generations), ignored);
    for (int generation = decision.generations - 1; generation >= 1; --generation) {
        const std::filesystem::path from = rotated_log_path(path, generation);
        if (std::filesystem::exists(from, ignored)) {
            std::filesystem::rename(from, rotated_log_path(path, generation + 1), ignored);
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(path, rotated_log_path(path, 1), rename_error);
    if (rename_error) {
        error = "could not rotate log file: " + rename_error.message();
        return false;
    }
    return true;
}

std::string format_bytes(uint64_t bytes) {
    char buffer[64];
    if (bytes >= (1u << 20)) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MiB",
                      static_cast<double>(bytes) / static_cast<double>(1u << 20));
    } else if (bytes >= (1u << 10)) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KiB",
                      static_cast<double>(bytes) / static_cast<double>(1u << 10));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return std::string(buffer);
}

}  // namespace gt
