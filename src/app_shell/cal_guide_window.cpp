#include "app_shell/cal_guide_window.h"

#include "app_shell/diagnostics.h"
#include "app_shell/ui_theme.h"
#include "app_shell/ui_widgets.h"

#include <algorithm>
#include <cmath>

namespace gt {
namespace {

constexpr wchar_t kClassName[] = L"RayNeoSpatialCalibrationGuide";
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameIntervalMs = 20;  // ~40-50 fps after timer rounding; >= 30 fps
constexpr double kMonitorProbeIntervalS = 2.0;

}  // namespace

CalGuideWindow::~CalGuideWindow() {
    close();
}

LRESULT CALLBACK CalGuideWindow::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<CalGuideWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            break;
        }
        case WM_NCHITTEST:
            return HTTRANSPARENT;  // click-through
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;  // painted whole from a back buffer
        case WM_DISPLAYCHANGE:
            if (self != nullptr) {
                self->on_display_change();
                return 0;
            }
            break;
        case WM_TIMER:
            if (self != nullptr && wparam == kFrameTimer) {
                self->on_frame();
                return 0;
            }
            break;
        case WM_PAINT:
            if (self != nullptr) {
                self->paint();
                return 0;
            }
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool CalGuideWindow::create(const RECT& monitor) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &CalGuideWindow::window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kClassName;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        registered = true;
    }
    monitor_ = monitor;
    // Layered + transparent = click-through; no-activate + tool window = never
    // takes focus or a taskbar button.
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
                            kClassName, L"Calibration guide", WS_POPUP, monitor.left, monitor.top,
                            monitor.right - monitor.left, monitor.bottom - monitor.top, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    fonts_.create(monitor.bottom - monitor.top);
    ball_valid_ = false;
    QueryPerformanceCounter(&last_frame_);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetTimer(hwnd_, kFrameTimer, kFrameIntervalMs, nullptr);
    return true;
}

void CalGuideWindow::close() {
    if (hwnd_ != nullptr) {
        KillTimer(hwnd_, kFrameTimer);
        // Cleared first: DestroyWindow may run from this window's own
        // WM_DISPLAYCHANGE, and nothing may touch the handle afterwards.
        const HWND hwnd = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(hwnd);
    }
    release_back_buffer();
    fonts_.destroy();
    ball_valid_ = false;
}

void CalGuideWindow::release_back_buffer() {
    if (back_dc_ != nullptr) {
        if (back_previous_ != nullptr) {
            SelectObject(back_dc_, back_previous_);
        }
        DeleteDC(back_dc_);
    }
    if (back_bitmap_ != nullptr) {
        DeleteObject(back_bitmap_);
    }
    back_dc_ = nullptr;
    back_bitmap_ = nullptr;
    back_previous_ = nullptr;
    back_width_ = 0;
    back_height_ = 0;
}

void CalGuideWindow::on_display_change() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT monitor{};
    if (!find_glasses_monitor_rect(monitor)) {
        // The glasses display went away: close; update() probes again.
        close();
        next_monitor_probe_s_ = 0.0;
        return;
    }
    if (EqualRect(&monitor, &monitor_)) {
        return;
    }
    monitor_ = monitor;
    SetWindowPos(hwnd_, HWND_TOPMOST, monitor.left, monitor.top, monitor.right - monitor.left,
                 monitor.bottom - monitor.top, SWP_NOACTIVATE);
    fonts_.create(monitor.bottom - monitor.top);
    release_back_buffer();
    ball_valid_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CalGuideWindow::update(const CalRunState& run, double now_s, float fov_h_deg) {
    view_ = cal_guide_view(run, now_s);
    fov_h_deg_ = fov_h_deg;
    if (!view_.visible) {
        close();
        next_monitor_probe_s_ = 0.0;
        return;
    }
    if (hwnd_ == nullptr) {
        if (now_s < next_monitor_probe_s_) {
            return;
        }
        next_monitor_probe_s_ = now_s + kMonitorProbeIntervalS;
        RECT monitor{};
        if (!find_glasses_monitor_rect(monitor) || !create(monitor)) {
            return;  // no glasses display: the PC panel guide is enough
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CalGuideWindow::on_frame() {
    if (hwnd_ == nullptr) {
        return;
    }
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double dt = std::clamp(static_cast<double>(now.QuadPart - last_frame_.QuadPart) /
                                     static_cast<double>(std::max<LONGLONG>(1, frequency.QuadPart)),
                                 0.0, 0.2);
    last_frame_ = now;
    // Repaint only when something moved: update() repaints for each new
    // sample or state; here only the eased ball (and its disappearance).
    bool changed = false;
    if (view_.has_live) {
        const RECT client{0, 0, monitor_.right - monitor_.left, monitor_.bottom - monitor_.top};
        const CalGuidePoint target =
            ui::cal_guide_target_ball(client, view_, ui::glasses_guide_style(fonts_), fov_h_deg_);
        if (!ball_valid_) {
            ball_ = target;
            ball_valid_ = true;
            changed = true;
        } else {
            changed = cal_guide_ease_ball(ball_, target, dt);
        }
    } else if (ball_valid_) {
        ball_valid_ = false;
        changed = true;
    }
    if (changed) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void CalGuideWindow::paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (back_dc_ == nullptr || back_width_ != width || back_height_ != height) {
        release_back_buffer();
        back_dc_ = CreateCompatibleDC(dc);
        back_bitmap_ = ui::create_paint_bitmap(dc, width, height);
        if (back_dc_ != nullptr && back_bitmap_ != nullptr) {
            back_previous_ = SelectObject(back_dc_, back_bitmap_);
            back_width_ = width;
            back_height_ = height;
        } else {
            release_back_buffer();
        }
    }
    if (back_dc_ != nullptr) {
        {
            ui::Canvas canvas(back_dc_, client, ui::dark_theme(), 96);
            const ui::CalGuideStyle style = ui::glasses_guide_style(fonts_);
            ui::paint_cal_guide(canvas, client, view_, style, fov_h_deg_, ball_valid_ ? &ball_ : nullptr);
        }
        BitBlt(dc, 0, 0, width, height, back_dc_, 0, 0, SRCCOPY);
    }
    EndPaint(hwnd_, &ps);
}

}  // namespace gt
