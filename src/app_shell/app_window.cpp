#include "app_shell/app_window.h"

#include "app/engine_protocol.h"
#include "app_shell/clock.h"
#include "app_shell/ui_orbit.h"
#include "app_shell/ui_paint.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace gt::ui {
namespace {

using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

UINT window_dpi(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        auto fn = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
        if (fn != nullptr) {
            const UINT dpi = fn(hwnd);
            if (dpi >= 72 && dpi <= 480) {
                return dpi;
            }
        }
    }
    HDC dc = GetDC(hwnd);
    if (dc == nullptr) {
        return 96;
    }
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96;
}

void enable_dark_title_bar(HWND hwnd) {
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) {
        return;
    }
    auto fn = reinterpret_cast<DwmSetWindowAttributeFn>(
        GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (fn != nullptr) {
        const BOOL enabled = TRUE;
        // DWMWA_USE_IMMERSIVE_DARK_MODE (20 on current builds, 19 before 20H1).
        if (FAILED(fn(hwnd, 20, &enabled, sizeof(enabled)))) {
            fn(hwnd, 19, &enabled, sizeof(enabled));
        }
    }
    FreeLibrary(dwm);
}

bool open_path(const std::filesystem::path& path, std::wstring& error) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        error = L"'" + path.wstring() + L"' does not exist yet";
        return false;
    }
    const HINSTANCE launched =
        ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr,
                      path.parent_path().wstring().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launched) <= 32) {
        error = L"Windows could not open '" + path.wstring() + L"'";
        return false;
    }
    return true;
}

}  // namespace

AppWindow::~AppWindow() {
    release_back_buffer();
    fonts_.destroy();
    if (icon_ != nullptr) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
    if (big_icon_ != nullptr) {
        DestroyIcon(big_icon_);
        big_icon_ = nullptr;
    }
    if (small_icon_ != nullptr) {
        DestroyIcon(small_icon_);
        small_icon_ = nullptr;
    }
}

bool AppWindow::create(HINSTANCE instance, std::string& error) {
    instance_ = instance;
    tray_message_ = WM_APP + 0x200;
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = &AppWindow::window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kAppWindowClass;
    window_class.hIcon = create_app_icon(32);
    // No class small icon: the window small icon is set explicitly through
    // WM_SETICON (small_icon_), which owns its HICON lifetime.
    window_class.hIconSm = nullptr;
    if (RegisterClassExW(&window_class) == 0) {
        error = "RegisterClassExW failed";
        return false;
    }
    icon_ = window_class.hIcon;

    const WindowPlacement& placement = state_.config.window;
    int x = placement.x;
    int y = placement.y;
    int width = placement.width;
    int height = placement.height;
    RECT work_area{0, 0, 1280, 800};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    const int work_width = work_area.right - work_area.left;
    const int work_height = work_area.bottom - work_area.top;
    width = std::clamp(width, 900, std::max(900, work_width));
    height = std::clamp(height, 640, std::max(640, work_height));
    const int work_left = static_cast<int>(work_area.left);
    const int work_top = static_cast<int>(work_area.top);
    const int work_right = static_cast<int>(work_area.right);
    const int work_bottom = static_cast<int>(work_area.bottom);
    x = std::clamp(x, work_left - 40, std::max(work_left, work_right - 200));
    y = std::clamp(y, work_top, std::max(work_top, work_bottom - 120));

    hwnd_ = CreateWindowExW(0, kAppWindowClass, L"RayNeo Spatial", WS_OVERLAPPEDWINDOW, x, y, width,
                            height, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        error = "CreateWindowExW failed";
        return false;
    }
    big_icon_ = create_app_icon(32);
    small_icon_ = create_app_icon(16);
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon_));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon_));
    enable_dark_title_bar(hwnd_);
    if (placement.maximized) {
        ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
    } else {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
    }
    UpdateWindow(hwnd_);
    return true;
}

void AppWindow::run() {
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void AppWindow::shutdown() {
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK AppWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    AppWindow* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<AppWindow*>(create_struct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hwnd_ = hwnd;
        }
    }
    if (self == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return self->dispatch(message, wparam, lparam);
}

LRESULT AppWindow::dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    if (taskbar_created_message_ != 0 && message == taskbar_created_message_) {
        std::string tray_error;
        if (tray_.add(hwnd_, tray_message_, icon_, L"RayNeo Spatial", tray_error)) {
            add_event(L"notification-area icon re-added after an Explorer restart");
        }
        return 0;
    }
    switch (message) {
        case WM_CREATE:
            on_create();
            return 0;
        case WM_PAINT:
            on_paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            on_size();
            return 0;
        case WM_DPICHANGED:
            on_dpi_changed(wparam, lparam);
            return 0;
        case WM_TIMER:
            on_timer(wparam);
            return 0;
        case WM_CLOSE:
            on_close();
            return 0;
        case WM_DISPLAYCHANGE:
            on_display_change();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            on_end_session(wparam != 0);
            return 0;
        case WM_DESTROY:
            on_destroy();
            return 0;
        case WM_MOUSEMOVE:
            on_mouse_move(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_LBUTTONDOWN:
            on_lbutton_down(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_LBUTTONUP:
            on_lbutton_up(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            return 0;
        case WM_MOUSEWHEEL:
            on_mouse_wheel(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        case WM_KEYDOWN:
            on_key_down(wparam, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                on_set_cursor();
                return TRUE;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            RECT work_area{0, 0, 1280, 800};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
            const LONG min_width = static_cast<LONG>(scale_px(1000, dpi_));
            const LONG min_height = static_cast<LONG>(scale_px(680, dpi_));
            info->ptMinTrackSize.x = std::min(min_width, work_area.right - work_area.left);
            info->ptMinTrackSize.y = std::min(min_height, work_area.bottom - work_area.top);
            return 0;
        }
        default:
            break;
    }
    if (message == tray_message_) {
        on_tray_message(lparam);
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

void AppWindow::on_create() {
    dpi_ = window_dpi(hwnd_);
    fonts_.create(dpi_);
    state_.hwnd = hwnd_;

    std::string tray_error;
    if (!tray_.add(hwnd_, tray_message_, icon_, L"RayNeo Spatial", tray_error)) {
        state_.set_banner(L"the notification-area icon could not be created; the window will "
                          L"still work",
                          Severity::Warning);
    }
    SetTimer(hwnd_, kTimerHealth, static_cast<UINT>(state_.config.health_poll_ms), nullptr);
    refresh_health(true);
    update_tooltip();
    add_event(L"controller ready; paths root: " + state_.paths.root.wstring());
    if (!state_.config.last_engine_error.empty()) {
        state_.set_banner(L"the previous engine run failed: " +
                              wide_from_utf8(state_.config.last_engine_error) +
                              L"  (open the engine log for the full output)",
                          Severity::Warning);
    }
}

void AppWindow::on_size() {
    invalidate();
}

void AppWindow::on_dpi_changed(WPARAM wparam, LPARAM lparam) {
    dpi_ = LOWORD(wparam);
    fonts_.create(dpi_);
    const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
    if (suggested != nullptr) {
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    invalidate();
}

void AppWindow::on_display_change() {
    // A monitor may have vanished (unplugged glasses): re-clamp into the
    // current work area and re-run the expensive checks immediately instead
    // of waiting for the 2 s cadence.
    RECT work_area{0, 0, 1280, 800};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    RECT window_rect{};
    if (GetWindowRect(hwnd_, &window_rect)) {
        const int width = window_rect.right - window_rect.left;
        const int height = window_rect.bottom - window_rect.top;
        int x = window_rect.left;
        int y = window_rect.top;
        x = std::clamp(x, static_cast<int>(work_area.left) - 40,
                       std::max(static_cast<int>(work_area.left),
                                static_cast<int>(work_area.right) - 200));
        y = std::clamp(y, static_cast<int>(work_area.top),
                       std::max(static_cast<int>(work_area.top),
                                static_cast<int>(work_area.bottom) - 120));
        if (x != window_rect.left || y != window_rect.top) {
            SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    refresh_health(true);
    update_tooltip();
    invalidate();
}

void AppWindow::on_end_session(bool ending) {
    if (!ending) {
        return;
    }
    // System shutdown/logoff: ask the engine to quit. No bounded wait here;
    // blocking the session teardown would risk the watchdog. A still
    // starting (windowless) owned engine is terminated outright.
    if (state_.engine.busy()) {
        state_.engine.abort_for_session_end();
    }
}

void AppWindow::on_timer(UINT_PTR timer_id) {
    state_.now_s = steady_now_s();
    if (timer_id == kTimerHealth) {
        refresh_health(state_.now_s >= state_.next_expensive_check_s);
        update_tooltip();
        invalidate();
    } else if (timer_id == kTimerAnimation) {
        invalidate();
    }
    update_animation_timer();
}

void AppWindow::on_close() {
    if (state_.quitting) {
        return;
    }
    if (state_.config.close_to_tray && !state_.quit_requested && tray_.added()) {
        ShowWindow(hwnd_, SW_HIDE);
        if (!state_.tray_notified) {
            tray_.notify(L"RayNeo Spatial is still running",
                         L"Use the tray icon to show the window, start or stop the engine, "
                         L"recenter, or quit.");
            state_.tray_notified = true;
        }
        add_event(L"window hidden to the notification area");
        return;
    }
    state_.quit_requested = true;
    capture_placement();
    if (state_.config_valid) {
        std::wstring error;
        if (!save_config_to_disk(state_, error)) {
            // The window is going away: a banner would never be seen, so record
            // the failure in the event log instead of showing anything.
            add_event(L"preferences were not saved: " + error);
        }
    }
    stop_engine_for_quit(state_);
    DestroyWindow(hwnd_);
}

void AppWindow::on_destroy() {
    // Backstop for paths that destroy the window without going through
    // on_close: never orphan the engine. Best effort only; the bounded wait
    // lives in on_close.
    if (state_.engine.busy()) {
        state_.engine.request_stop();
    }
    KillTimer(hwnd_, kTimerHealth);
    KillTimer(hwnd_, kTimerAnimation);
    animation_timer_ = false;
    tray_.remove();
    release_back_buffer();
    state_.hwnd = nullptr;
    PostQuitMessage(0);
}

void AppWindow::capture_placement() {
    if (hwnd_ == nullptr) {
        return;
    }
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd_, &placement)) {
        const bool maximized = placement.showCmd == SW_SHOWMAXIMIZED;
        RECT rect = placement.rcNormalPosition;
        state_.config.window.maximized = maximized;
        state_.config.window.x = rect.left;
        state_.config.window.y = rect.top;
        state_.config.window.width = rect.right - rect.left;
        state_.config.window.height = rect.bottom - rect.top;
    }
}

void AppWindow::invalidate() {
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void AppWindow::ensure_back_buffer(int width, int height) {
    if (back_dc_ != nullptr && back_width_ == width && back_height_ == height) {
        return;
    }
    release_back_buffer();
    if (width <= 0 || height <= 0) {
        return;
    }
    HDC screen = GetDC(hwnd_);
    back_dc_ = CreateCompatibleDC(screen);
    back_bitmap_ = CreateCompatibleBitmap(screen, width, height);
    ReleaseDC(hwnd_, screen);
    if (back_dc_ == nullptr || back_bitmap_ == nullptr) {
        release_back_buffer();
        return;
    }
    back_previous_ = SelectObject(back_dc_, back_bitmap_);
    back_width_ = width;
    back_height_ = height;
}

void AppWindow::release_back_buffer() {
    if (back_dc_ != nullptr) {
        if (back_previous_ != nullptr) {
            SelectObject(back_dc_, back_previous_);
            back_previous_ = nullptr;
        }
        DeleteDC(back_dc_);
        back_dc_ = nullptr;
    }
    if (back_bitmap_ != nullptr) {
        DeleteObject(back_bitmap_);
        back_bitmap_ = nullptr;
    }
    back_width_ = 0;
    back_height_ = 0;
}

void AppWindow::on_paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    ensure_back_buffer(width, height);
    if (back_dc_ != nullptr) {
        PaintContext context;
        context.state = &state_;
        context.fonts = &fonts_;
        context.dpi = dpi_;
        context.client = client;
        Canvas canvas(back_dc_, client);
        paint_window(canvas, context, hotspots_);
        BitBlt(dc, 0, 0, width, height, back_dc_, 0, 0, SRCCOPY);
    }
    EndPaint(hwnd_, &paint);
}

int AppWindow::hit_test(int x, int y) const {
    for (const Hotspot& spot : hotspots_) {
        if (x >= spot.rect.left && x < spot.rect.right && y >= spot.rect.top &&
            y < spot.rect.bottom) {
            return spot.id;
        }
    }
    return kUiNone;
}

const Hotspot* AppWindow::hotspot_at(int x, int y) const {
    for (const Hotspot& spot : hotspots_) {
        if (x >= spot.rect.left && x < spot.rect.right && y >= spot.rect.top &&
            y < spot.rect.bottom) {
            return &spot;
        }
    }
    return nullptr;
}

const Hotspot* AppWindow::hotspot(int id, int arg) const {
    for (const Hotspot& spot : hotspots_) {
        if (spot.id == id && (arg < 0 || spot.arg == arg)) {
            return &spot;
        }
    }
    return nullptr;
}

int AppWindow::panel_at(int x, int y) const {
    for (int panel = 0; panel < kScrollPanelCount; ++panel) {
        const RECT& panel_rect = state_.scroll[panel].panel;
        if (panel_rect.right > panel_rect.left && panel_rect.bottom > panel_rect.top &&
            x >= panel_rect.left && x < panel_rect.right && y >= panel_rect.top &&
            y < panel_rect.bottom) {
            return panel;
        }
    }
    return -1;
}

void AppWindow::set_panel_offset(int panel, int offset) {
    if (panel < 0 || panel >= kScrollPanelCount) {
        return;
    }
    ScrollPortal& portal = state_.scroll[panel];
    const int clamped = std::clamp(offset, 0, portal.max_offset);
    if (clamped == portal.offset) {
        return;
    }
    portal.offset = clamped;
    state_.hover_id = kUiNone;
    pending_click_id_ = kUiNone;
    invalidate();
}

void AppWindow::ensure_focus_visible() {
    const Hotspot* spot = hotspot(state_.focus_id, -1);
    if (spot == nullptr || spot->panel < 0 || spot->panel >= kScrollPanelCount) {
        return;
    }
    const ScrollPortal& portal = state_.scroll[spot->panel];
    if (!portal.visible) {
        return;
    }
    if (spot->full.top < portal.view.top) {
        set_panel_offset(spot->panel, portal.offset - (portal.view.top - spot->full.top));
    } else if (spot->full.bottom > portal.view.bottom) {
        set_panel_offset(spot->panel, portal.offset + (spot->full.bottom - portal.view.bottom));
    }
}

void AppWindow::on_mouse_move(int x, int y) {
    state_.now_s = steady_now_s();
    if (state_.drag_active) {
        if (state_.drag_splitter >= 0 && state_.drag_splitter < kUiSplitCount) {
            SplitterTrack& track = state_.split_track[state_.drag_splitter];
            if (track.valid) {
                const int pos = track.column == 2 ? x : y;
                const int at = std::clamp(pos, track.min_px, track.max_px);
                if (track.column == 2) {
                    state_.config.splits.column =
                        static_cast<float>(at - track.span_start_px) / track.span_px;
                } else {
                    const int delta = at - track.origin_px;
                    const int h0 = track.first_px + delta;
                    const int h1 = track.second_px - delta;
                    const int h2 = track.span_px - track.first_px - track.second_px;
                    float* fracs = track.column == 0 ? state_.config.splits.left
                                                     : state_.config.splits.right;
                    fracs[track.index] = static_cast<float>(h0) / track.span_px;
                    fracs[track.index + 1] = static_cast<float>(h1) / track.span_px;
                    fracs[track.index == 0 ? 2 : 0] = static_cast<float>(h2) / track.span_px;
                }
                invalidate();
            }
            return;
        }
        if (state_.drag_scroll_panel >= 0 &&
            state_.drag_scroll_panel < kScrollPanelCount) {
            const ScrollPortal& portal = state_.scroll[state_.drag_scroll_panel];
            const int track_h = portal.scrollbar.bottom - portal.scrollbar.top;
            const int thumb_h = portal.thumb.bottom - portal.thumb.top;
            const int travel = std::max(1, track_h - thumb_h);
            const LONG raw_position =
                static_cast<LONG>(y - state_.drag_scroll_grab) - portal.scrollbar.top;
            const int position =
                static_cast<int>(std::clamp(raw_position, 0L, static_cast<LONG>(travel)));
            set_panel_offset(state_.drag_scroll_panel,
                             portal.max_offset > 0 ? position * portal.max_offset / travel : 0);
            return;
        }
        if (state_.drag_field >= 0) {
            const Hotspot* spot = hotspot(kUiFieldBase + state_.drag_field, -1);
            if (spot != nullptr && spot->full.right > spot->full.left) {
                const auto field = static_cast<LayoutField>(state_.drag_field);
                const float fraction = static_cast<float>(x - spot->full.left) /
                                       static_cast<float>(spot->full.right - spot->full.left);
                const float value = layout_slider_value(field, fraction);
                if (!state_.layout.screens.empty() &&
                    state_.selected_screen < state_.layout.screens.size()) {
                    std::string error;
                    set_layout_field(state_.layout, state_.layout.screens[state_.selected_screen],
                                     field, value, error);
                    state_.layout_dirty = true;
                }
            }
        } else if (state_.orbiting) {
            state_.orbit_yaw_deg += static_cast<float>(x - state_.orbit_origin.x) * 0.4f;
            state_.orbit_pitch_deg =
                std::clamp(state_.orbit_pitch_deg +
                                static_cast<float>(y - state_.orbit_origin.y) * 0.4f,
                            -80.0f, 80.0f);
            while (state_.orbit_yaw_deg > 180.0f) {
                state_.orbit_yaw_deg -= 360.0f;
            }
            while (state_.orbit_yaw_deg < -180.0f) {
                state_.orbit_yaw_deg += 360.0f;
            }
            state_.orbit_origin = POINT{x, y};
            invalidate();
        } else if (state_.drag_screen >= 0) {
            const Hotspot* editor_spot = hotspot(kUiEditorCanvas, -1);
            if (editor_spot == nullptr) {
                return;
            }
            if (static_cast<size_t>(state_.drag_screen) < state_.layout.screens.size()) {
                ScreenLayout& screen = state_.layout.screens[static_cast<size_t>(state_.drag_screen)];
                std::string error;
                float yaw = screen.yaw_deg;
                float pitch = screen.pitch_deg;
                if (state_.editor_3d) {
                    ScreenLayout start_screen = screen;
                    start_screen.yaw_deg = state_.drag_start_yaw;
                    start_screen.pitch_deg = state_.drag_start_pitch;
                    OrbitView orbit;
                    orbit.yaw_deg = state_.orbit_yaw_deg;
                    orbit.pitch_deg = state_.orbit_pitch_deg;
                    orbit.distance_m = state_.orbit_distance_m;
                    const OrbitDragAngles angles = orbit_drag_angles(
                        start_screen, orbit, orbit_camera(orbit), editor_spot->rect,
                        x - state_.drag_origin.x, y - state_.drag_origin.y);
                    yaw = angles.yaw_deg;
                    pitch = angles.pitch_deg;
                } else {
                    const EditorGeometry geometry = editor_geometry(state_.layout, editor_spot->rect, dpi_);
                    yaw = editor_yaw_from_point(geometry, POINT{x, y});
                    pitch = state_.drag_start_pitch +
                            static_cast<float>(state_.drag_origin.y - y) * 0.25f;
                }
                set_layout_field(state_.layout, screen, LayoutField::Yaw, yaw, error);
                set_layout_field(state_.layout, screen, LayoutField::Pitch, pitch, error);
                state_.layout_dirty = true;
            }
        }
        invalidate();
        return;
    }
    const int hover = hit_test(x, y);
    if (hover != state_.hover_id) {
        state_.hover_id = hover;
        invalidate();
    }
    on_set_cursor();
}

void AppWindow::on_lbutton_down(int x, int y) {
    SetFocus(hwnd_);
    state_.now_s = steady_now_s();
    const Hotspot* spot = hotspot_at(x, y);
    if (spot == nullptr || !spot->enabled) {
        pending_click_id_ = kUiNone;
        return;
    }
    if (spot->id >= static_cast<int>(kUiScrollBase) &&
        spot->id < static_cast<int>(kUiScrollBase) + static_cast<int>(kScrollPanelCount)) {
        pending_click_id_ = kUiNone;
        const int panel = spot->arg;
        if (panel >= 0 && panel < kScrollPanelCount) {
            const ScrollPortal& portal = state_.scroll[panel];
            if (y >= portal.thumb.top && y < portal.thumb.bottom) {
                state_.drag_scroll_panel = panel;
                state_.drag_scroll_grab = y - portal.thumb.top;
                state_.drag_active = true;
                SetCapture(hwnd_);
            } else {
                const int view_h = portal.view.bottom - portal.view.top;
                const int direction = y < portal.thumb.top ? -1 : 1;
                set_panel_offset(panel, portal.offset + direction * view_h * 9 / 10);
            }
        }
        invalidate();
        return;
    }
    if (spot->id >= kUiSplitBase && spot->id < kUiSplitBase + kUiSplitCount) {
        const int index = spot->id - kUiSplitBase;
        if (state_.split_track[index].valid) {
            state_.drag_splitter = index;
            state_.drag_active = true;
            pending_click_id_ = kUiNone;
            SetCapture(hwnd_);
            on_mouse_move(x, y);
        }
        invalidate();
        return;
    }
    pending_click_id_ = spot->id;
    pending_click_arg_ = spot->arg;
    set_focus(spot->id);
    if (spot->id == kUiEditorCanvas) {
        int screen = -1;
        if (state_.editor_3d) {
            OrbitView orbit;
            orbit.yaw_deg = state_.orbit_yaw_deg;
            orbit.pitch_deg = state_.orbit_pitch_deg;
            orbit.distance_m = state_.orbit_distance_m;
            screen = orbit_hit_test(state_.layout, orbit, orbit_camera(orbit), spot->rect, x, y,
                                    scale_px(14, dpi_));
        } else {
            const EditorGeometry geometry = editor_geometry(state_.layout, spot->rect, dpi_);
            screen = editor_screen_hit_test(state_.layout, geometry, POINT{x, y},
                                            scale_px(14, dpi_));
        }
        if (screen >= 0) {
            state_.selected_screen = static_cast<size_t>(screen);
            state_.drag_screen = screen;
            state_.drag_origin = POINT{x, y};
            state_.drag_start_pitch = state_.layout.screens[static_cast<size_t>(screen)].pitch_deg;
            state_.drag_start_yaw = state_.layout.screens[static_cast<size_t>(screen)].yaw_deg;
            state_.drag_active = true;
            SetCapture(hwnd_);
        } else if (state_.editor_3d) {
            state_.orbiting = true;
            state_.orbit_origin = POINT{x, y};
            state_.drag_active = true;
            SetCapture(hwnd_);
        }
        invalidate();
        return;
    }
    if (spot->id >= kUiFieldBase && spot->id < kUiFieldBase + static_cast<int>(LayoutField::Count)) {
        state_.drag_field = spot->arg;
        state_.drag_active = true;
        SetCapture(hwnd_);
        on_mouse_move(x, y);
        return;
    }
    invalidate();
}

void AppWindow::on_lbutton_up(int x, int y) {
    if (state_.drag_active) {
        const bool moved_splitter = state_.drag_splitter >= 0;
        state_.drag_active = false;
        state_.drag_field = -1;
        state_.drag_screen = -1;
        state_.orbiting = false;
        state_.drag_scroll_panel = -1;
        state_.drag_splitter = -1;
        pending_click_id_ = kUiNone;
        ReleaseCapture();
        if (moved_splitter && state_.config_valid) {
            std::wstring ignored;
            save_config_to_disk(state_, ignored);
        }
        invalidate();
        return;
    }
    if (pending_click_id_ != kUiNone) {
        const Hotspot* spot = hotspot_at(x, y);
        const int id = pending_click_id_;
        const int arg = pending_click_arg_;
        pending_click_id_ = kUiNone;
        if (spot != nullptr && spot->id == id && spot->enabled) {
            execute(id, arg);
        }
    }
}

void AppWindow::on_mouse_wheel(int delta) {
    state_.now_s = steady_now_s();
    if (state_.drag_active) {
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(hwnd_, &cursor);
    const Hotspot* canvas_spot = hotspot_at(cursor.x, cursor.y);
    if (canvas_spot != nullptr && canvas_spot->id == kUiEditorCanvas && state_.editor_3d) {
        const float factor = delta > 0 ? 0.9f : 1.1f;
        state_.orbit_distance_m = std::clamp(state_.orbit_distance_m * factor, 2.5f, 20.0f);
        invalidate();
        return;
    }
    const int panel = panel_at(cursor.x, cursor.y);
    if (panel >= 0 && state_.scroll[panel].visible) {
        const ScrollPortal& portal = state_.scroll[panel];
        const int view_h =
            static_cast<int>(std::max(1L, portal.view.bottom - portal.view.top));
        const int step = std::max(30, view_h / 6);
        const int notches = delta / WHEEL_DELTA;
        const int amount = notches != 0 ? -notches * step : (delta > 0 ? -step : step);
        set_panel_offset(panel, portal.offset + amount);
        return;
    }
    const Hotspot* spot = hotspot_at(cursor.x, cursor.y);
    if (spot == nullptr || spot->id < kUiFieldBase ||
        spot->id >= kUiFieldBase + static_cast<int>(LayoutField::Count)) {
        return;
    }
    const auto field = static_cast<LayoutField>(spot->arg);
    float minimum = 0.0f;
    float maximum = 0.0f;
    float step = 0.0f;
    layout_field_range(field, minimum, maximum, step);
    if (state_.layout.screens.empty() || state_.selected_screen >= state_.layout.screens.size()) {
        return;
    }
    const float current = layout_field_value(state_.layout, state_.layout.screens[state_.selected_screen],
                                            field);
    const float direction = delta > 0 ? 1.0f : -1.0f;
    std::string error;
    if (set_layout_field(state_.layout, state_.layout.screens[state_.selected_screen], field,
                         current + direction * step, error)) {
        state_.layout_dirty = true;
    }
    invalidate();
}

void AppWindow::on_key_down(WPARAM key, bool shift) {
    state_.now_s = steady_now_s();
    switch (key) {
        case VK_TAB:
            cycle_focus(shift);
            return;
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_HOME:
        case VK_END:
            adjust_focused(key);
            return;
        case VK_SPACE:
        case VK_RETURN:
            activate_focused();
            return;
        case VK_ESCAPE:
            if (state_.editor_fullscreen) {
                state_.editor_fullscreen = false;
                invalidate();
                return;
            }
            on_close();
            return;
        case 'S':
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                execute(kUiSaveLayout, 0);
            }
            return;
        default:
            break;
    }
}

void AppWindow::on_set_cursor() {
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(hwnd_, &cursor);
    const Hotspot* spot = hotspot_at(cursor.x, cursor.y);
    if (spot == nullptr) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return;
    }
    if (spot->id >= kUiFieldBase && spot->id < kUiFieldBase + static_cast<int>(LayoutField::Count)) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return;
    }
    if (spot->id == kUiEditorCanvas) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return;
    }
    if (spot->id >= kUiSplitBase && spot->id < kUiSplitBase + kUiSplitCount) {
        SetCursor(LoadCursorW(nullptr, spot->id == kUiSplitBase + 4 ? IDC_SIZEWE : IDC_SIZENS));
        return;
    }
    if (spot->id >= static_cast<int>(kUiScrollBase) &&
        spot->id < static_cast<int>(kUiScrollBase) + static_cast<int>(kScrollPanelCount)) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return;
    }
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
}

void AppWindow::set_focus(int id) {
    if (state_.focus_id != id) {
        state_.focus_id = id;
        invalidate();
    }
}

void AppWindow::cycle_focus(bool backwards) {
    std::vector<int> ids;
    for (const Hotspot& spot : hotspots_) {
        const bool scrollbar = spot.id >= static_cast<int>(kUiScrollBase) &&
                                 spot.id < static_cast<int>(kUiScrollBase) +
                                             static_cast<int>(kScrollPanelCount);
        if (spot.enabled && !scrollbar) {
            ids.push_back(spot.id);
        }
    }
    if (ids.empty()) {
        return;
    }
    auto current = std::find(ids.begin(), ids.end(), state_.focus_id);
    size_t index = 0;
    if (current != ids.end()) {
        const size_t position = static_cast<size_t>(std::distance(ids.begin(), current));
        index = backwards ? (position + ids.size() - 1) % ids.size() : (position + 1) % ids.size();
    }
    state_.focus_id = ids[index];
    ensure_focus_visible();
    invalidate();
}

void AppWindow::adjust_focused(WPARAM key) {
    const Hotspot* live = hotspot(state_.focus_id, -1);
    if (live == nullptr || !live->enabled) {
        return;
    }
    if (state_.focus_id >= kUiFieldBase &&
        state_.focus_id < kUiFieldBase + static_cast<int>(LayoutField::Count)) {
        const auto field = static_cast<LayoutField>(state_.focus_id - kUiFieldBase);
        float minimum = 0.0f;
        float maximum = 0.0f;
        float step = 0.0f;
        layout_field_range(field, minimum, maximum, step);
        if (state_.layout.screens.empty() ||
            state_.selected_screen >= state_.layout.screens.size()) {
            return;
        }
        ScreenLayout& screen = state_.layout.screens[state_.selected_screen];
        const float current = layout_field_value(state_.layout, screen, field);
        float delta = step;
        if (key == VK_UP) {
            delta = step * 5.0f;
        } else if (key == VK_PRIOR) {
            delta = step * 10.0f;
        } else if (key == VK_NEXT) {
            delta = -step * 10.0f;
        } else if (key == VK_DOWN) {
            delta = -step * 5.0f;
        } else if (key == VK_LEFT) {
            delta = -step;
        } else if (key == VK_RIGHT) {
            delta = step;
        }
        float value = current + delta;
        if (key == VK_HOME) {
            value = minimum;
        } else if (key == VK_END) {
            value = maximum;
        }
        std::string error;
        if (set_layout_field(state_.layout, screen, field, value, error)) {
            state_.layout_dirty = true;
            invalidate();
        }
        return;
    }
    if (state_.focus_id == kUiEditorCanvas && !state_.layout.screens.empty() &&
        state_.selected_screen < state_.layout.screens.size()) {
        ScreenLayout& screen = state_.layout.screens[state_.selected_screen];
        const bool horizontal = key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END;
        const LayoutField field = horizontal ? LayoutField::Yaw : LayoutField::Pitch;
        float minimum = 0.0f;
        float maximum = 0.0f;
        float step = 0.0f;
        layout_field_range(field, minimum, maximum, step);
        const float current = layout_field_value(state_.layout, screen, field);
        float value = current;
        if (key == VK_LEFT) {
            value = current - step;
        } else if (key == VK_RIGHT) {
            value = current + step;
        } else if (key == VK_UP) {
            value = current + step;
        } else if (key == VK_DOWN) {
            value = current - step;
        } else if (key == VK_HOME) {
            value = minimum;
        } else if (key == VK_END) {
            value = maximum;
        }
        std::string error;
        if (set_layout_field(state_.layout, screen, field, value, error)) {
            state_.layout_dirty = true;
            invalidate();
        }
    }
    if (state_.focus_id == kUiEditorCanvas ||
        (state_.focus_id >= kUiFieldBase &&
         state_.focus_id < kUiFieldBase + static_cast<int>(LayoutField::Count))) {
        return;
    }
    scroll_focused_panel(key);
}

void AppWindow::scroll_focused_panel(WPARAM key) {
    const Hotspot* spot = hotspot(state_.focus_id, -1);
    if (spot == nullptr || spot->panel < 0 || spot->panel >= kScrollPanelCount) {
        return;
    }
    const ScrollPortal& portal = state_.scroll[spot->panel];
    if (!portal.visible) {
        return;
    }
    const int view_h =
        static_cast<int>(std::max(1L, portal.view.bottom - portal.view.top));
    if (key == VK_PRIOR) {
        set_panel_offset(spot->panel, portal.offset - view_h * 9 / 10);
    } else if (key == VK_NEXT) {
        set_panel_offset(spot->panel, portal.offset + view_h * 9 / 10);
    } else if (key == VK_HOME) {
        set_panel_offset(spot->panel, 0);
    } else if (key == VK_END) {
        set_panel_offset(spot->panel, portal.max_offset);
    } else if (key == VK_UP) {
        set_panel_offset(spot->panel, portal.offset - 30);
    } else if (key == VK_DOWN) {
        set_panel_offset(spot->panel, portal.offset + 30);
    }
}

void AppWindow::activate_focused() {
    if (state_.focus_id == kUiNone) {
        cycle_focus(false);
        return;
    }
    const Hotspot* spot = hotspot(state_.focus_id, -1);
    if (spot == nullptr || !spot->enabled) {
        cycle_focus(false);
        return;
    }
    if (state_.focus_id >= kUiFieldBase) {
        return;
    }
    execute(state_.focus_id, 0);
}

void AppWindow::execute(int id, int arg) {
    (void)arg;
    if (state_.quitting) {
        return;
    }
    state_.now_s = steady_now_s();
    std::wstring error;
    std::string engine_error;
    std::string layout_error;
    std::string tool_error;
    switch (id) {
        case kUiStartWorkspace:
        case kUiStartPreview: {
            const LaunchMode mode = id == kUiStartWorkspace ? LaunchMode::Workspace
                                                            : LaunchMode::Preview;
            if (!start_engine(state_, mode, error)) {
                state_.set_banner(error, Severity::Error);
                add_event(L"start refused: " + error);
            } else {
                state_.show_toast(mode == LaunchMode::Workspace ? L"Starting full workspace..."
                                                                : L"Starting preview...");
            }
            break;
        }
        case kUiStopEngine:
            stop_engine(state_);
            state_.show_toast(L"Stop requested; the engine will quit gracefully");
            break;
        case kUiRecenter:
            if (state_.engine.send_command(kEngineMessageRecenter, engine_error)) {
                state_.show_toast(L"Recenter sent to the engine");
            } else {
                state_.set_banner(L"Recenter failed: " + wide_from_utf8(engine_error), Severity::Warning);
            }
            break;
        case kUiToggleYaw:
            if (!state_.engine.send_command(kEngineMessageToggleYaw, engine_error)) {
                state_.set_banner(L"Yaw toggle failed: " + wide_from_utf8(engine_error), Severity::Warning);
            } else {
                state_.show_toast(L"Yaw tracking toggled (Ctrl+Alt+Y also works)");
            }
            break;
        case kUiTogglePitch:
            if (!state_.engine.send_command(kEngineMessageTogglePitch, engine_error)) {
                state_.set_banner(L"Pitch toggle failed: " + wide_from_utf8(engine_error),
                                  Severity::Warning);
            } else {
                state_.show_toast(L"Pitch tracking toggled (Ctrl+Alt+P also works)");
            }
            break;
        case kUiReloadLayout:
            if (!state_.engine.send_command(kEngineMessageReloadLayout, engine_error)) {
                state_.set_banner(L"Layout reload failed: " + wide_from_utf8(engine_error),
                                  Severity::Warning);
            } else if (state_.layout_dirty) {
                state_.set_banner(L"Reload requested, but the editor has unsaved changes: press "
                                  L"Save to apply them or Revert to discard them",
                                  Severity::Warning);
            } else {
                state_.show_toast(L"Layout reload requested; the engine confirms by screen count");
            }
            break;
        case kUiSaveLayout:
            if (!save_layout_to_disk(state_, error)) {
                state_.set_banner(error, Severity::Error);
            } else {
                state_.show_toast(L"Layout saved; reload requested (watch the screen count)");
                refresh_health(true);
            }
            break;
        case kUiRevertLayout:
            if (!revert_layout_from_disk(state_, error)) {
                state_.set_banner(error, Severity::Error);
            } else {
                state_.show_toast(L"Layout reloaded from disk");
            }
            break;
        case kUiAddScreen:
            if (!add_screen(state_.layout, layout_error)) {
                state_.set_banner(L"Could not add a screen: " + wide_from_utf8(layout_error),
                                  Severity::Error);
            } else {
                state_.selected_screen = state_.layout.screens.size() - 1;
                state_.layout_dirty = true;
                state_.show_toast(L"Screen added (" +
                                  std::to_wstring(state_.layout.screens.size()) +
                                  L" of 8); Save and reload to apply it");
            }
            break;
        case kUiRemoveScreen:
            if (!remove_screen(state_.layout, state_.selected_screen, layout_error)) {
                state_.set_banner(L"Could not remove the screen: " + wide_from_utf8(layout_error),
                                  Severity::Error);
            } else {
                if (state_.selected_screen >= state_.layout.screens.size()) {
                    state_.selected_screen = state_.layout.screens.size() - 1;
                }
                state_.layout_dirty = true;
                state_.show_toast(L"Screen removed; Save and reload to apply it");
            }
            break;
        case kUiRefresh:
            if (!reload_app_config(state_, error)) {
                state_.set_banner(error, Severity::Warning);
            } else {
                // The poll cadence is a live setting: re-arm the timer so an
                // edited health_poll_ms takes effect immediately.
                KillTimer(hwnd_, kTimerHealth);
                SetTimer(hwnd_, kTimerHealth, static_cast<UINT>(state_.config.health_poll_ms),
                         nullptr);
                state_.show_toast(L"Diagnostics and preferences refreshed");
            }
            refresh_health(true);
            break;
        case kUiLaunchCalibration:
            if (!state_.paths.calibration_tool_found) {
                state_.set_banner(L"orientation_calibrate.exe was not found next to this app",
                                  Severity::Error);
            } else if (state_.engine.launch_tool(
                           state_.paths.calibration_executable,
                           {"--output", narrow_utf8(state_.paths.calibration_path)}, tool_error)) {
                state_.show_toast(L"Calibration started in its own console window");
                add_event(L"launched orientation_calibrate.exe; it overwrites the calibration "
                          L"file when it succeeds");
            } else {
                state_.set_banner(L"Could not start the calibration tool: " +
                                      wide_from_utf8(tool_error),
                                  Severity::Error);
            }
            break;
        case kUiOpenLogs: {
            ensure_log_directory(state_);
            std::wstring open_error;
            if (!open_path(state_.paths.log_dir, open_error)) {
                state_.set_banner(L"Could not open the logs folder: " + open_error,
                                  Severity::Warning);
            }
            break;
        }
        case kUiOpenEngineLog: {
            std::wstring open_error;
            if (!open_path(state_.paths.engine_log, open_error)) {
                state_.set_banner(L"Could not open the engine log: " + open_error,
                                  Severity::Warning);
            }
            break;
        }
        case kUiEditorFullscreen:
            state_.editor_fullscreen = !state_.editor_fullscreen;
            break;
        case kUiEditorDimToggle:
            state_.editor_3d = !state_.editor_3d;
            break;
        case kUiPresetSave: {
            std::string preset_name;
            if (!prompt_preset_name(preset_name)) {
                break;
            }
            std::string preset_error;
            if (!save_layout_preset(layout_presets_dir(state_), preset_name, state_.layout,
                                    preset_error)) {
                state_.set_banner(wide_from_utf8(preset_error), Severity::Error);
                break;
            }
            refresh_layout_presets(state_);
            state_.loaded_preset = preset_name;
            state_.config.active_preset = preset_name;
            if (state_.config_valid && !save_config_to_disk(state_, error)) {
                state_.set_banner(error, Severity::Warning);
                break;
            }
            state_.show_toast(L"Preset '" + wide_from_utf8(preset_name) + L"' saved");
            break;
        }
        case kUiPresetDelete: {
            if (state_.loaded_preset.empty()) {
                break;
            }
            const std::string doomed = state_.loaded_preset;
            std::string preset_error;
            if (!delete_layout_preset(layout_presets_dir(state_), doomed, preset_error)) {
                state_.set_banner(wide_from_utf8(preset_error), Severity::Error);
                break;
            }
            refresh_layout_presets(state_);
            state_.show_toast(L"Preset '" + wide_from_utf8(doomed) + L"' deleted");
            break;
        }
        case kUiCloseToTray:
            state_.config.close_to_tray = !state_.config.close_to_tray;
            if (!save_config_to_disk(state_, error)) {
                state_.set_banner(error, Severity::Warning);
            }
            break;
        case kUiPreviewWithoutHeadTracking:
            state_.config.preview_without_head_tracking =
                !state_.config.preview_without_head_tracking;
            if (!save_config_to_disk(state_, error)) {
                state_.set_banner(error, Severity::Warning);
            }
            break;
        case kUiRecoverDisplays:
            if (recover_display_topology(state_)) {
                state_.show_toast(
                    L"Display recovery applied; use Win+P if the laptop display stays off");
            } else {
                state_.show_toast(L"Display recovery failed; use Win+P to restore the display");
            }
            refresh_health(true);
            break;
        case kUiQuit:
            state_.quit_requested = true;
            on_close();
            break;
        default:
            if (id >= kUiPresetBase && id < kUiPresetBase + 32) {
                const auto preset = static_cast<LayoutPreset>(id - kUiPresetBase);
                state_.layout = preset_layout(preset);
                state_.selected_screen = 0;
                state_.layout_dirty = true;
                state_.show_toast(L"Preset '" + wide_from_utf8(layout_preset_name(preset)) +
                                  L"' loaded; Save and reload to write it");
            } else if (id >= kUiScreenBase && id < kUiScreenBase + 8) {
                state_.selected_screen = static_cast<size_t>(id - kUiScreenBase);
                state_.focus_id = id;
            } else if (id >= kUiUserPresetBase && id < kUiUserPresetBase + 32) {
                const size_t index = static_cast<size_t>(id - kUiUserPresetBase);
                if (index < state_.preset_names.size()) {
                    const std::string& name = state_.preset_names[index];
                    Layout preset;
                    std::string preset_error;
                    if (!load_layout_preset(layout_presets_dir(state_), name, preset,
                                            preset_error)) {
                        state_.set_banner(wide_from_utf8(preset_error), Severity::Error);
                    } else {
                        state_.layout = std::move(preset);
                        state_.selected_screen = 0;
                        state_.layout_dirty = true;
                        state_.loaded_preset = name;
                        state_.config.active_preset = name;
                        if (state_.config_valid) {
                            std::wstring config_error;
                            if (!save_config_to_disk(state_, config_error)) {
                                state_.set_banner(config_error, Severity::Warning);
                            }
                        }
                        state_.show_toast(L"Preset '" + wide_from_utf8(name) +
                                          L"' loaded; Save and reload to write it");
                    }
                }
            } else if (id == kUiEditorCanvas) {
                state_.focus_id = id;
            }
            break;
    }
    invalidate();
}

void AppWindow::handle_tray_command(TrayCommand command) {
    switch (command) {
        case TrayCommand::Show:
            ShowWindow(hwnd_, SW_SHOW);
            ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
            break;
        case TrayCommand::StartWorkspace:
            execute(kUiStartWorkspace, 0);
            break;
        case TrayCommand::StartPreview:
            execute(kUiStartPreview, 0);
            break;
        case TrayCommand::Stop:
            execute(kUiStopEngine, 0);
            break;
        case TrayCommand::Recenter:
            execute(kUiRecenter, 0);
            break;
        case TrayCommand::ReloadLayout:
            execute(kUiReloadLayout, 0);
            break;
        case TrayCommand::Quit:
            state_.quit_requested = true;
            on_close();
            break;
        case TrayCommand::None:
        default:
            break;
    }
}

void AppWindow::on_tray_message(LPARAM lparam) {
    switch (static_cast<UINT>(lparam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            handle_tray_command(TrayCommand::Show);
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT point{};
            GetCursorPos(&point);
            const TrayCommand command =
                tray_.show_menu(hwnd_, point, state_.engine.busy());
            handle_tray_command(command);
            break;
        }
        default:
            break;
    }
}

void AppWindow::refresh_health(bool expensive) {
    poll_engine(state_);
    if (expensive) {
        run_expensive_diagnostics(state_);
    } else {
        collect_display_and_vdd(state_.diagnostics);
        collect_telemetry(state_.paths, state_.diagnostics);
        collect_engine_log(state_.paths, state_.diagnostics);
    }
}

void AppWindow::update_tooltip() {
    const std::wstring state_text = engine_process_state_label(state_.engine.snapshot().state);
    tray_.set_tooltip(L"RayNeo Spatial - engine " + state_text);
}

void AppWindow::update_animation_timer() {
    const bool toast_active = !state_.toast.empty() && state_.now_s < state_.toast_expiry_s;
    const bool wanted = state_.starting || toast_active;
    if (wanted == animation_timer_) {
        return;
    }
    animation_timer_ = wanted;
    if (wanted) {
        SetTimer(hwnd_, kTimerAnimation, kAnimationIntervalMs, nullptr);
    } else {
        KillTimer(hwnd_, kTimerAnimation);
        invalidate();
    }
}

void AppWindow::add_event(const std::wstring& text) {
    state_.add_event(text);
}

namespace {

constexpr WORD kPresetDlgEdit = 1001;
constexpr WORD kPresetDlgError = 1002;

struct PresetNameDialog {
    std::wstring initial;
    std::wstring result;
    bool accepted = false;
};

void dlg_push_u16(std::vector<BYTE>& blob, WORD value) {
    blob.push_back(static_cast<BYTE>(value & 0xFF));
    blob.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
}

void dlg_push_u32(std::vector<BYTE>& blob, DWORD value) {
    dlg_push_u16(blob, static_cast<WORD>(value & 0xFFFF));
    dlg_push_u16(blob, static_cast<WORD>((value >> 16) & 0xFFFF));
}

void dlg_push_wstr(std::vector<BYTE>& blob, const wchar_t* text) {
    for (const wchar_t* p = text;; ++p) {
        dlg_push_u16(blob, static_cast<WORD>(*p));
        if (*p == L'\0') {
            break;
        }
    }
}

void dlg_align_u32(std::vector<BYTE>& blob) {
    while (blob.size() % 4 != 0) {
        blob.push_back(0);
    }
}

void dlg_push_item(std::vector<BYTE>& blob, DWORD style, short x, short y, short cx, short cy,
                   WORD id, WORD class_atom, const wchar_t* text) {
    dlg_align_u32(blob);
    dlg_push_u32(blob, style);
    dlg_push_u32(blob, 0);
    dlg_push_u16(blob, static_cast<WORD>(x));
    dlg_push_u16(blob, static_cast<WORD>(y));
    dlg_push_u16(blob, static_cast<WORD>(cx));
    dlg_push_u16(blob, static_cast<WORD>(cy));
    dlg_push_u16(blob, id);
    dlg_push_u16(blob, 0xFFFF);
    dlg_push_u16(blob, class_atom);
    dlg_push_wstr(blob, text);
    dlg_push_u16(blob, 0);
}

std::vector<BYTE> build_preset_name_template() {
    std::vector<BYTE> blob;
    dlg_push_u32(blob, DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    dlg_push_u32(blob, 0);
    dlg_push_u16(blob, 5);
    dlg_push_u16(blob, 0);
    dlg_push_u16(blob, 0);
    dlg_push_u16(blob, 178);
    dlg_push_u16(blob, 74);
    dlg_push_u16(blob, 0);
    dlg_push_u16(blob, 0);
    dlg_push_wstr(blob, L"Save preset");
    dlg_push_u16(blob, 8);
    dlg_push_wstr(blob, L"MS Shell Dlg");
    dlg_push_item(blob, WS_CHILD | WS_VISIBLE | SS_LEFT, 7, 7, 164, 8, 0xFFFF, 0x0082,
                  L"Preset &name:");
    dlg_push_item(blob, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 7, 17,
                  164, 14, kPresetDlgEdit, 0x0081, L"");
    dlg_push_item(blob, WS_CHILD | WS_VISIBLE | SS_LEFT, 7, 33, 164, 18, kPresetDlgError, 0x0082,
                  L"");
    dlg_push_item(blob, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 67, 56, 50, 14,
                  IDOK, 0x0080, L"OK");
    dlg_push_item(blob, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 121, 56, 50, 14,
                  IDCANCEL, 0x0080, L"Cancel");
    return blob;
}

}  // namespace

INT_PTR CALLBACK AppWindow::preset_name_dlg_proc(HWND dlg, UINT message, WPARAM wparam,
                                                 LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dlg, DWLP_USER, lparam);
        const PresetNameDialog* state = reinterpret_cast<PresetNameDialog*>(lparam);
        SendDlgItemMessageW(dlg, kPresetDlgEdit, EM_SETLIMITTEXT, 64, 0);
        if (state != nullptr && !state->initial.empty()) {
            SetDlgItemTextW(dlg, kPresetDlgEdit, state->initial.c_str());
            SendDlgItemMessageW(dlg, kPresetDlgEdit, EM_SETSEL, 0, -1);
        }
        return TRUE;
    }
    if (message != WM_COMMAND) {
        return FALSE;
    }
    const WORD id = LOWORD(wparam);
    if (id == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    if (id != IDOK) {
        return FALSE;
    }
    wchar_t buffer[65] = {};
    GetDlgItemTextW(dlg, kPresetDlgEdit, buffer, 65);
    std::string error;
    const std::string name = utf8_from_wide(buffer);
    if (!layout_preset_name_valid(name, error)) {
        SetDlgItemTextW(dlg, kPresetDlgError, wide_from_utf8(error).c_str());
        MessageBeep(MB_ICONWARNING);
        return TRUE;
    }
    PresetNameDialog* state =
        reinterpret_cast<PresetNameDialog*>(GetWindowLongPtrW(dlg, DWLP_USER));
    if (state != nullptr) {
        state->result = buffer;
        state->accepted = true;
    }
    EndDialog(dlg, IDOK);
    return TRUE;
}

bool AppWindow::prompt_preset_name(std::string& name) {
    PresetNameDialog state;
    state.initial = wide_from_utf8(state_.loaded_preset);
    const std::vector<BYTE> templ = build_preset_name_template();
    const INT_PTR rc = DialogBoxIndirectParamW(
        instance_, reinterpret_cast<const DLGTEMPLATE*>(templ.data()), hwnd_, preset_name_dlg_proc,
        reinterpret_cast<LPARAM>(&state));
    if (rc != IDOK || !state.accepted) {
        return false;
    }
    name = utf8_from_wide(state.result);
    return true;
}

}  // namespace gt::ui
