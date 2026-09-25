#pragma once

// View comfort settings shared by the controller (config/app.json, the
// dashboard) and the engine (command line + live window messages): reading
// stabilisation level, per-screen dimming and a warm night tint.
//
// Pure logic, no windows.h: the app-model selftest checks the ranges, the
// engine argument contract and the brightness/tint maths the renderer uses.

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace gt {

inline constexpr int kComfortMaxScreens = 8;  // validate_layout's screen limit
inline constexpr int kComfortStabiliseLevels = 5;  // off, low, medium, high, ultra
inline constexpr int kComfortStabiliseDefault = 2;  // medium
inline constexpr int kComfortMinBrightnessPct = 10;  // never fully black
inline constexpr float kComfortFocusFadeTauS = 0.25f;

enum class DimMode : int {
    Off = 0,     // every screen at full brightness
    Manual = 1,  // each screen at its own brightness slider
    Focus = 2,   // the screen you look at stays bright, the others dim
};
inline constexpr int kDimModeCount = 3;

struct ViewComfort {
    int stabilise_level = kComfortStabiliseDefault;
    DimMode dim_mode = DimMode::Off;
    std::array<int, kComfortMaxScreens> screen_brightness_pct{100, 100, 100, 100,
                                                              100, 100, 100, 100};
    int focus_dim_pct = 40;  // Focus mode: brightness of the screens you are not looking at
    bool night_tint = false;
    int night_tint_pct = 50;  // warmth strength, 0..100
};

bool operator==(const ViewComfort& a, const ViewComfort& b);

const char* stabilise_level_name(int level);  // "off" .. "ultra"
const char* dim_mode_name(DimMode mode);      // "off", "manual", "focus"
bool parse_dim_mode(const std::string& text, DimMode& mode);

bool validate_view_comfort(const ViewComfort& comfort, std::string& error);

struct Rgb {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

// Warm tint multiplier: 0 % is neutral; 100 % roughly a 2700 K warm white
// (blue cut hardest, green a little, red untouched).
Rgb night_tint_rgb(int pct);
// The tint the renderer applies when night tint is enabled, else neutral.
Rgb comfort_tint(const ViewComfort& comfort);

// Index of the screen whose centre is nearest the view direction, by angular
// distance on the sphere; -1 when there are no screens.
int comfort_focused_screen(const std::vector<std::pair<float, float>>& screen_yaw_pitch_deg,
                           float view_yaw_deg, float view_pitch_deg);

// Steady-state brightness (0..1) for a screen. Focus mode with no focused
// screen (focused_index < 0) keeps every screen bright.
float comfort_target_brightness(const ViewComfort& comfort, size_t screen_index,
                                int focused_index);

// Exponential approach used for the focus fade, frame-rate independent.
float comfort_fade(float current, float target, float dt_s);

// Engine switches. Only values that differ from the defaults are emitted, so
// a default configuration launches with exactly the historical command line.
void append_view_comfort_args(const ViewComfort& comfort, std::vector<std::string>& arguments);
// Parses one engine switch at argv[index] (advancing index past its value).
// Returns false with an error for a malformed value; `handled` reports
// whether argv[index] was a comfort switch at all.
bool parse_view_comfort_arg(int argc, char** argv, int& index, ViewComfort& comfort,
                            bool& handled, std::string& error);

}  // namespace gt
