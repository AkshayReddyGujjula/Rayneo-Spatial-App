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
constexpr double kBallTauS = 0.05;     // eases the ball between ~22 Hz live lines
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
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    fonts_.destroy();
    ball_valid_ = false;
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
    if (view_.has_live) {
        const RECT client{0, 0, monitor_.right - monitor_.left, monitor_.bottom - monitor_.top};
        const CalGuidePoint target =
            ui::cal_guide_target_ball(client, view_, ui::glasses_guide_style(fonts_), fov_h_deg_);
        if (!ball_valid_) {
            ball_ = target;
            ball_valid_ = true;
        } else {
            const float k = static_cast<float>(1.0 - std::exp(-dt / kBallTauS));
            ball_.x += (target.x - ball_.x) * k;
            ball_.y += (target.y - ball_.y) * k;
        }
    } else {
        ball_valid_ = false;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void CalGuideWindow::paint() {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1, width), std::max(1, height));
    if (memory != nullptr && bitmap != nullptr) {
        const HGDIOBJ previous = SelectObject(memory, bitmap);
        {
            ui::Canvas canvas(memory, client, ui::dark_theme(), 96);
            const ui::CalGuideStyle style = ui::glasses_guide_style(fonts_);
            ui::paint_cal_guide(canvas, client, view_, style, fov_h_deg_, ball_valid_ ? &ball_ : nullptr);
        }
        BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
        SelectObject(memory, previous);
    }
    if (bitmap != nullptr) {
        DeleteObject(bitmap);
    }
    if (memory != nullptr) {
        DeleteDC(memory);
    }
    EndPaint(hwnd_, &ps);
}

}  // namespace gt
