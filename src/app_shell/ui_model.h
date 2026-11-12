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
struct EngineOptions {
    // Short-horizon head-motion prediction in the renderer. Default off
    // until the engine side ships and is measured.
    bool motion_prediction = false;

    bool operator==(const EngineOptions& other) const {
        return motion_prediction == other.motion_prediction;
    }
    bool operator!=(const EngineOptions& other) const { return !(*this == other); }
};

// Magnetic heading lock as reported by the engine (status file or
// telemetry). Unknown means "this engine does not report it".
enum class HeadingLockState : int {
    Unknown = 0,
    Off,
    Locked,
    Disturbed,
    Relearning,
};

bool parse_heading_lock_state(const std::string& text, HeadingLockState& state);
const wchar_t* heading_lock_label(HeadingLockState state);
const wchar_t* heading_lock_detail(HeadingLockState state);

}  // namespace gt
