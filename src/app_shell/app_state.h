#pragma once

// Everything the window needs in one place: resolved paths, preferences, the
// edited layout, the engine owner, the health report and the small amount of
// interaction state (focus, hover, drag). ui_paint.cpp reads it, app_window.cpp
// mutates it.

#include "app_shell/app_config.h"
#include "app_shell/app_layout.h"
#include "app_shell/diagnostics.h"
#include "app_shell/engine_client.h"
#include "app_shell/engine_commands.h"
#include "app_shell/paths.h"
#include "app_shell/telemetry.h"

#include <windows.h>

#include <string>
#include <vector>

namespace gt {

// Stable element ids. They are the keyboard focus identity, so they must not be
// renumbered once shipped.
enum UiId : int {
    kUiNone = 0,
    kUiStartWorkspace = 1,
    kUiStartPreview = 2,
    kUiStopEngine = 3,
    kUiRecenter = 4,
    kUiToggleYaw = 5,
    kUiTogglePitch = 6,
    kUiReloadLayout = 7,
    kUiSaveLayout = 8,
    kUiRevertLayout = 9,
    kUiAddScreen = 10,
    kUiRemoveScreen = 11,
    kUiRefresh = 12,
    kUiLaunchCalibration = 13,
    kUiOpenLogs = 14,
    kUiOpenEngineLog = 15,
    kUiCloseToTray = 16,
    kUiPreviewWithoutHeadTracking = 17,
    kUiQuit = 18,
    kUiRecoverDisplays = 19,
    kUiPresetSave = 20,
    kUiPresetDelete = 21,
    kUiPresetBase = 100,
    kUiFieldBase = 200,
    kUiScreenBase = 300,
    kUiEditorCanvas = 400,
    kUiScrollBase = 500,
    kUiUserPresetBase = 600,
    kUiHelpBase = 700,
};

inline int ui_user_preset_id(size_t index) {
    return kUiUserPresetBase + static_cast<int>(index);
}

inline int ui_help_id(int panel, int row) {
    return kUiHelpBase + panel * 64 + row;
}

inline int ui_preset_id(LayoutPreset preset) {
    return kUiPresetBase + static_cast<int>(preset);
}

inline int ui_field_id(LayoutField field) {
    return kUiFieldBase + static_cast<int>(field);
}

inline int ui_screen_id(size_t index) {
    return kUiScreenBase + static_cast<int>(index);
}

enum class Severity {
    Neutral = 0,
    Ok = 1,
    Warning = 2,
    Error = 3,
};

inline COLORREF severity_color(Severity severity) {
    switch (severity) {
        case Severity::Ok:
            return RGB(63, 178, 127);
        case Severity::Warning:
            return RGB(224, 169, 59);
        case Severity::Error:
            return RGB(224, 96, 94);
        case Severity::Neutral:
        default:
            return RGB(107, 118, 136);
    }
}

// Scrollable content panels. Each portal paints its content clipped to its
// view rect at (content_y - offset); the paint pass refreshes the geometry
// cache below every frame, and input (wheel, thumb drag, focus reveal) works
// off that cache.
enum ScrollPanel : int {
    kScrollStatus = 0,
    kScrollEngine = 1,
    kScrollDiagnostics = 2,
    kScrollLayout = 3,
    kScrollFields = 4,
    kScrollPanelCount = 5,
};

struct ScrollPortal {
    int offset = 0;
    RECT panel{};
    RECT view{};
    RECT scrollbar{};
    RECT thumb{};
    int content_h = 0;
    int max_offset = 0;
    int last_max = 0;
    bool visible = false;
};

inline const wchar_t* severity_text(Severity severity) {
    switch (severity) {
        case Severity::Ok:
            return L"OK";
        case Severity::Warning:
            return L"Warning";
        case Severity::Error:
            return L"Error";
        case Severity::Neutral:
        default:
            return L"Unknown";
    }
}

struct AppState {
    Paths paths;
    AppConfig config;
    // False when config/app.json failed to load: the running config is then
    // defaults, and automatic saves must not overwrite the user's file until
    // it reloads cleanly or the user changes a setting on purpose.
    bool config_valid = true;
    Layout layout;
    bool layout_dirty = false;
    size_t selected_screen = 0;
    std::vector<std::string> preset_names;
    std::string loaded_preset;

    EngineClient engine;
    EngineStatusFile engine_status;
    DiagnosticsReport diagnostics;

    double now_s = 0.0;
    double next_expensive_check_s = 0.0;
    bool starting = false;
    // Taskbar auto-hide lifecycle: captured at engine start, re-applied
    // after every display recovery and engine exit. The engine restores it
    // too, but a forced kill after the 45 s stop grace can still cut a slow
    // restore short, so the controller owns the last word. -1 = none.
    int taskbar_state_at_engine_start = -1;
    // Poll deadline for the sticky re-check: Explorer flips the tray
    // seconds after churn, so each poll re-asserts until this passes.
    double taskbar_restore_until_s = 0.0;
    // Deferred display recovery: set when the engine fails while still
    // alive (foreign timeout); the recovery runs once the process is gone.
    bool display_recovery_pending = false;

    std::wstring banner;
    Severity banner_severity = Severity::Neutral;
    std::wstring toast;
    double toast_expiry_s = 0.0;
    std::vector<std::wstring> events;

    int hover_id = 0;
    int focus_id = 0;
    int drag_field = -1;    // LayoutField while a slider is dragged
    int drag_screen = -1;   // screen index while the editor canvas is dragged
    POINT drag_origin{};
    float drag_start_value = 0.0f;
    float drag_start_pitch = 0.0f;
    bool drag_active = false;

    bool quit_requested = false;
    bool quitting = false;
    bool tray_notified = false;
    HWND hwnd = nullptr;

    ScrollPortal scroll[kScrollPanelCount];
    int drag_scroll_panel = -1;
    int drag_scroll_grab = 0;

    void add_event(const std::wstring& text);
    void set_banner(const std::wstring& text, Severity severity);
    void show_toast(const std::wstring& text);
    void clear_banner();
};

// Shared actions used by the window, the tray menu and the keyboard. They return
// false with a human-readable reason instead of failing silently.
bool start_engine(AppState& state, LaunchMode mode, std::wstring& error);
void stop_engine(AppState& state);
bool save_layout_to_disk(AppState& state, std::wstring& error);
bool revert_layout_from_disk(AppState& state, std::wstring& error);
std::string layout_presets_dir(const AppState& state);
void refresh_layout_presets(AppState& state);
bool save_config_to_disk(AppState& state, std::wstring& error);
bool reload_app_config(AppState& state, std::wstring& error);
void stop_engine_for_quit(AppState& state);
bool recover_display_topology(AppState& state);
// Re-applies the taskbar state captured at engine start after display
// churn (recovery, engine exit), and arms a sticky poll window that keeps
// re-asserting it while Explorer settles. No-op when nothing was captured.
void restore_taskbar_after_churn(AppState& state);
std::wstring start_block_reason(const AppState& state, LaunchMode mode);
void poll_engine(AppState& state);
void run_expensive_diagnostics(AppState& state);
void ensure_log_directory(const AppState& state);

}  // namespace gt
