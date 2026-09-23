#include "layout/layout.h"

#include "util/utf8_path.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gt {
namespace {

using Json = nlohmann::json;

bool finite_between(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

Json to_json_value(const Layout& layout) {
    Json screens = Json::array();
    for (const ScreenLayout& screen : layout.screens) {
        screens.push_back({
            {"id", screen.id},
            {"vdd_index", screen.vdd_index},
            {"yaw_deg", screen.yaw_deg},
            {"pitch_deg", screen.pitch_deg},
            {"roll_deg", screen.roll_deg},
            {"distance_m", screen.distance_m},
            {"width_m", screen.width_m},
            {"height_m", screen.height_m},
            {"color", screen.color},
        });
    }
    return Json{
        {"version", layout.version},
        {"fov_deg", layout.fov_deg},
        {"screens", screens},
        {"capture_policy",
         {
             {"active_fps", layout.capture_policy.active_fps},
             {"mid_fps", layout.capture_policy.mid_fps},
             {"idle_fps", layout.capture_policy.idle_fps},
             {"enter_deg", layout.capture_policy.enter_deg},
             {"leave_deg", layout.capture_policy.leave_deg},
         }},
    };
}

template <typename T>
T required(const Json& object, const char* key) {
    if (!object.contains(key)) {
        throw std::runtime_error(std::string("missing required field '") + key + "'");
    }
    return object.at(key).get<T>();
}

}  // namespace

Layout default_layout() {
    Layout layout;
    layout.screens = {
        ScreenLayout{"left", 1, -45.0f, 0.0f, 0.0f, 2.0f, 1.6f, 0.9f,
                     {0.90f, 0.25f, 0.25f}},
        ScreenLayout{"centre", 2, 0.0f, 0.0f, 0.0f, 2.0f, 1.6f, 0.9f,
                     {0.24f, 0.45f, 0.95f}},
        ScreenLayout{"right", 3, 45.0f, 0.0f, 0.0f, 2.0f, 1.6f, 0.9f,
                     {0.25f, 0.85f, 0.35f}},
    };
    return layout;
}

bool validate_layout(const Layout& layout, std::string& error) {
    if (layout.version != 1) {
        error = "unsupported layout version";
        return false;
    }
    if (!finite_between(layout.fov_deg, 20.0f, 150.0f)) {
        error = "fov_deg must be between 20 and 150";
        return false;
    }
    if (layout.screens.empty() || layout.screens.size() > 8) {
        error = "screens must contain between 1 and 8 entries";
        return false;
    }
    std::set<std::string> ids;
    std::set<int> vdd_indices;
    for (const ScreenLayout& screen : layout.screens) {
        if (screen.id.empty() || screen.id.size() > 64) {
            error = "screen id must contain 1 to 64 characters";
            return false;
        }
        if (!ids.insert(screen.id).second) {
            error = "screen ids must be unique";
            return false;
        }
        if (screen.vdd_index < 0 || screen.vdd_index > 15 ||
            !vdd_indices.insert(screen.vdd_index).second) {
            error = "vdd_index values must be unique and between 0 and 15";
            return false;
        }
        if (!finite_between(screen.yaw_deg, -180.0f, 180.0f) ||
            !finite_between(screen.pitch_deg, -89.0f, 89.0f) ||
            !finite_between(screen.roll_deg, -180.0f, 180.0f)) {
            error = "screen yaw/roll must be -180..180 and pitch must be -89..89";
            return false;
        }
        if (!finite_between(screen.distance_m, 0.25f, 20.0f) ||
            !finite_between(screen.width_m, 0.1f, 10.0f) ||
            !finite_between(screen.height_m, 0.1f, 10.0f)) {
            error = "screen distance must be 0.25..20m and dimensions 0.1..10m";
            return false;
        }
        for (float component : screen.color) {
            if (!finite_between(component, 0.0f, 1.0f)) {
                error = "screen color components must be between 0 and 1";
                return false;
            }
        }
    }
    const CapturePolicy& policy = layout.capture_policy;
    if (policy.idle_fps < 1 || policy.mid_fps < policy.idle_fps ||
        policy.active_fps < policy.mid_fps || policy.active_fps > 240) {
        error = "capture FPS must satisfy 1 <= idle <= mid <= active <= 240";
        return false;
    }
    if (!finite_between(policy.leave_deg, 0.0f, 179.0f) ||
        !finite_between(policy.enter_deg, 0.0f, 179.0f) ||
        policy.enter_deg <= policy.leave_deg) {
        error = "capture enter_deg must be greater than leave_deg";
        return false;
    }
    return true;
}

bool load_layout(const std::string& path, Layout& layout, std::string& error) {
    try {
        std::ifstream input(path_from_utf8(path));
        if (!input) {
            error = "could not open layout file";
            return false;
        }
        Json document;
        input >> document;
        if (!document.is_object()) {
            error = "layout root must be an object";
            return false;
        }

        Layout parsed;
        parsed.version = required<int>(document, "version");
        parsed.fov_deg = required<float>(document, "fov_deg");
        const Json& screens = document.at("screens");
        if (!screens.is_array()) {
            error = "screens must be an array";
            return false;
        }
        for (const Json& value : screens) {
            ScreenLayout screen;
            screen.id = required<std::string>(value, "id");
            screen.vdd_index = required<int>(value, "vdd_index");
            screen.yaw_deg = required<float>(value, "yaw_deg");
            screen.pitch_deg = required<float>(value, "pitch_deg");
            screen.roll_deg = required<float>(value, "roll_deg");
            screen.distance_m = required<float>(value, "distance_m");
            screen.width_m = required<float>(value, "width_m");
            screen.height_m = value.value("height_m", screen.width_m * 9.0f / 16.0f);
            if (value.contains("color")) {
                screen.color = value.at("color").get<std::array<float, 3>>();
            }
            parsed.screens.push_back(std::move(screen));
        }
        if (document.contains("capture_policy")) {
            const Json& policy = document.at("capture_policy");
            parsed.capture_policy.active_fps = required<int>(policy, "active_fps");
            parsed.capture_policy.mid_fps = required<int>(policy, "mid_fps");
            parsed.capture_policy.idle_fps = required<int>(policy, "idle_fps");
            parsed.capture_policy.enter_deg = required<float>(policy, "enter_deg");
            parsed.capture_policy.leave_deg = required<float>(policy, "leave_deg");
        }
        if (!validate_layout(parsed, error)) {
            return false;
        }
        layout = std::move(parsed);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("invalid layout JSON: ") + exception.what();
        return false;
    }
}

bool save_layout(const std::string& path, const Layout& layout, std::string& error) {
    if (!validate_layout(layout, error)) {
        return false;
    }
    const std::filesystem::path destination = path_from_utf8(path);
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::out | std::ios::trunc);
    if (!output) {
        error = "could not open temporary layout file";
        return false;
    }
    output << to_json_value(layout).dump(2) << '\n';
    output.close();
    if (!output) {
        error = "failed while writing layout file";
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return false;
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "could not replace layout file (Windows error " + std::to_string(GetLastError()) + ")";
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return false;
    }
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary, destination, rename_error);
    if (rename_error) {
        error = "could not replace layout file: " + rename_error.message();
        std::filesystem::remove(temporary, rename_error);
        return false;
    }
#endif
    return true;
}

}  // namespace gt
