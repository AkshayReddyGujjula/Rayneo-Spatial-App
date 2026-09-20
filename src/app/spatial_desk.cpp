#include "imu/imu_source.h"
#include "imu/orientation_calibration.h"
#include "render/renderer.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using SteadyClock = std::chrono::steady_clock;

struct MonitorEntry {
    HMONITOR handle = nullptr;
    RECT rect{};
    bool primary = false;
    std::wstring name;
};

struct Options {
    int monitor = -1;
    float fov = 46.0f;
    bool no_imu = false;
    bool freeze_still = true;
    double seconds = 0.0;
    std::string log_path;
    std::string calibration_path;
};

struct AppState {
    bool quit = false;
    gt::ImuSource* imu = nullptr;
};

AppState* g_app = nullptr;

BOOL CALLBACK monitor_enum_proc(HMONITOR handle, HDC, LPRECT, LPARAM data) {
    auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(handle, &info)) {
        MonitorEntry entry;
        entry.handle = handle;
        entry.rect = info.rcMonitor;
        entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        entry.name = info.szDevice;
        list->push_back(entry);
    }
    return TRUE;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CLOSE:
        case WM_DESTROY:
            if (g_app != nullptr) {
                g_app->quit = true;
            }
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE && g_app != nullptr) {
                g_app->quit = true;
            }
            if (wparam == 'R' && g_app != nullptr && g_app->imu != nullptr) {
                g_app->imu->recenter();
                std::printf("  recentered\n");
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void print_usage() {
    std::printf(
        "spatial_desk - head-tracked spatial view on the RayNeo GT\n"
        "  --monitor N   display index to render on (default: first non-primary)\n"
        "  --fov DEG     virtual horizontal field of view (default 46)\n"
        "  --seconds N   exit after N seconds (0 = run until ESC)\n"
        "  --no-imu      run without head tracking (fixed camera)\n"
        "  --freeze-still  hold the view steady while the head is still (default)\n"
        "  --no-freeze-still  always follow the raw head pose\n"
        "  --log FILE     append a diagnostic CSV (elapsed, gyro, bias, pose, still)\n"
        "  --calibration FILE  sensor-to-head calibration (default config/orientation.json)\n");
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--monitor") == 0 && i + 1 < argc) {
            opt.monitor = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--fov") == 0 && i + 1 < argc) {
            opt.fov = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--no-imu") == 0) {
            opt.no_imu = true;
        } else if (std::strcmp(a, "--freeze-still") == 0) {
            opt.freeze_still = true;
        } else if (std::strcmp(a, "--no-freeze-still") == 0) {
            opt.freeze_still = false;
        } else if (std::strcmp(a, "--seconds") == 0 && i + 1 < argc) {
            opt.seconds = std::atof(argv[++i]);
        } else if (std::strcmp(a, "--log") == 0 && i + 1 < argc) {
            opt.log_path = argv[++i];
        } else if (std::strcmp(a, "--calibration") == 0 && i + 1 < argc) {
            opt.calibration_path = argv[++i];
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            print_usage();
            return false;
        } else {
            std::printf("unknown option: %s\n", a);
            print_usage();
            return false;
        }
    }
    return true;
}

void enable_dpi_awareness() {
    using SetDpiFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        auto fn = reinterpret_cast<SetDpiFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn != nullptr && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    SetProcessDPIAware();
}

std::string default_calibration_path() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return "config/orientation.json";
    }
    executable.resize(length);
    const std::filesystem::path executable_path(executable);
    return (executable_path.parent_path().parent_path() / "config" / "orientation.json").string();
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        return 2;
    }
    if (opt.calibration_path.empty()) {
        opt.calibration_path = default_calibration_path();
    }
    if (!std::isfinite(opt.fov) || opt.fov < 20.0f || opt.fov > 150.0f ||
        !std::isfinite(opt.seconds) || opt.seconds < 0.0) {
        std::printf("invalid arguments: fov must be 20..150 degrees and seconds must be non-negative\n");
        return 2;
    }

    std::array<float, 9> sensor_to_head;
    if (!opt.no_imu) {
        std::string calibration_error;
        if (!gt::load_orientation_calibration(opt.calibration_path, sensor_to_head, calibration_error)) {
            std::printf("orientation calibration required: %s (%s)\n", opt.calibration_path.c_str(),
                        calibration_error.c_str());
            std::printf("run orientation_calibrate.exe while wearing the glasses, then start spatial_desk again\n");
            return 1;
        }
    }

    enable_dpi_awareness();

    std::vector<MonitorEntry> monitors;
    EnumDisplayMonitors(nullptr, nullptr, monitor_enum_proc, reinterpret_cast<LPARAM>(&monitors));
    std::printf("monitors:\n");
    int selected = -1;
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& m = monitors[i];
        std::printf("  [%zu] %ls %dx%d%s\n", i, m.name.c_str(), m.rect.right - m.rect.left,
                    m.rect.bottom - m.rect.top, m.primary ? " (primary)" : "");
    }
    if (opt.monitor >= 0 && opt.monitor < static_cast<int>(monitors.size())) {
        selected = opt.monitor;
    } else {
        for (size_t i = 0; i < monitors.size(); ++i) {
            if (!monitors[i].primary) {
                selected = static_cast<int>(i);
                break;
            }
        }
        if (selected < 0 && !monitors.empty()) {
            selected = 0;
        }
    }
    if (selected < 0) {
        std::printf("no monitors found\n");
        return 1;
    }
    const MonitorEntry& target = monitors[selected];
    const int width = target.rect.right - target.rect.left;
    const int height = target.rect.bottom - target.rect.top;
    std::printf("rendering on [%d] %dx%d\n", selected, width, height);
    std::printf("controls: ESC quit, R recenter head tracking\n");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RayNeoSpatialDesk";
    if (RegisterClassExW(&wc) == 0) {
        std::printf("RegisterClassExW failed (%lu)\n", GetLastError());
        return 1;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"RayNeoSpatialDesk", L"RayNeo Spatial Desk",
                                WS_POPUP, target.rect.left, target.rect.top, width, height,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        std::printf("CreateWindowExW failed (%lu)\n", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    gt::Renderer renderer;
    std::string error;
    if (!renderer.init(hwnd, static_cast<uint32_t>(width), static_cast<uint32_t>(height), error)) {
        std::printf("renderer init failed: %s\n", error.c_str());
        DestroyWindow(hwnd);
        return 1;
    }

    gt::ImuSource imu;
    AppState state;
    state.imu = opt.no_imu ? nullptr : &imu;
    g_app = &state;
    if (!opt.no_imu) {
        imu.set_sensor_to_head(sensor_to_head);
        std::printf("loaded orientation calibration: %s\n", opt.calibration_path.c_str());
        imu.set_freeze_when_still(opt.freeze_still);
        imu.start();
    }

    const auto start = SteadyClock::now();
    uint64_t frames = 0;
    uint64_t frames_at_stat = 0;
    double next_stat = 1.0;
    const gt::CameraSigns signs;

    std::ofstream diagnostics;
    if (!opt.log_path.empty()) {
        diagnostics.open(opt.log_path, std::ios::out | std::ios::trunc);
        diagnostics << "elapsed_s,tick_100us,gx_raw,gy_raw,gz_raw,bias_x,bias_y,bias_z,yaw_deg,pitch_deg,"
                       "roll_deg,still\n";
    }
    int log_rows = 0;

    while (!state.quit) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            state.quit = true;
        }
        const double elapsed = std::chrono::duration<double>(SteadyClock::now() - start).count();
        if (opt.seconds > 0.0 && elapsed >= opt.seconds) {
            state.quit = true;
        }

        const gt::Quat head = opt.no_imu ? gt::Quat{} : imu.orientation();
        renderer.set_signs(signs);
        renderer.render(head, opt.fov, static_cast<float>(elapsed));
        renderer.wait_for_frame();
        if (!renderer.present()) {
            std::printf("present failed, exiting\n");
            break;
        }
        ++frames;

        if (diagnostics.is_open()) {
            const gt::Euler le = gt::camera_applied_euler(head, signs);
            const gt::Vec3 g = imu.last_gyro_degs();
            const gt::Vec3 b = imu.gyro_bias_degs();
            char row[256];
            std::snprintf(row, sizeof(row), "%.3f,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n",
                          elapsed, imu.last_tick(), g.x, g.y, g.z, b.x, b.y, b.z, le.yaw_deg, le.pitch_deg,
                          le.roll_deg, imu.still() ? 1 : 0);
            diagnostics << row;
            if (++log_rows % 120 == 0) {
                diagnostics.flush();
            }
        }

        if (elapsed >= next_stat) {
            const gt::Euler e = gt::camera_applied_euler(head, signs);
            const double fps = static_cast<double>(frames - frames_at_stat) / (elapsed - (next_stat - 1.0));
            if (opt.no_imu) {
                std::printf("[%.0fs] fps=%.1f  no-imu\n", elapsed, fps);
            } else {
                const gt::Vec3 bias = imu.gyro_bias_degs();
                std::printf(
                    "[%.0fs] fps=%.1f  imu=%.1fHz (%s)  bias=(%+.2f,%+.2f,%+.2f)  yaw=%7.2f pitch=%6.2f roll=%6.2f\n",
                    elapsed, fps, imu.sample_rate_hz(), imu.status().c_str(), bias.x, bias.y, bias.z,
                    e.yaw_deg, e.pitch_deg, e.roll_deg);
            }
            frames_at_stat = frames;
            next_stat += 1.0;
        }
    }

    std::printf("shutting down\n");
    if (diagnostics.is_open()) {
        diagnostics.close();
    }
    if (!opt.no_imu) {
        imu.stop();
    }
    g_app = nullptr;
    renderer.shutdown();
    DestroyWindow(hwnd);
    return 0;
}
