#include "app_shell/paths.h"

#include "app/engine_protocol.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gt {
namespace {

std::filesystem::path executable_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

std::filesystem::path discover_root(const std::filesystem::path& start) {
    for (std::filesystem::path candidate = start;; candidate = candidate.parent_path()) {
        if (path_exists(candidate / "config" / "layouts" / "default.json")) {
            return candidate;
        }
        if (path_exists(candidate / "config")) {
            return candidate;
        }
        if (candidate.empty() || candidate == candidate.parent_path()) {
            break;
        }
    }
    return start;
}

}  // namespace

std::string utf8_from_wide(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring wide_from_utf8(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring widen(const std::filesystem::path& path) {
    return path.wstring();
}

std::string narrow_utf8(const std::filesystem::path& path) {
    return utf8_from_wide(path.wstring());
}

std::filesystem::path resolve_config_path(const std::filesystem::path& root,
                                          const std::string& value) {
    std::filesystem::path path(wide_from_utf8(value));
    if (path.is_absolute()) {
        return path;
    }
    return root / path;
}

bool resolve_base_paths(Paths& paths, std::string& error) {
    paths.executable = executable_path();
    if (paths.executable.empty()) {
        error = "could not locate the controller executable";
        return false;
    }
    paths.executable_dir = paths.executable.parent_path();
    paths.root = discover_root(paths.executable_dir);
    paths.config_dir = paths.root / "config";
    paths.app_config_path = paths.config_dir / "app.json";
    return true;
}

void apply_config_paths(Paths& paths, const AppConfig& config) {
    paths.layout_path = resolve_config_path(paths.root, config.layout_path);
    paths.calibration_path = resolve_config_path(paths.root, config.calibration_path);
    paths.log_dir = resolve_config_path(paths.root, config.log_dir);
    paths.engine_log = paths.log_dir / "engine.log";
    paths.telemetry_log = paths.log_dir / "telemetry.csv";
    paths.engine_status = paths.log_dir / "engine-status.txt";

    const std::filesystem::path beside_app = paths.executable_dir / kEngineExecutableName;
    const std::filesystem::path at_root = paths.root / kEngineExecutableName;
    if (path_exists(beside_app)) {
        paths.engine_executable = beside_app;
        paths.portable = paths.executable_dir == paths.root ||
                         path_exists(paths.executable_dir / "config" / "layouts" / "default.json");
    } else if (path_exists(at_root)) {
        paths.engine_executable = at_root;
    } else {
        paths.engine_executable = beside_app;
    }
    paths.engine_found = path_exists(paths.engine_executable);

    const std::filesystem::path calibration_beside_app =
        paths.executable_dir / kCalibrationExecutableName;
    const std::filesystem::path calibration_at_root = paths.root / kCalibrationExecutableName;
    if (path_exists(calibration_beside_app)) {
        paths.calibration_executable = calibration_beside_app;
    } else {
        paths.calibration_executable = calibration_at_root;
    }
    paths.calibration_tool_found = path_exists(paths.calibration_executable);
}

}  // namespace gt
