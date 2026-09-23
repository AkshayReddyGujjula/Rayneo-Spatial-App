#include "app_shell/app_layout.h"

#include "util/utf8_path.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <set>

namespace gt {
namespace {

struct PresetSpec {
    LayoutPreset preset;
    const char* name;
    const char* label;
};

const PresetSpec kPresets[] = {
    {LayoutPreset::Single, "single", "Single centred"},
    {LayoutPreset::TripleArc, "triple-arc", "Triple arc (default)"},
    {LayoutPreset::QuadArc, "quad-arc", "Quad arc"},
    {LayoutPreset::FiveArc, "five-arc", "Five arc"},
    {LayoutPreset::WideArc, "wide-arc", "Wide arc (3 x 4 m)"},
};

const std::array<std::array<float, 3>, 8> kScreenColors{{
    {0.90f, 0.25f, 0.25f},
    {0.24f, 0.45f, 0.95f},
    {0.25f, 0.85f, 0.35f},
    {0.85f, 0.70f, 0.25f},
    {0.65f, 0.35f, 0.90f},
    {0.25f, 0.80f, 0.85f},
    {0.90f, 0.50f, 0.25f},
    {0.55f, 0.65f, 0.80f},
}};

ScreenLayout make_screen(const std::string& id, int vdd_index, float yaw_deg, float distance_m,
                         float width_m, size_t color_index, float pitch_deg = 0.0f) {
    ScreenLayout screen;
    screen.id = id;
    screen.vdd_index = vdd_index;
    screen.yaw_deg = yaw_deg;
    screen.pitch_deg = pitch_deg;
    screen.roll_deg = 0.0f;
    screen.distance_m = distance_m;
    screen.width_m = width_m;
    screen.height_m = width_m * 9.0f / 16.0f;
    screen.color = kScreenColors[color_index % kScreenColors.size()];
    return screen;
}

float wrap_deg(float degrees) {
    while (degrees > 180.0f) {
        degrees -= 360.0f;
    }
    while (degrees < -180.0f) {
        degrees += 360.0f;
    }
    return degrees;
}

void normalize_fps(int& idle, int& mid, int& active, int authoritative) {
    if (idle < 1) {
        idle = 1;
    }
    // The field the user moved is authoritative; the coupled fields follow it so
    // the slider never fights the pointer, and the ordering 1 <= idle <= mid <=
    // active still holds for gt::validate_layout.
    if (authoritative == 0) {
        if (mid < idle) {
            mid = idle;
        }
        if (active < mid) {
            active = mid;
        }
    } else if (authoritative == 1) {
        if (idle > mid) {
            idle = mid;
        }
        if (active < mid) {
            active = mid;
        }
    } else {
        if (mid > active) {
            mid = active;
        }
        if (idle > mid) {
            idle = mid;
        }
    }
    if (active > 240) {
        active = 240;
    }
    if (mid > active) {
        mid = active;
    }
    if (idle > mid) {
        idle = mid;
    }
}

}  // namespace

const char* layout_preset_name(LayoutPreset preset) {
    for (const PresetSpec& spec : kPresets) {
        if (spec.preset == preset) {
            return spec.name;
        }
    }
    return "unknown";
}

const char* layout_preset_label(LayoutPreset preset) {
    for (const PresetSpec& spec : kPresets) {
        if (spec.preset == preset) {
            return spec.label;
        }
    }
    return "Unknown";
}

std::vector<LayoutPreset> layout_presets() {
    std::vector<LayoutPreset> presets;
    presets.reserve(std::size(kPresets));
    for (const PresetSpec& spec : kPresets) {
        presets.push_back(spec.preset);
    }
    return presets;
}

Layout preset_layout(LayoutPreset preset) {
    Layout layout;
    switch (preset) {
        case LayoutPreset::Single:
            layout.screens = {make_screen("centre", 1, 0.0f, 2.0f, 1.6f, 1)};
            break;
        case LayoutPreset::TripleArc:
            layout = default_layout();
            break;
        case LayoutPreset::QuadArc:
            layout.screens = {
                make_screen("left", 1, -67.5f, 2.0f, 1.6f, 0),
                make_screen("centre-left", 2, -22.5f, 2.0f, 1.6f, 3),
                make_screen("centre-right", 3, 22.5f, 2.0f, 1.6f, 1),
                make_screen("right", 4, 67.5f, 2.0f, 1.6f, 2),
            };
            break;
        case LayoutPreset::FiveArc:
            layout.screens = {
                make_screen("far-left", 1, -90.0f, 2.2f, 1.6f, 0),
                make_screen("left", 2, -45.0f, 2.0f, 1.6f, 3),
                make_screen("centre", 3, 0.0f, 2.0f, 1.6f, 1),
                make_screen("right", 4, 45.0f, 2.0f, 1.6f, 2),
                make_screen("far-right", 5, 90.0f, 2.2f, 1.6f, 4),
            };
            break;
        case LayoutPreset::WideArc:
            layout.screens = {
                make_screen("left", 1, -55.0f, 3.0f, 4.0f, 0),
                make_screen("centre", 2, 0.0f, 3.0f, 4.0f, 1),
                make_screen("right", 3, 55.0f, 3.0f, 4.0f, 2),
            };
            break;
    }
    return layout;
}

int next_free_vdd_index(const Layout& layout) {
    std::set<int> used;
    for (const ScreenLayout& screen : layout.screens) {
        used.insert(screen.vdd_index);
    }
    for (int index = 0; index <= 15; ++index) {
        if (used.count(index) == 0) {
            return index;
        }
    }
    return -1;
}

std::string next_screen_id(const Layout& layout) {
    std::set<std::string> used;
    for (const ScreenLayout& screen : layout.screens) {
        used.insert(screen.id);
    }
    for (int index = 1; index <= 99; ++index) {
        const std::string candidate = "screen-" + std::to_string(index);
        if (used.count(candidate) == 0) {
            return candidate;
        }
    }
    return std::string();
}

bool add_screen(Layout& layout, std::string& error) {
    if (layout.screens.size() >= 8) {
        error = "the layout already has the maximum of 8 screens";
        return false;
    }
    const std::string id = next_screen_id(layout);
    const int vdd_index = next_free_vdd_index(layout);
    if (id.empty() || vdd_index < 0) {
        error = "no free screen id or virtual-display index is available";
        return false;
    }
    ScreenLayout screen;
    if (layout.screens.empty()) {
        screen = make_screen(id, vdd_index, 0.0f, 2.0f, 1.6f, 1);
    } else {
        const ScreenLayout& last = layout.screens.back();
        screen = make_screen(id, vdd_index, std::min(180.0f, last.yaw_deg + 30.0f),
                             last.distance_m, last.width_m, layout.screens.size());
        screen.height_m = last.height_m;
        screen.pitch_deg = last.pitch_deg;
        screen.roll_deg = last.roll_deg;
        screen.color = kScreenColors[layout.screens.size() % kScreenColors.size()];
    }
    layout.screens.push_back(screen);
    if (!validate_layout(layout, error)) {
        layout.screens.pop_back();
        return false;
    }
    return true;
}

bool remove_screen(Layout& layout, size_t index, std::string& error) {
    if (layout.screens.size() <= 1) {
        error = "the layout must keep at least one screen";
        return false;
    }
    if (index >= layout.screens.size()) {
        error = "screen index is out of range";
        return false;
    }
    const ScreenLayout removed = layout.screens[index];
    layout.screens.erase(layout.screens.begin() + static_cast<std::ptrdiff_t>(index));
    if (!validate_layout(layout, error)) {
        layout.screens.insert(layout.screens.begin() + static_cast<std::ptrdiff_t>(index), removed);
        return false;
    }
    return true;
}

const wchar_t* layout_field_label(LayoutField field) {
    switch (field) {
        case LayoutField::Yaw:
            return L"Yaw";
        case LayoutField::Pitch:
            return L"Pitch";
        case LayoutField::Roll:
            return L"Roll";
        case LayoutField::Distance:
            return L"Distance";
        case LayoutField::Width:
            return L"Width";
        case LayoutField::Height:
            return L"Height";
        case LayoutField::Fov:
            return L"Field of view";
        case LayoutField::ActiveFps:
            return L"Active capture";
        case LayoutField::MidFps:
            return L"Mid capture";
        case LayoutField::IdleFps:
            return L"Idle capture";
        case LayoutField::EnterDeg:
            return L"Idle beyond";
        case LayoutField::LeaveDeg:
            return L"Active within";
        case LayoutField::Count:
        default:
            return L"";
    }
}

const char* layout_field_key(LayoutField field) {
    switch (field) {
        case LayoutField::Yaw:
            return "yaw_deg";
        case LayoutField::Pitch:
            return "pitch_deg";
        case LayoutField::Roll:
            return "roll_deg";
        case LayoutField::Distance:
            return "distance_m";
        case LayoutField::Width:
            return "width_m";
        case LayoutField::Height:
            return "height_m";
        case LayoutField::Fov:
            return "fov_deg";
        case LayoutField::ActiveFps:
            return "active_fps";
        case LayoutField::MidFps:
            return "mid_fps";
        case LayoutField::IdleFps:
            return "idle_fps";
        case LayoutField::EnterDeg:
            return "enter_deg";
        case LayoutField::LeaveDeg:
            return "leave_deg";
        case LayoutField::Count:
        default:
            return "";
    }
}

float layout_field_value(const Layout& layout, const ScreenLayout& screen, LayoutField field) {
    switch (field) {
        case LayoutField::Yaw:
            return screen.yaw_deg;
        case LayoutField::Pitch:
            return screen.pitch_deg;
        case LayoutField::Roll:
            return screen.roll_deg;
        case LayoutField::Distance:
            return screen.distance_m;
        case LayoutField::Width:
            return screen.width_m;
        case LayoutField::Height:
            return screen.height_m;
        case LayoutField::Fov:
            return layout.fov_deg;
        case LayoutField::ActiveFps:
            return static_cast<float>(layout.capture_policy.active_fps);
        case LayoutField::MidFps:
            return static_cast<float>(layout.capture_policy.mid_fps);
        case LayoutField::IdleFps:
            return static_cast<float>(layout.capture_policy.idle_fps);
        case LayoutField::EnterDeg:
            return layout.capture_policy.enter_deg;
        case LayoutField::LeaveDeg:
            return layout.capture_policy.leave_deg;
        case LayoutField::Count:
        default:
            return 0.0f;
    }
}

bool layout_field_range(LayoutField field, float& minimum, float& maximum, float& step) {
    switch (field) {
        case LayoutField::Yaw:
            minimum = -180.0f;
            maximum = 180.0f;
            step = 1.0f;
            return true;
        case LayoutField::Pitch:
            minimum = -89.0f;
            maximum = 89.0f;
            step = 1.0f;
            return true;
        case LayoutField::Roll:
            minimum = -180.0f;
            maximum = 180.0f;
            step = 1.0f;
            return true;
        case LayoutField::Distance:
            minimum = 0.25f;
            maximum = 20.0f;
            step = 0.05f;
            return true;
        case LayoutField::Width:
        case LayoutField::Height:
            minimum = 0.1f;
            maximum = 10.0f;
            step = 0.05f;
            return true;
        case LayoutField::Fov:
            minimum = 20.0f;
            maximum = 150.0f;
            step = 1.0f;
            return true;
        case LayoutField::ActiveFps:
        case LayoutField::MidFps:
        case LayoutField::IdleFps:
            minimum = 1.0f;
            maximum = 240.0f;
            step = 1.0f;
            return true;
        case LayoutField::EnterDeg:
        case LayoutField::LeaveDeg:
            minimum = 0.0f;
            maximum = 179.0f;
            step = 1.0f;
            return true;
        case LayoutField::Count:
        default:
            return false;
    }
}

float layout_slider_fraction(LayoutField field, float value) {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float step = 0.0f;
    if (!layout_field_range(field, minimum, maximum, step) || maximum <= minimum) {
        return 0.0f;
    }
    const float bounded = std::clamp(value, minimum, maximum);
    if (field == LayoutField::Distance) {
        return std::log(bounded / minimum) / std::log(maximum / minimum);
    }
    return (bounded - minimum) / (maximum - minimum);
}

float layout_slider_value(LayoutField field, float fraction) {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float step = 0.0f;
    if (!layout_field_range(field, minimum, maximum, step) || maximum <= minimum) {
        return 0.0f;
    }
    const float bounded = std::clamp(fraction, 0.0f, 1.0f);
    if (field == LayoutField::Distance) {
        return minimum * std::pow(maximum / minimum, bounded);
    }
    return minimum + bounded * (maximum - minimum);
}

bool set_layout_field(Layout& layout, ScreenLayout& screen, LayoutField field, float value,
                      std::string& error) {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float step = 0.0f;
    if (!layout_field_range(field, minimum, maximum, step)) {
        error = "unknown layout field";
        return false;
    }
    if (!std::isfinite(value)) {
        error = "value must be finite";
        return false;
    }
    if (field == LayoutField::Yaw || field == LayoutField::Roll) {
        value = wrap_deg(value);
    } else {
        value = std::clamp(value, minimum, maximum);
    }

    switch (field) {
        case LayoutField::Yaw:
            screen.yaw_deg = value;
            break;
        case LayoutField::Pitch:
            screen.pitch_deg = value;
            break;
        case LayoutField::Roll:
            screen.roll_deg = value;
            break;
        case LayoutField::Distance:
            screen.distance_m = value;
            break;
        case LayoutField::Width:
            screen.width_m = value;
            break;
        case LayoutField::Height:
            screen.height_m = value;
            break;
        case LayoutField::Fov:
            layout.fov_deg = value;
            break;
        case LayoutField::ActiveFps: {
            layout.capture_policy.active_fps = static_cast<int>(std::lround(value));
            normalize_fps(layout.capture_policy.idle_fps, layout.capture_policy.mid_fps,
                          layout.capture_policy.active_fps, 2);
            break;
        }
        case LayoutField::MidFps: {
            layout.capture_policy.mid_fps = static_cast<int>(std::lround(value));
            normalize_fps(layout.capture_policy.idle_fps, layout.capture_policy.mid_fps,
                          layout.capture_policy.active_fps, 1);
            break;
        }
        case LayoutField::IdleFps: {
            layout.capture_policy.idle_fps = static_cast<int>(std::lround(value));
            normalize_fps(layout.capture_policy.idle_fps, layout.capture_policy.mid_fps,
                          layout.capture_policy.active_fps, 0);
            break;
        }
        case LayoutField::EnterDeg:
            layout.capture_policy.enter_deg = value;
            if (layout.capture_policy.enter_deg <= layout.capture_policy.leave_deg) {
                layout.capture_policy.leave_deg = std::max(0.0f, layout.capture_policy.enter_deg - 1.0f);
            }
            break;
        case LayoutField::LeaveDeg:
            layout.capture_policy.leave_deg = value;
            if (layout.capture_policy.enter_deg <= layout.capture_policy.leave_deg) {
                layout.capture_policy.enter_deg = std::min(179.0f, layout.capture_policy.leave_deg + 1.0f);
            }
            break;
        case LayoutField::Count:
        default:
            error = "unknown layout field";
            return false;
    }
    return validate_layout(layout, error);
}

std::string format_layout_field_value(LayoutField field, float value) {
    char buffer[48];
    switch (field) {
        case LayoutField::Yaw:
        case LayoutField::Pitch:
        case LayoutField::Roll:
        case LayoutField::Fov:
        case LayoutField::EnterDeg:
        case LayoutField::LeaveDeg:
            std::snprintf(buffer, sizeof(buffer), "%.0f deg", static_cast<double>(value));
            break;
        case LayoutField::Distance:
        case LayoutField::Width:
        case LayoutField::Height:
            std::snprintf(buffer, sizeof(buffer), "%.2f m", static_cast<double>(value));
            break;
        case LayoutField::ActiveFps:
        case LayoutField::MidFps:
        case LayoutField::IdleFps:
            std::snprintf(buffer, sizeof(buffer), "%.0f fps", static_cast<double>(value));
            break;
        case LayoutField::Count:
        default:
            std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
            break;
    }
    return std::string(buffer);
}

std::string layout_summary(const Layout& layout) {
    if (layout.screens.empty()) {
        return "no screens";
    }
    float minimum_yaw = layout.screens.front().yaw_deg;
    float maximum_yaw = layout.screens.front().yaw_deg;
    for (const ScreenLayout& screen : layout.screens) {
        minimum_yaw = std::min(minimum_yaw, screen.yaw_deg);
        maximum_yaw = std::max(maximum_yaw, screen.yaw_deg);
    }
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%zu screens | arc %.0f..%.0f deg | %.0f deg fov | %d/%d/%d fps",
                  layout.screens.size(), static_cast<double>(minimum_yaw),
                  static_cast<double>(maximum_yaw), static_cast<double>(layout.fov_deg),
                  layout.capture_policy.active_fps, layout.capture_policy.mid_fps,
                  layout.capture_policy.idle_fps);
    return std::string(buffer);
}

namespace {

bool preset_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == ' ' || c == '-' || c == '_' || c == '.';
}

std::filesystem::path preset_file_path(const std::string& presets_dir, const std::string& name) {
    return path_from_utf8(presets_dir) / (name + ".json");
}

}  // namespace

bool layout_preset_name_valid(const std::string& name, std::string& error) {
    if (name.empty() || name.size() > 64) {
        error = "preset name must contain 1 to 64 characters";
        return false;
    }
    for (char c : name) {
        if (!preset_char_ok(c)) {
            error = "preset name may only contain letters, digits, space, '-', '_' and '.'";
            return false;
        }
    }
    if (name.front() == ' ' || name.front() == '.' || name.back() == ' ' || name.back() == '.') {
        error = "preset name must not start or end with a space or dot";
        return false;
    }
    if (name.find("..") != std::string::npos) {
        error = "preset name must not contain '..'";
        return false;
    }
    return true;
}

std::vector<std::string> list_layout_presets(const std::string& presets_dir) {
    std::vector<std::string> names;
    std::error_code error;
    std::filesystem::directory_iterator it(path_from_utf8(presets_dir), error);
    if (error) {
        return names;
    }
    const std::filesystem::directory_iterator end;
    for (; it != end; it.increment(error)) {
        if (error) {
            break;
        }
        if (!it->is_regular_file(error) || error) {
            continue;
        }
        if (it->path().extension() != ".json") {
            continue;
        }
        std::string stem_error;
        const std::string stem = utf8_from_path(it->path().stem());
        if (!layout_preset_name_valid(stem, stem_error)) {
            continue;
        }
        names.push_back(stem);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool save_layout_preset(const std::string& presets_dir, const std::string& name,
                        const Layout& layout, std::string& error) {
    if (!layout_preset_name_valid(name, error)) {
        return false;
    }
    std::error_code directory_error;
    std::filesystem::create_directories(path_from_utf8(presets_dir), directory_error);
    if (directory_error) {
        error = "could not create the presets directory";
        return false;
    }
    return save_layout(utf8_from_path(preset_file_path(presets_dir, name)), layout, error);
}

bool load_layout_preset(const std::string& presets_dir, const std::string& name, Layout& layout,
                        std::string& error) {
    if (!layout_preset_name_valid(name, error)) {
        return false;
    }
    Layout candidate;
    std::string load_error;
    if (!load_layout(utf8_from_path(preset_file_path(presets_dir, name)), candidate, load_error)) {
        error = "could not load preset '" + name + "': " + load_error;
        return false;
    }
    layout = std::move(candidate);
    return true;
}

bool delete_layout_preset(const std::string& presets_dir, const std::string& name,
                          std::string& error) {
    if (!layout_preset_name_valid(name, error)) {
        return false;
    }
    std::error_code remove_error;
    const bool removed = std::filesystem::remove(preset_file_path(presets_dir, name), remove_error);
    if (remove_error || !removed) {
        error = "could not delete preset '" + name + "'";
        return false;
    }
    return true;
}

}  // namespace gt
