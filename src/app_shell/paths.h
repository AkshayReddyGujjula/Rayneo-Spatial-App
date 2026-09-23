#pragma once

// Path discovery for the controller.
//
// Nothing is assumed about where the executables live: the controller walks up
// from its own image until it finds config/layouts/default.json (or a config
// directory), which makes a portable staging folder work exactly like a
// repository checkout. The engine executable is looked for next to the
// controller first, then at the discovered root.

#include "app_shell/app_config.h"

#include <filesystem>
#include <string>

namespace gt {

struct Paths {
    std::filesystem::path executable;
    std::filesystem::path executable_dir;
    std::filesystem::path root;
    std::filesystem::path config_dir;
    std::filesystem::path app_config_path;
    std::filesystem::path layout_path;
    std::filesystem::path calibration_path;
    std::filesystem::path log_dir;
    std::filesystem::path engine_executable;
    std::filesystem::path calibration_executable;
    std::filesystem::path engine_log;
    std::filesystem::path telemetry_log;
    std::filesystem::path engine_status;
    bool engine_found = false;
    bool calibration_tool_found = false;
    bool portable = false;  // executables live inside the discovered root
};

std::string utf8_from_wide(const std::wstring& text);
std::wstring wide_from_utf8(const std::string& text);

// Discovers the controller image, its directory, the root and config directory.
bool resolve_base_paths(Paths& paths, std::string& error);

// Resolves the config-relative paths (layout, calibration, logs, executables).
void apply_config_paths(Paths& paths, const AppConfig& config);

// Joins a config value with the root unless it is already absolute.
std::filesystem::path resolve_config_path(const std::filesystem::path& root,
                                          const std::string& value);

std::wstring widen(const std::filesystem::path& path);
std::string narrow_utf8(const std::filesystem::path& path);

}  // namespace gt
