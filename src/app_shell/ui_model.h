#pragma once

// Window-free model of the controller's navigation and appearance choices:
// the sidebar pages, the colour theme preference, the engine options the
// controller persists for the engine, and the magnetic heading-lock status
// vocabulary. Everything here is plain data so app_selftest can cover it.

#include <string>

namespace gt {

// Sidebar pages. The numeric values are only used as array indices and as
// the offset of the navigation hotspot ids; the persisted form is the key.
enum class AppPage : int {
    Home = 0,
    Layout,
    Comfort,
    Tracking,
    Diagnostics,
    Settings,
    Count,
};

inline constexpr int kAppPageCount = static_cast<int>(AppPage::Count);

const char* app_page_key(AppPage page);
bool parse_app_page(const std::string& text, AppPage& page);
// Short sidebar label, page header title and one-line page description.
const wchar_t* app_page_label(AppPage page);
const wchar_t* app_page_title(AppPage page);
const wchar_t* app_page_subtitle(AppPage page);

enum class ThemeMode : int {
    Dark = 0,
    Light,
    System,
    Count,
};

inline constexpr int kThemeModeCount = static_cast<int>(ThemeMode::Count);

const char* theme_mode_name(ThemeMode mode);
const wchar_t* theme_mode_label(ThemeMode mode);
bool parse_theme_mode(const std::string& text, ThemeMode& mode);
// True when the resolved theme is dark. System follows the Windows app
// colour setting the caller read (AppsUseLightTheme).
bool theme_is_dark(ThemeMode mode, bool system_uses_light_theme);

// Engine options the controller stores in app.json. They are carried to the
// engine at integration time (launch switch or live message); the UI only
// edits and persists them. Add new options here, then wire them in
// app_state.cpp (launch command) and the Tracking page.
// Turn prediction choices offered by the Tracking page (engine --predict-ms;
// 0 = off). 35 ms is the engine default measured in docs/overnight/track-a.md.
inline constexpr int kPredictMsDefault = 35;
inline constexpr int kPredictMsMax = 100;  // the engine clamps to 0..100
inline constexpr int kPredictMsChoiceCount = 4;
inline constexpr int kPredictMsChoices[kPredictMsChoiceCount] = {0, 20, 35, 50};
// Index of value in kPredictMsChoices, or -1 for a hand-edited other value.
int predict_ms_choice_index(int predict_ms);
// Screen scaling choices (engine --display-scale; 0 = Windows default, never
// touched). 175% is the most Windows allows on a 1920x1080 virtual display.
inline constexpr int kDisplayScaleChoiceCount = 5;
inline constexpr int kDisplayScaleChoices[kDisplayScaleChoiceCount] = {0, 100, 125, 150, 175};
// Index of value in kDisplayScaleChoices, or -1 when it is not offered.
int display_scale_choice_index(int display_scale_pct);

struct EngineOptions {
    // Motion-to-photon prediction horizon during fast head turns, passed as
    // --predict-ms. 0 = off. Inactive below 10 deg/s, so reading is unaffected.
    int predict_ms = kPredictMsDefault;
    // Experimental online hard-iron tracking for head-worn magnets such as
    // earbuds (--mag-track-hard-iron). Off by default: unverified on glasses.
    bool mag_track_hard_iron = false;
    // Windows display scale on the virtual screens while the workspace runs
    // (--display-scale, restored on exit). 0 = Windows default (untouched).
    int display_scale_pct = 0;

    bool operator==(const EngineOptions& other) const {
        return predict_ms == other.predict_ms && mag_track_hard_iron == other.mag_track_hard_iron &&
               display_scale_pct == other.display_scale_pct;
    }
    bool operator!=(const EngineOptions& other) const { return !(*this == other); }
};

// Magnetic heading lock as reported by the engine (status file key "mag").
// Unknown means "not running / this engine does not report it".
enum class HeadingLockState : int {
    Unknown = 0,
    Off,
    Locked,
    Disturbed,
    Relearning,
    Acquiring,
};

bool parse_heading_lock_state(const std::string& text, HeadingLockState& state);

// Engine startup phase as reported by the status file (key "phase", see
// app/engine_protocol.h). Unknown covers an older engine that does not
// report it and any value this controller does not know (a newer engine):
// both keep the generic Starting / Running view.
enum class EnginePhase : int {
    Unknown = 0,
    Preparing,
    Takeover,
    WaitingDisplay,
    Connecting,
    ImuWarmup,
    HoldStill,
    Tracking,
    Running,
    Restoring,
};

EnginePhase parse_engine_phase(const std::string& text);

// Plain-language startup steps for the Home hero. Visible only during the
// phases the wearer waits through; the active step's progress is -1 when
// it has none (indeterminate).
inline constexpr int kStartupStepCount = 4;
// Warm-up share after which the hero switches to "Get ready to hold still".
inline constexpr double kStartupGetReadyAt = 0.6;

struct StartupView {
    bool visible = false;
    int step_count = 0;    // kStartupStepCount with head tracking, else 0
    int active_step = -1;  // index into steps
    const wchar_t* steps[kStartupStepCount] = {L"Screens", L"Glasses sensor", L"Warm-up", L"Hold still"};
    const wchar_t* headline = L"";
    const wchar_t* detail = L"";
    double progress = -1.0;
    bool get_ready = false;   // the end of the warm-up: hold still is next
    bool hold_still = false;  // the gyro bias is being measured: keep still
};

StartupView startup_view(EnginePhase phase, double phase_progress, bool head_tracking);

// One line shown under the Start buttons.
inline constexpr const wchar_t* kStartStillHint =
    L"After you start, look straight ahead and keep your head still for a few seconds when asked.";
const wchar_t* heading_lock_label(HeadingLockState state);
const wchar_t* heading_lock_detail(HeadingLockState state);

}  // namespace gt
