#include "imu/imu_source.h"
#include "imu/orientation_calibration.h"
#include "layout/layout.h"
#include "render/renderer.h"
#include "vdd/display_config.h"
#include "vdd/vdd_client.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
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
    std::wstring description;
    bool glasses = false;
};

struct Options {
    int monitor = -1;
    bool monitor_explicit = false;
    float fov = 46.0f;
    bool fov_explicit = false;
    bool no_imu = false;
    bool freeze_still = true;
    double seconds = 0.0;
    std::string log_path;
    std::string calibration_path;
    std::string layout_path;
    bool virtual_displays = true;
};

struct AppState {
    bool quit = false;
    gt::ImuSource* imu = nullptr;
};

AppState* g_app = nullptr;

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ != nullptr) CloseHandle(handle_);
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

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
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(info.szDevice, 0, &monitor, 0)) {
            entry.description = monitor.DeviceString;
            std::wstring searchable = entry.description + L" " + monitor.DeviceID;
            for (wchar_t& character : searchable) {
                character = static_cast<wchar_t>(std::towlower(character));
            }
            const bool branded = searchable.find(L"smartglasses") != std::wstring::npos ||
                                 searchable.find(L"rayneo") != std::wstring::npos;
            const bool tcl_secondary = !entry.primary &&
                                       searchable.find(L"tcl") != std::wstring::npos;
            entry.glasses = branded || tcl_secondary;
        }
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
        "  --calibration FILE  sensor-to-head calibration (default config/orientation.json)\n"
        "  --layout FILE  screen layout (default config/layouts/default.json)\n"
        "  --no-virtual-displays  render labelled test screens without Parsec VDD\n");
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--monitor") == 0 && i + 1 < argc) {
            opt.monitor = std::atoi(argv[++i]);
            opt.monitor_explicit = true;
        } else if (std::strcmp(a, "--fov") == 0 && i + 1 < argc) {
            opt.fov = static_cast<float>(std::atof(argv[++i]));
            opt.fov_explicit = true;
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
        } else if (std::strcmp(a, "--layout") == 0 && i + 1 < argc) {
            opt.layout_path = argv[++i];
        } else if (std::strcmp(a, "--no-virtual-displays") == 0) {
            opt.virtual_displays = false;
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

std::string default_repo_path(const std::filesystem::path& relative) {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return relative.string();
    }
    executable.resize(length);
    const std::filesystem::path executable_path(executable);
    return (executable_path.parent_path().parent_path() / relative).string();
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        return 2;
    }
    if (opt.calibration_path.empty()) {
        opt.calibration_path = default_repo_path(std::filesystem::path("config") / "orientation.json");
    }
    if (opt.layout_path.empty()) {
        opt.layout_path =
            default_repo_path(std::filesystem::path("config") / "layouts" / "default.json");
    }
    if (!std::isfinite(opt.fov) || opt.fov < 20.0f || opt.fov > 150.0f ||
        !std::isfinite(opt.seconds) || opt.seconds < 0.0) {
        std::printf("invalid arguments: fov must be 20..150 degrees and seconds must be non-negative\n");
        return 2;
    }

    UniqueHandle instance(CreateMutexW(nullptr, FALSE, L"Local\\RayNeoSpatialDesk"));
    if (instance.get() == nullptr) {
        std::printf("could not create the single-instance guard (Windows error %lu)\n",
                    GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::printf("spatial_desk is already running\n");
        return 1;
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
    gt::Layout layout;
    std::string layout_error;
    if (!gt::load_layout(opt.layout_path, layout, layout_error)) {
        std::printf("layout load failed: %s (%s)\n", opt.layout_path.c_str(), layout_error.c_str());
        return 1;
    }
    if (!opt.fov_explicit) {
        opt.fov = layout.fov_deg;
    }

    gt::VddClient vdd;
    std::vector<gt::ConfiguredDisplay> virtual_displays;
    if (opt.virtual_displays) {
        std::string vdd_error;
        if (!vdd.connect(layout.screens.size(), vdd_error) ||
            !gt::configure_virtual_displays(vdd.display_indices(), 1920, 1080, 120,
                                            virtual_displays, vdd_error)) {
            std::printf("virtual display startup failed: %s\n", vdd_error.c_str());
            std::printf("install the signed Parsec VDD, use Windows Extend mode, or pass "
                        "--no-virtual-displays for the renderer-only diagnostic\n");
            return 1;
        }
        std::printf("created %zu virtual displays (Parsec VDD version %d)\n",
                    virtual_displays.size(), vdd.driver_version());
        for (const auto& display : virtual_displays) {
            std::printf("  VDD[%d] %dx%d@%dHz at (%d,%d)\n", display.driver_index,
                        display.width, display.height, display.refresh_hz, display.x, display.y);
        }
    }

    enable_dpi_awareness();

    std::vector<MonitorEntry> monitors;
    EnumDisplayMonitors(nullptr, nullptr, monitor_enum_proc, reinterpret_cast<LPARAM>(&monitors));
    std::printf("monitors:\n");
    int selected = -1;
    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto& m = monitors[i];
        std::printf("  [%zu] %ls %ls %dx%d%s%s\n", i, m.name.c_str(), m.description.c_str(),
                    m.rect.right - m.rect.left, m.rect.bottom - m.rect.top,
                    m.primary ? " (primary)" : "", m.glasses ? " (RayNeo)" : "");
    }
    if (opt.monitor_explicit) {
        if (opt.monitor < 0 || opt.monitor >= static_cast<int>(monitors.size())) {
            std::printf("invalid monitor index %d; choose one of the indices listed above\n",
                        opt.monitor);
            return 2;
        }
        selected = opt.monitor;
    } else {
        for (size_t i = 0; i < monitors.size(); ++i) {
            if (monitors[i].glasses) {
                selected = static_cast<int>(i);
                break;
            }
        }
    }
    if (selected < 0) {
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
    if (!renderer.set_layout(layout, error)) {
        std::printf("renderer layout failed: %s\n", error.c_str());
        renderer.shutdown();
        DestroyWindow(hwnd);
        return 1;
    }
    std::printf("loaded layout: %s (%zu screens)\n", opt.layout_path.c_str(), layout.screens.size());

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
    std::error_code layout_time_error;
    auto layout_write_time = std::filesystem::last_write_time(opt.layout_path, layout_time_error);
    double next_layout_check = 0.5;

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

        if (elapsed >= next_layout_check) {
            next_layout_check = elapsed + 0.5;
            std::error_code time_error;
            const auto write_time = std::filesystem::last_write_time(opt.layout_path, time_error);
            if (!time_error && (layout_time_error || write_time != layout_write_time)) {
                layout_write_time = write_time;
                layout_time_error.clear();
                gt::Layout candidate;
                std::string reload_error;
                const size_t previous_display_count = layout.screens.size();
                bool displays_changed = false;
                bool reload_ok = gt::load_layout(opt.layout_path, candidate, reload_error);
                if (reload_ok && opt.virtual_displays) {
                    displays_changed = candidate.screens.size() != previous_display_count;
                    reload_ok = vdd.resize(candidate.screens.size(), reload_error) &&
                                gt::configure_virtual_displays(
                                    vdd.display_indices(), 1920, 1080, 120, virtual_displays,
                                    reload_error);
                }
                if (reload_ok) {
                    reload_ok = renderer.set_layout(candidate, reload_error);
                }
                if (reload_ok) {
                    layout = std::move(candidate);
                    if (!opt.fov_explicit) {
                        opt.fov = layout.fov_deg;
                    }
                    std::printf("reloaded layout (%zu screens)\n", layout.screens.size());
                } else {
                    if (opt.virtual_displays && displays_changed) {
                        std::string rollback_error;
                        if (!vdd.resize(previous_display_count, rollback_error) ||
                            !gt::configure_virtual_displays(vdd.display_indices(), 1920, 1080, 120,
                                                            virtual_displays, rollback_error)) {
                            std::printf("virtual display rollback failed: %s\n",
                                        rollback_error.c_str());
                            state.quit = true;
                        }
                    }
                    std::printf("layout reload ignored: %s\n", reload_error.c_str());
                }
            }
        }

        if (opt.virtual_displays && vdd.consecutive_keepalive_failures() >= 5) {
            std::printf("Parsec VDD keepalive failed repeatedly; exiting before its watchdog "
                        "removes the desktops\n");
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
    vdd.disconnect();
    g_app = nullptr;
    renderer.shutdown();
    DestroyWindow(hwnd);
    return 0;
}
