#include "app_shell/app_config.h"

#include "app_shell/app_layout.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
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

bool read_float(const Json& object, const char* key, float& out, std::string& error);

Json splits_to_json(const SplitFractions& splits) {
    return Json{
        {"column", splits.column},
        {"left", {splits.left[0], splits.left[1], splits.left[2]}},
        {"right", {splits.right[0], splits.right[1], splits.right[2]}},
    };
}

bool read_splits(const Json& document, SplitFractions& out, std::string& error) {
    if (!document.contains("splits")) {
        return true;
    }
    const Json& splits = document.at("splits");
    if (!splits.is_object()) {
        error = "field 'splits' must be an object";
        return false;
    }
    if (!read_float(splits, "column", out.column, error)) {
        return false;
    }
    for (const char* key : {"left", "right"}) {
        if (!splits.contains(key)) {
            continue;
        }
        const Json& triple = splits.at(key);
        if (!triple.is_array() || triple.size() != 3) {
            error = std::string("field 'splits.") + key + "' must be an array of 3 numbers";
            return false;
        }
        float* target = key[0] == 'l' ? out.left : out.right;
        for (size_t i = 0; i < 3; ++i) {
            if (!triple[i].is_number()) {
                error = std::string("field 'splits.") + key + "' must be an array of 3 numbers";
                return false;
            }
            target[i] = triple[i].get<float>();
        }
    }
    return true;
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

bool read_float(const Json& object, const char* key, float& out, std::string& error) {
    if (!object.contains(key)) {
        return true;
    }
    if (!object.at(key).is_number()) {
        error = std::string("field '") + key + "' must be a number";
        return false;
    }
    out = object.at(key).get<float>();
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

Json comfort_to_json(const ViewComfort& comfort) {
    return Json{
        {"stabilise", stabilise_level_name(comfort.stabilise_level)},
        {"dim_mode", dim_mode_name(comfort.dim_mode)},
        {"screen_brightness_pct", comfort.screen_brightness_pct},
        {"focus_dim_pct", comfort.focus_dim_pct},
        {"night_tint", comfort.night_tint},
        {"night_tint_pct", comfort.night_tint_pct},
    };
}

// Optional object; a file written before it existed loads the defaults.
bool read_comfort(const Json& document, ViewComfort& comfort, std::string& error) {
    if (!document.contains("view_comfort")) {
        return true;
    }
    const Json& node = document.at("view_comfort");
    if (!node.is_object()) {
        error = "field 'view_comfort' must be an object";
        return false;
    }
    if (node.contains("stabilise")) {
        const Json& value = node.at("stabilise");
        bool matched = false;
        for (int level = 0; value.is_string() && level < kComfortStabiliseLevels; ++level) {
            if (value.get<std::string>() == stabilise_level_name(level)) {
                comfort.stabilise_level = level;
                matched = true;
            }
        }
        if (!matched) {
            error = "view_comfort.stabilise must be off, low, medium, high or ultra";
            return false;
        }
    }
    if (node.contains("dim_mode")) {
        const Json& value = node.at("dim_mode");
        if (!value.is_string() || !parse_dim_mode(value.get<std::string>(), comfort.dim_mode)) {
            error = "view_comfort.dim_mode must be off, manual or focus";
            return false;
        }
    }
    if (node.contains("screen_brightness_pct")) {
        const Json& list = node.at("screen_brightness_pct");
        if (!list.is_array() || list.size() > comfort.screen_brightness_pct.size()) {
            error = "view_comfort.screen_brightness_pct must be an array of up to 8 values";
            return false;
        }
        for (size_t i = 0; i < list.size(); ++i) {
            if (!list[i].is_number_integer()) {
                error = "view_comfort.screen_brightness_pct values must be integers";
                return false;
            }
            comfort.screen_brightness_pct[i] = list[i].get<int>();
        }
    }
    return read_int(node, "focus_dim_pct", comfort.focus_dim_pct, error) &&
           read_bool(node, "night_tint", comfort.night_tint, error) &&
           read_int(node, "night_tint_pct", comfort.night_tint_pct, error);
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
    const float* parts[7] = {&config.splits.column, &config.splits.left[0], &config.splits.left[1],
                             &config.splits.left[2], &config.splits.right[0], &config.splits.right[1],
                             &config.splits.right[2]};
    for (const float* part : parts) {
        if (!std::isfinite(*part) || *part < -1.0f || *part > 1.0f) {
            error = "split fractions must be -1 (automatic) or a 0..1 share";
            return false;
        }
    }
    if (config.health_poll_ms < kMinHealthPollMs || config.health_poll_ms > kMaxHealthPollMs) {
        error = "health_poll_ms must be 500..10000 (polling is capped at 2 Hz)";
        return false;
    }
    if (config.last_engine_error.size() > 2048) {
        error = "last_engine_error must be at most 2048 characters";
        return false;
    }
    if (!validate_view_comfort(config.comfort, error)) {
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
        {"last_engine_error", config.last_engine_error},
        {"window", window_to_json(config.window)},
        {"splits", splits_to_json(config.splits)},
        {"view_comfort", comfort_to_json(config.comfort)},
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
    if (!read_splits(document, parsed.splits, error)) {
        return false;
    }
    if (!read_comfort(document, parsed.comfort, error)) {
        return false;
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
