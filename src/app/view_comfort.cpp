#include "app/view_comfort.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace gt {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

bool parse_int(const std::string& text, int& value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed < -100000 || parsed > 100000) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool in_range(int value, int minimum, int maximum) {
    return value >= minimum && value <= maximum;
}

}  // namespace

bool operator==(const ViewComfort& a, const ViewComfort& b) {
    return a.stabilise_level == b.stabilise_level && a.dim_mode == b.dim_mode &&
           a.screen_brightness_pct == b.screen_brightness_pct &&
           a.focus_dim_pct == b.focus_dim_pct && a.night_tint == b.night_tint &&
           a.night_tint_pct == b.night_tint_pct;
}

const char* stabilise_level_name(int level) {
    static const char* const kNames[kComfortStabiliseLevels] = {"off", "low", "medium", "high",
                                                                "ultra"};
    return kNames[std::clamp(level, 0, kComfortStabiliseLevels - 1)];
}

const char* dim_mode_name(DimMode mode) {
    switch (mode) {
        case DimMode::Manual:
            return "manual";
        case DimMode::Focus:
            return "focus";
        case DimMode::Off:
        default:
            return "off";
    }
}

bool parse_dim_mode(const std::string& text, DimMode& mode) {
    for (int i = 0; i < kDimModeCount; ++i) {
        if (text == dim_mode_name(static_cast<DimMode>(i))) {
            mode = static_cast<DimMode>(i);
            return true;
        }
    }
    return false;
}

bool validate_view_comfort(const ViewComfort& comfort, std::string& error) {
    if (!in_range(comfort.stabilise_level, 0, kComfortStabiliseLevels - 1)) {
        error = "stabilise level must be 0..4 (off, low, medium, high, ultra)";
        return false;
    }
    const int mode = static_cast<int>(comfort.dim_mode);
    if (!in_range(mode, 0, kDimModeCount - 1)) {
        error = "dim mode must be off, manual or focus";
        return false;
    }
    for (int pct : comfort.screen_brightness_pct) {
        if (!in_range(pct, kComfortMinBrightnessPct, 100)) {
            error = "screen brightness must be 10..100 %";
            return false;
        }
    }
    if (!in_range(comfort.focus_dim_pct, kComfortMinBrightnessPct, 100)) {
        error = "focus dim brightness must be 10..100 %";
        return false;
    }
    if (!in_range(comfort.night_tint_pct, 0, 100)) {
        error = "night tint strength must be 0..100 %";
        return false;
    }
    return true;
}

Rgb night_tint_rgb(int pct) {
    const float s = static_cast<float>(std::clamp(pct, 0, 100)) / 100.0f;
    return Rgb{1.0f, 1.0f - 0.30f * s, 1.0f - 0.62f * s};
}

Rgb comfort_tint(const ViewComfort& comfort) {
    return comfort.night_tint ? night_tint_rgb(comfort.night_tint_pct) : Rgb{};
}

int comfort_focused_screen(const std::vector<std::pair<float, float>>& screen_yaw_pitch_deg,
                           float view_yaw_deg, float view_pitch_deg) {
    int best = -1;
    float best_cos = -2.0f;
    const float vy = view_yaw_deg * kDegToRad;
    const float vp = view_pitch_deg * kDegToRad;
    for (size_t i = 0; i < screen_yaw_pitch_deg.size(); ++i) {
        const float sy = screen_yaw_pitch_deg[i].first * kDegToRad;
        const float sp = screen_yaw_pitch_deg[i].second * kDegToRad;
        // Cosine of the angle between two directions given as yaw/pitch.
        const float c = std::sin(vp) * std::sin(sp) + std::cos(vp) * std::cos(sp) * std::cos(vy - sy);
        if (c > best_cos) {
            best_cos = c;
            best = static_cast<int>(i);
        }
    }
    return best;
}

float comfort_target_brightness(const ViewComfort& comfort, size_t screen_index,
                                int focused_index) {
    switch (comfort.dim_mode) {
        case DimMode::Manual:
            if (screen_index < comfort.screen_brightness_pct.size()) {
                return static_cast<float>(comfort.screen_brightness_pct[screen_index]) / 100.0f;
            }
            return 1.0f;
        case DimMode::Focus:
            if (focused_index < 0 || static_cast<size_t>(focused_index) == screen_index) {
                return 1.0f;
            }
            return static_cast<float>(comfort.focus_dim_pct) / 100.0f;
        case DimMode::Off:
        default:
            return 1.0f;
    }
}

float comfort_fade(float current, float target, float dt_s) {
    if (!(dt_s > 0.0f)) {
        return current;
    }
    const float k = 1.0f - std::exp(-dt_s / kComfortFocusFadeTauS);
    return current + (target - current) * k;
}

void append_view_comfort_args(const ViewComfort& comfort, std::vector<std::string>& arguments) {
    const ViewComfort defaults;
    if (comfort.stabilise_level != defaults.stabilise_level) {
        arguments.push_back("--stabilise");
        arguments.push_back(stabilise_level_name(comfort.stabilise_level));
    }
    if (comfort.dim_mode != DimMode::Off) {
        arguments.push_back("--dim-mode");
        arguments.push_back(dim_mode_name(comfort.dim_mode));
        if (comfort.dim_mode == DimMode::Manual) {
            std::string list;
            for (size_t i = 0; i < comfort.screen_brightness_pct.size(); ++i) {
                list += (i == 0 ? "" : ",") + std::to_string(comfort.screen_brightness_pct[i]);
            }
            arguments.push_back("--screen-brightness");
            arguments.push_back(list);
        } else {
            arguments.push_back("--focus-dim");
            arguments.push_back(std::to_string(comfort.focus_dim_pct));
        }
    }
    if (comfort.night_tint) {
        arguments.push_back("--night-tint");
        arguments.push_back(std::to_string(comfort.night_tint_pct));
    }
}

bool parse_view_comfort_arg(int argc, char** argv, int& index, ViewComfort& comfort,
                            bool& handled, std::string& error) {
    handled = false;
    const std::string name = argv[index];
    const bool known = name == "--stabilise" || name == "--dim-mode" ||
                       name == "--screen-brightness" || name == "--focus-dim" ||
                       name == "--night-tint";
    if (!known) {
        return true;
    }
    handled = true;
    if (index + 1 >= argc) {
        error = name + " needs a value";
        return false;
    }
    const std::string value = argv[++index];
    int number = 0;
    if (name == "--stabilise") {
        for (int level = 0; level < kComfortStabiliseLevels; ++level) {
            if (value == stabilise_level_name(level) ||
                (parse_int(value, number) && number == level)) {
                comfort.stabilise_level = level;
                return true;
            }
        }
        error = "--stabilise expects off, low, medium, high or ultra";
        return false;
    }
    if (name == "--dim-mode") {
        if (!parse_dim_mode(value, comfort.dim_mode)) {
            error = "--dim-mode expects off, manual or focus";
            return false;
        }
        return true;
    }
    if (name == "--screen-brightness") {
        std::array<int, kComfortMaxScreens> parsed = comfort.screen_brightness_pct;
        size_t count = 0;
        size_t start = 0;
        while (start <= value.size()) {
            const size_t comma = value.find(',', start);
            const std::string item =
                value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (count >= parsed.size() || !parse_int(item, number) ||
                !in_range(number, kComfortMinBrightnessPct, 100)) {
                error = "--screen-brightness expects up to 8 comma-separated values 10..100";
                return false;
            }
            parsed[count++] = number;
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        comfort.screen_brightness_pct = parsed;
        return true;
    }
    if (name == "--focus-dim") {
        if (!parse_int(value, number) || !in_range(number, kComfortMinBrightnessPct, 100)) {
            error = "--focus-dim expects 10..100";
            return false;
        }
        comfort.focus_dim_pct = number;
        return true;
    }
    // --night-tint: 0 disables, 1..100 enables at that strength.
    if (!parse_int(value, number) || !in_range(number, 0, 100)) {
        error = "--night-tint expects 0..100";
        return false;
    }
    comfort.night_tint = number > 0;
    if (number > 0) {
        comfort.night_tint_pct = number;
    }
    return true;
}

}  // namespace gt
