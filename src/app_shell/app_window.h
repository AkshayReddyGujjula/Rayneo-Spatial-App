#pragma once

// The controller window: dashboard, layout editor, tray lifecycle and timers.
//
// The UI is event-driven. The only recurring timer is the health poll, which the
// configuration cannot set faster than twice per second; a second timer exists
// solely while a transition is on screen (engine starting, toast fading) and is
// killed as soon as that transition ends.

#include "app_shell/app_state.h"
#include "app_shell/tray_icon.h"
#include "app_shell/ui_theme.h"
#include "app_shell/ui_widgets.h"

#include <windows.h>

#include <string>
#include <vector>

namespace gt::ui {

inline constexpr UINT_PTR kTimerHealth = 1;
inline constexpr UINT_PTR kTimerAnimation = 2;
inline constexpr UINT kAnimationIntervalMs = 50;

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
    void on_end_session(bool ending);
    void on_timer(UINT_PTR timer_id);
    void on_close();
    void on_destroy();
    void on_mouse_move(int x, int y);
    void on_lbutton_down(int x, int y);
    void on_lbutton_up(int x, int y);
    void on_mouse_wheel(int delta);
    void on_key_down(WPARAM key, bool shift);
    void on_tray_message(LPARAM lparam);
    void on_set_cursor();

    void execute(int id, int arg);
    void handle_tray_command(TrayCommand command);
    void activate_focused();
    void adjust_focused(WPARAM key);
    void cycle_focus(bool backwards);
    void set_focus(int id);
    void refresh_health(bool expensive);
    void update_tooltip();
    void update_animation_timer();
    void invalidate();
    void ensure_back_buffer(int width, int height);
    void release_back_buffer();
    void capture_placement();
    int hit_test(int x, int y) const;
    int panel_at(int x, int y) const;
    void set_panel_offset(int panel, int offset);
    void ensure_focus_visible();
    void scroll_focused_panel(WPARAM key);
    const Hotspot* hotspot(int id, int arg) const;
    const Hotspot* hotspot_at(int x, int y) const;
    void add_event(const std::wstring& text);

    AppState state_;
    FontSet fonts_;
    TrayIcon tray_;
    std::vector<Hotspot> hotspots_;
    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HDC back_dc_ = nullptr;
    HBITMAP back_bitmap_ = nullptr;
    HGDIOBJ back_previous_ = nullptr;
    int back_width_ = 0;
    int back_height_ = 0;
    HICON icon_ = nullptr;
    HICON big_icon_ = nullptr;
    HICON small_icon_ = nullptr;
    UINT tray_message_ = 0;
    UINT taskbar_created_message_ = 0;
    UINT dpi_ = 96;
    int pending_click_id_ = 0;
    int pending_click_arg_ = 0;
    bool animation_timer_ = false;
};

}  // namespace gt::ui
