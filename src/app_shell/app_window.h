#pragma once

// The controller window: sidebar pages, the setup guide, the arrangement
// editor, tray lifecycle and timers.
//
// The UI is event-driven. The only recurring timer is the health poll, which
// the configuration cannot set faster than twice per second; the animation
// timer exists solely while something is moving (hover fades, page
// transitions, a pulsing status, a toast) and is killed as soon as every
// animated value has settled, so an idle window burns no CPU.

#include "app_shell/app_state.h"
#include "app_shell/calibration_runner.h"
#include "app_shell/tray_icon.h"
#include "app_shell/ui_input.h"
#include "app_shell/ui_theme.h"
#include "app_shell/ui_widgets.h"

#include <windows.h>

#include <string>
#include <vector>

namespace gt::ui {

inline constexpr UINT_PTR kTimerHealth = 1;
inline constexpr UINT_PTR kTimerAnimation = 2;
// Status-only refresh at the 2 Hz cap while the engine reports the sensor
// warm-up or the hold-still measurement (the 1.3 s window is shorter than
// the default 1 s health poll can follow).
inline constexpr UINT_PTR kTimerStartup = 3;
inline constexpr UINT kStartupPollMs = 500;
inline constexpr UINT kAnimationIntervalMs = 15;
inline constexpr double kPageTransitionS = 0.22;

class AppWindow {
public:
    AppWindow() = default;
    ~AppWindow();
    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    // Bootstrap fills state() before create(): resolve_base_paths, config load,
    // layout load and the first health pass all happen there.
    AppState& state() { return state_; }

    bool create(HINSTANCE instance, std::string& error);
    void run();
    void shutdown();

    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT dispatch(UINT message, WPARAM wparam, LPARAM lparam);

    void on_create();
    void on_paint();
    void on_size();
    void on_dpi_changed(WPARAM wparam, LPARAM lparam);
    void on_display_change();
    void on_settings_change();
    void on_end_session(bool ending);
    void on_timer(UINT_PTR timer_id);
    void on_close();
    void on_destroy();
    void on_mouse_move(int x, int y);
    void on_mouse_leave();
    void on_lbutton_down(int x, int y);
    void on_lbutton_up(int x, int y);
    void on_capture_changed();
    void finish_drag(const PointerDragEnd& drag_end);
    void on_mouse_wheel(int delta);
    void on_key_down(WPARAM key, bool shift);
    void on_tray_message(LPARAM lparam);
    void on_set_cursor();
    void on_calibration_output();

    void execute(int id, int arg);
    void handle_tray_command(TrayCommand command);
    void activate_focused();
    void adjust_focused(WPARAM key);
    void cycle_focus(bool backwards);
    void set_focus(int id);
    void navigate(AppPage page);
    void apply_theme();
    void start_calibration(CalFlow flow);
    void cancel_calibration();
    void finish_onboarding(bool skipped);
    void refresh_health(bool expensive);
    void update_tooltip();
    void update_animation_timer();
    void update_startup_timer();
    void invalidate();
    void ensure_back_buffer(int width, int height);
    void release_back_buffer();
    void capture_fade_snapshot();
    void release_fade_buffer();
    void capture_placement();
    int hit_test(int x, int y) const;
    int panel_at(int x, int y) const;
    void set_panel_offset(int panel, int offset);
    void ensure_focus_visible();
    void scroll_focused_panel(WPARAM key);
    const Hotspot* hotspot(int id, int arg) const;
    const Hotspot* hotspot_at(int x, int y) const;
    void add_event(const std::wstring& text);
    bool prompt_preset_name(std::string& name);
    static INT_PTR CALLBACK preset_name_dlg_proc(HWND dlg, UINT message, WPARAM wparam,
                                                 LPARAM lparam);

    AppState state_;
    FontSet fonts_;
    TrayIcon tray_;
    CalibrationRunner calibration_;
    std::vector<Hotspot> hotspots_;
    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HDC back_dc_ = nullptr;
    HBITMAP back_bitmap_ = nullptr;
    HGDIOBJ back_previous_ = nullptr;
    int back_width_ = 0;
    int back_height_ = 0;
    // Snapshot of the last frame, faded out over the new page on navigation.
    HDC fade_dc_ = nullptr;
    HBITMAP fade_bitmap_ = nullptr;
    HGDIOBJ fade_previous_ = nullptr;
    int fade_left_ = 0;
    HICON icon_ = nullptr;
    HICON big_icon_ = nullptr;
    HICON small_icon_ = nullptr;
    UINT tray_message_ = 0;
    UINT calibration_message_ = 0;
    UINT taskbar_created_message_ = 0;
    UINT dpi_ = 96;
    int pending_click_id_ = 0;
    int pending_click_arg_ = 0;
    bool animation_timer_ = false;
    bool startup_timer_ = false;
    bool tracking_mouse_ = false;
    double last_animation_s_ = 0.0;
};

}  // namespace gt::ui
