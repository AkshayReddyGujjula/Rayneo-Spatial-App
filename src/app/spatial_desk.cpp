#include "app/engine_protocol.h"
#include "capture/desktop_duplication.h"
#include "imu/imu_source.h"
#include "imu/pose_smoother.h"
#include "imu/orientation_calibration.h"
#include "layout/layout.h"
#include "render/renderer.h"
#include "vdd/display_config.h"
#include "vdd/vdd_client.h"
#include "util/utf8_path.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
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
    bool virtual_display = false;
};

// Per-screen capture throttle. Tiers index active_fps / mid_fps / idle_fps:
// 0 = active (closest to the view), 1 = mid, 2 = idle.
struct CaptureTierState {
    int tier = 1;
    double next_poll_time = 0.0;
};

constexpr int kFatalCaptureFailures = 120;

// Machine-readable engine status, rewritten once per second and at every state
// change. The controller reads it at up to 2 Hz (see app/engine_protocol.h), and
// it is the only channel that survives the engine being launched by hand.
struct StatusReport {
    std::string path;
    std::string state = gt::kStatusStarting;
    std::string mode = gt::kModePreview;
    std::string error;
    int screens = 0;
    double fps = 0.0;
    int monitor = -1;
    int monitor_width = 0;
    int monitor_height = 0;
    bool vdd = false;
    bool imu = false;
    bool detached = false;
    double elapsed_s = 0.0;
};

StatusReport g_status;

// Mirrors the armed topology guard for the query reply: set once the takeover
// commits, cleared once the pre-workspace arrangement is restored.
bool g_topology_takeover = false;

struct Options {
    int monitor = -1;
    bool monitor_explicit = false;
    float fov = 46.0f;
    bool fov_explicit = false;
    bool no_imu = false;
    bool smoothing = true;
    int stabilise = gt::kReadingHoldDefault;
    bool mag = true;
    double seconds = 0.0;
    std::string log_path;
    std::string calibration_path;
    std::string layout_path;
    std::string status_path;
    bool virtual_displays = true;
};

enum HotkeyId : int {
    kHotkeyRecenter = 1,
    kHotkeyToggleYaw = 2,
    kHotkeyTogglePitch = 3,
    kHotkeyQuit = 4,
    kHotkeyExitWorkspace = 5,
    kHotkeyCycleStabilise = 6,
};

struct AppState {
    bool quit = false;
    bool reload_layout = false;
    bool virtual_displays = true;
    int screen_count = 0;
    gt::ImuSource* imu = nullptr;
    gt::PoseSmoother view_smoother;
    gt::PoseSmoother::Config view_smoother_config;
    int stabilise_level = gt::kReadingHoldDefault;
    bool yaw_tracking = true;
    bool pitch_tracking = true;
    bool capture_yaw_hold = false;
    bool capture_pitch_hold = false;
    gt::Quat held_yaw_twist{};
    gt::Quat held_pitch_twist{};
};

AppState* g_app = nullptr;

void publish_status(const StatusReport& report) {
    if (report.path.empty()) {
        return;
    }
    std::string text;
    text += std::string(gt::kStatusKeyState) + "=" + report.state + "\n";
    text += std::string(gt::kStatusKeyMode) + "=" + report.mode + "\n";
    text += std::string(gt::kStatusKeyScreens) + "=" + std::to_string(report.screens) + "\n";
    char fps[64];
    std::snprintf(fps, sizeof(fps), "%.2f", report.fps);
    text += std::string(gt::kStatusKeyFps) + "=" + fps + "\n";
    text += std::string(gt::kStatusKeyMonitor) + "=" + std::to_string(report.monitor) + "\n";
    text += std::string(gt::kStatusKeyMonitorWidth) + "=" + std::to_string(report.monitor_width) + "\n";
    text += std::string(gt::kStatusKeyMonitorHeight) + "=" + std::to_string(report.monitor_height) + "\n";
    text += std::string(gt::kStatusKeyVdd) + "=" + (report.vdd ? "1" : "0") + "\n";
    text += std::string(gt::kStatusKeyImu) + "=" + (report.imu ? "1" : "0") + "\n";
    text += std::string(gt::kStatusKeyDetached) + "=" + (report.detached ? "1" : "0") + "\n";
    char elapsed[64];
    std::snprintf(elapsed, sizeof(elapsed), "%.2f", report.elapsed_s);
    text += std::string(gt::kStatusKeyElapsed) + "=" + elapsed + "\n";
    text += std::string(gt::kStatusKeyUpdated) + "=" +
            std::to_string(static_cast<long long>(std::time(nullptr))) + "\n";
    text += std::string(gt::kStatusKeyError) + "=" + gt::engine_status_sanitize(report.error) + "\n";

    // UTF-8 in, wide API out: the status path may live under a non-ASCII
    // profile directory, where the ANSI file APIs silently fail.
    const std::filesystem::path destination = gt::path_from_utf8(report.path);
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::out | std::ios::trunc);
    if (!output) {
        return;
    }
    output << text;
    output.flush();
    if (!output) {
        output.close();
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return;
    }
    output.close();
    // This is an ephemeral one-second heartbeat. Atomic replacement keeps
    // readers from seeing partial reports; forcing every rename to stable
    // storage only adds a disk sync to the render loop.
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
    }
}

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
            // Single matcher (unit-tested in vdd_selftest): the detached-
            // glasses recovery below must agree with the active scan.
            entry.glasses = gt::is_glasses_display(monitor.DeviceString, monitor.DeviceID,
                                                   entry.primary);
            std::wstring searchable = entry.description + L" " + monitor.DeviceID;
            for (wchar_t& character : searchable) {
                character = static_cast<wchar_t>(std::towlower(character));
            }
            entry.virtual_display = searchable.find(L"parsec") != std::wstring::npos ||
                                    searchable.find(L"psccdd") != std::wstring::npos ||
                                    searchable.find(L"vda") != std::wstring::npos;
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
            // No ESC quit or plain-R recenter here: both fired while typing
            // or escaping other apps. Quit is Ctrl+Alt+Q / Ctrl+Shift+\ only
            // (see register_global_hotkeys).
            return 0;
        case WM_HOTKEY:
            if (g_app != nullptr) {
                switch (static_cast<int>(wparam)) {
                    case kHotkeyRecenter:
                        if (g_app->imu != nullptr) {
                            g_app->imu->recenter();
                            g_app->view_smoother.reset();
                            std::printf("  recentered\n");
                        }
                        break;
                    case kHotkeyToggleYaw:
                        g_app->yaw_tracking = !g_app->yaw_tracking;
                        g_app->capture_yaw_hold = !g_app->yaw_tracking;
                        std::printf("  yaw tracking %s\n",
                                    g_app->yaw_tracking ? "on" : "off (view holds yaw)");
                        break;
                    case kHotkeyTogglePitch:
                        g_app->pitch_tracking = !g_app->pitch_tracking;
                        g_app->capture_pitch_hold = !g_app->pitch_tracking;
                        std::printf("  pitch tracking %s\n",
                                    g_app->pitch_tracking ? "on" : "off (view holds pitch)");
                        break;
                    case kHotkeyCycleStabilise:
                        g_app->stabilise_level =
                            (g_app->stabilise_level + 1) % gt::kReadingHoldLevels;
                        gt::apply_reading_hold(g_app->stabilise_level, g_app->view_smoother_config);
                        g_app->view_smoother.configure(g_app->view_smoother_config);
                        std::printf("  reading stabilisation %s\n",
                                    gt::reading_hold_name(g_app->stabilise_level));
                        break;
                    case kHotkeyQuit:
                        g_app->quit = true;
                        break;
                    case kHotkeyExitWorkspace:
                        g_app->quit = true;
                        break;
                    default:
                        break;
                }
            }
            return 0;
        // Private controller messages (app/engine_protocol.h). They are sent by
        // "RayNeo Spatial.exe" with SendMessageTimeout, so every handler must
        // return promptly and must never block on the render loop.
        case gt::kEngineMessageRecenter:
            if (g_app != nullptr && g_app->imu != nullptr) {
                g_app->imu->recenter();
                g_app->view_smoother.reset();
                std::printf("  recentered (controller)\n");
            }
            return 0;
        case gt::kEngineMessageToggleYaw:
            if (g_app != nullptr) {
                g_app->yaw_tracking = !g_app->yaw_tracking;
                g_app->capture_yaw_hold = !g_app->yaw_tracking;
                std::printf("  yaw tracking %s (controller)\n",
                            g_app->yaw_tracking ? "on" : "off (view holds yaw)");
            }
            return 0;
        case gt::kEngineMessageTogglePitch:
            if (g_app != nullptr) {
                g_app->pitch_tracking = !g_app->pitch_tracking;
                g_app->capture_pitch_hold = !g_app->pitch_tracking;
                std::printf("  pitch tracking %s (controller)\n",
                            g_app->pitch_tracking ? "on" : "off (view holds pitch)");
            }
            return 0;
        case gt::kEngineMessageReloadLayout:
            if (g_app != nullptr) {
                g_app->reload_layout = true;
                std::printf("  layout reload requested (controller)\n");
            }
            return 0;
        case gt::kEngineMessageQuit:
            if (g_app != nullptr) {
                g_app->quit = true;
                std::printf("  graceful quit requested (controller)\n");
            }
            return 0;
        case gt::kEngineMessageQuery: {
            if (g_app == nullptr) {
                return 0;
            }
            unsigned flags = 0;
            if (g_app->yaw_tracking) {
                flags |= gt::kEngineFlagYawTracking;
            }
            if (g_app->pitch_tracking) {
                flags |= gt::kEngineFlagPitchTracking;
            }
            if (g_app->virtual_displays) {
                flags |= gt::kEngineFlagVirtualDisplays;
            }
            if (g_app->imu != nullptr) {
                flags |= gt::kEngineFlagHeadTracking;
            }
            if (g_topology_takeover) {
                flags |= gt::kEngineFlagTopologyTakeover;
            }
            flags |= (static_cast<unsigned>(g_app->screen_count) << gt::kEngineScreenCountShift) &
                     gt::kEngineScreenCountMask;
            return static_cast<LRESULT>(flags);
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
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
        "  --seconds N   exit after N seconds (0 = run until quit)\n"
        "  --no-imu      run without head tracking (fixed camera)\n"
        "  --no-smoothing  disable 1-euro view smoothing (raw pose to renderer)\n"
        "  --stabilise L   reading stabilisation off|low|medium|high (default medium);\n"
        "                Ctrl+Alt+S cycles it live\n"
        "  --no-mag        disable the magnetometer heading lock even if calibrated\n"
        "  --freeze-still / --no-freeze-still  obsolete, accepted and ignored (the pose path\n"
        "                is always live; the gyro bias owns steady error)\n"
        "  --log FILE     append a diagnostic CSV (elapsed, gyro, bias, view pose, rest,\n"
        "                adaptation state, corrected rate, escape rollbacks)\n"
        "  --calibration FILE  sensor-to-head calibration (default config/orientation.json)\n"
        "  --layout FILE  screen layout (default config/layouts/default.json)\n"
        "  --no-virtual-displays  render labelled test screens without Parsec VDD\n"
        "  --status FILE  write a key=value engine status file for the controller\n"
        "  workspace mode detaches the laptop panel until exit (its windows move to\n"
        "                the center desktop); Ctrl+Shift+\\ exits the engine\n"
        "  global hotkeys: Ctrl+Shift+R recenter, Ctrl+Alt+Y yaw tracking, "
        "Ctrl+Alt+P pitch tracking, Ctrl+Alt+S reading stabilisation, Ctrl+Alt+Q quit, "
        "Ctrl+Shift:\\ exit workspace\n");
}

bool parse_args(int argc, char** argv, Options& opt, bool& show_help) {
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
        } else if (std::strcmp(a, "--no-smoothing") == 0) {
            opt.smoothing = false;
        } else if (std::strcmp(a, "--stabilise") == 0 && i + 1 < argc) {
            const std::string level = argv[++i];
            bool matched = false;
            for (int n = 0; n < gt::kReadingHoldLevels; ++n) {
                if (level == gt::reading_hold_name(n) || level == std::to_string(n)) {
                    opt.stabilise = n;
                    matched = true;
                }
            }
            if (!matched) {
                std::printf("--stabilise expects off, low, medium or high\n");
                return false;
            }
        } else if (std::strcmp(a, "--no-mag") == 0) {
            opt.mag = false;
        } else if (std::strcmp(a, "--freeze-still") == 0 ||
                   std::strcmp(a, "--no-freeze-still") == 0) {
            std::printf("note: %s is obsolete and ignored; the pose path is always live\n", a);
        } else if (std::strcmp(a, "--seconds") == 0 && i + 1 < argc) {
            opt.seconds = std::atof(argv[++i]);
        } else if (std::strcmp(a, "--log") == 0 && i + 1 < argc) {
            opt.log_path = argv[++i];
        } else if (std::strcmp(a, "--calibration") == 0 && i + 1 < argc) {
            opt.calibration_path = argv[++i];
        } else if (std::strcmp(a, "--layout") == 0 && i + 1 < argc) {
            opt.layout_path = argv[++i];
        } else if (std::strcmp(a, "--status") == 0 && i + 1 < argc) {
            opt.status_path = argv[++i];
        } else if (std::strcmp(a, "--no-virtual-displays") == 0) {
            opt.virtual_displays = false;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            print_usage();
            show_help = true;
            return false;
        } else {
            std::printf("unknown option: %s\n", a);
            print_usage();
            return false;
        }
    }
    return true;
}

void register_global_hotkeys(HWND hwnd) {
    struct Binding {
        int id;
        UINT modifiers;
        UINT virtual_key;
        const wchar_t* label;
    };
    const Binding bindings[] = {
        {kHotkeyRecenter, MOD_CONTROL | MOD_SHIFT, 'R', L"Ctrl+Shift+R (recenter)"},
        {kHotkeyToggleYaw, MOD_CONTROL | MOD_ALT, 'Y', L"Ctrl+Alt+Y (yaw tracking)"},
        {kHotkeyTogglePitch, MOD_CONTROL | MOD_ALT, 'P', L"Ctrl+Alt+P (pitch tracking)"},
        {kHotkeyCycleStabilise, MOD_CONTROL | MOD_ALT, 'S', L"Ctrl+Alt+S (reading stabilisation)"},
        {kHotkeyQuit, MOD_CONTROL | MOD_ALT, 'Q', L"Ctrl+Alt+Q (quit)"},
        {kHotkeyExitWorkspace, MOD_CONTROL | MOD_SHIFT, VK_OEM_5,
         L"Ctrl+Shift+\\ (exit workspace)"},
    };
    for (const Binding& binding : bindings) {
        if (!RegisterHotKey(hwnd, binding.id, binding.modifiers | MOD_NOREPEAT,
                            binding.virtual_key)) {
            std::printf("  hotkey %ls unavailable (already in use by another app)\n", binding.label);
        }
    }
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

float wrap_deg(float degrees) {
    while (degrees > 180.0f) {
        degrees -= 360.0f;
    }
    while (degrees < -180.0f) {
        degrees += 360.0f;
    }
    return degrees;
}

// Locate the repository root by walking up from the executable until a config
// directory is found, so nested build directories keep working. Falls back to
// the historical "two levels below the root" assumption if no marker is found.
std::filesystem::path repo_root_from_executable() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return {};
    }
    executable.resize(length);
    const std::filesystem::path executable_directory =
        std::filesystem::path(executable).parent_path();
    for (std::filesystem::path candidate = executable_directory;; candidate = candidate.parent_path()) {
        std::error_code exists_error;
        if (std::filesystem::exists(candidate / "config" / "layouts" / "default.json",
                                    exists_error)) {
            return candidate;
        }
        exists_error.clear();
        if (std::filesystem::exists(candidate / "config", exists_error)) {
            return candidate;
        }
        if (candidate == candidate.parent_path()) {
            break;
        }
    }
    return executable_directory.parent_path();
}

std::string default_repo_path(const std::filesystem::path& relative) {
    const std::filesystem::path root = repo_root_from_executable();
    if (root.empty()) {
        return gt::utf8_from_path(relative);
    }
    return gt::utf8_from_path(root / relative);
}

// Restores the pre-workspace display topology (laptop panel back, original
// primary) on every exit path once armed: normal shutdown restores explicitly
// and disarms, while early returns and failures restore here.
struct TopologyGuard {
    gt::WorkspaceTopology topo;
    gt::VddClient* vdd = nullptr;
    bool armed = false;
    ~TopologyGuard() {
        try {
            if (armed && topo.active && vdd != nullptr) {
                std::string error;
                if (!gt::restore_display_topology(topo, *vdd, error)) {
                    std::printf("display topology restore failed: %s\n", error.c_str());
                } else {
                    // The failing path already published; re-publish so the
                    // status file stops claiming the panel is detached.
                    g_topology_takeover = false;
                    g_status.detached = false;
                    publish_status(g_status);
                    if (topo.taskbar_state >= 0) {
                        std::printf("  taskbar auto-hide: captured=%d before-restore=%d "
                                    "after=%d%s\n",
                                    topo.taskbar_state, topo.taskbar_before,
                                    topo.taskbar_after,
                                    topo.taskbar_reapplied ? " (re-applied)"
                                                           : " (already correct)");
                    }
                }
            }
        } catch (...) {
        }
    }
    void arm(gt::WorkspaceTopology&& snapshot) {
        topo = std::move(snapshot);
        armed = true;
    }
    void disarm() {
        armed = false;
    }
};

struct WorkspacePlan {
    std::vector<std::pair<int, float>> driver_yaw;  // rank order
    int center_driver = -1;
    std::vector<gt::PlannedDesktop> desktops;
};

// Maps layout screens (by vdd_index) onto created driver indices in bind rank
// order, then plans the desktop arrangement: center primary, yaw-ordered sides.
bool plan_workspace_for_layout(const gt::Layout& layout, const std::vector<int>& driver_indices,
                               WorkspacePlan& plan, std::string& error) {
    if (layout.screens.size() != driver_indices.size()) {
        error = "capture binding requires one virtual display per layout screen";
        return false;
    }
    std::vector<size_t> order(layout.screens.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
        return layout.screens[left].vdd_index < layout.screens[right].vdd_index;
    });
    plan.driver_yaw.clear();
    for (size_t rank = 0; rank < order.size(); ++rank) {
        plan.driver_yaw.emplace_back(driver_indices[rank],
                                     layout.screens[order[rank]].yaw_deg);
    }
    plan.center_driver = gt::select_center_driver(plan.driver_yaw);
    if (plan.center_driver < 0) {
        error = "no virtual displays bound";
        return false;
    }
    plan.desktops = gt::plan_workspace_desktops(plan.driver_yaw, plan.center_driver, 1920);
    return true;
}

std::map<int, std::wstring> vdd_device_names() {
    std::map<int, std::wstring> names;
    for (const gt::VirtualDisplayInfo& info : gt::enumerate_virtual_displays()) {
        names[info.driver_index] = info.device_name;
    }
    return names;
}

bool bind_desktop_captures(
    gt::Renderer& renderer, const gt::Layout& layout,
    const std::vector<gt::ConfiguredDisplay>& displays,
    std::vector<std::unique_ptr<gt::DesktopDuplicator>>& captures, std::string& error) {
    if (layout.screens.size() != displays.size()) {
        error = "capture binding requires one virtual display per layout screen";
        return false;
    }
    std::vector<size_t> logical_order(layout.screens.size());
    std::iota(logical_order.begin(), logical_order.end(), 0);
    std::sort(logical_order.begin(), logical_order.end(), [&](size_t left, size_t right) {
        return layout.screens[left].vdd_index < layout.screens[right].vdd_index;
    });

    std::vector<std::unique_ptr<gt::DesktopDuplicator>> candidate(layout.screens.size());
    for (size_t rank = 0; rank < logical_order.size(); ++rank) {
        const size_t screen_index = logical_order[rank];
        auto duplicator = std::make_unique<gt::DesktopDuplicator>();
        if (!duplicator->initialize(renderer.device(), renderer.context(),
                                    displays[rank].device_name, error)) {
            error = "capture binding for screen '" + layout.screens[screen_index].id +
                    "' failed: " + error;
            return false;
        }
        if (duplicator->backend() == gt::CaptureBackend::GdiFallback) {
            std::printf("  screen '%s': DXGI unavailable, using CPU GDI capture fallback\n",
                        layout.screens[screen_index].id.c_str());
        }
        candidate[screen_index] = std::move(duplicator);
    }
    captures = std::move(candidate);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // The controller redirects stdout/stderr to engine.log. Disable stdio
    // buffering so a crash still leaves the last diagnostic line on disk.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    // First: per-monitor DPI awareness, before the takeover moves any window.
    // A DPI-unaware process reads virtualized window rects on a stale scale
    // that lags primary-display changes, while the display APIs speak
    // physical pixels; the migration must compare and set one frame only.
    enable_dpi_awareness();
    // Wide entry point: argv stays exact for non-ASCII paths, then narrows to
    // UTF-8 once. All file IO below decodes UTF-8 (see util/utf8_path.h).
    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<size_t>(argc < 0 ? 0 : argc));
    for (int i = 0; i < argc; ++i) {
        const std::wstring wide = argv[i] != nullptr ? argv[i] : L"";
        const std::string narrow = gt::utf8_from_wide_text(wide);
        if (!wide.empty() && narrow.empty()) {
            std::printf("invalid command line argument %d: not representable in UTF-8\n", i);
            return 2;
        }
        utf8_args.push_back(narrow);
    }
    std::vector<char*> narrow_argv;
    narrow_argv.reserve(utf8_args.size());
    for (std::string& arg : utf8_args) {
        narrow_argv.push_back(arg.data());
    }
    Options opt;
    bool show_help = false;
    if (!parse_args(argc, narrow_argv.data(), opt, show_help)) {
        if (show_help) {
            return 0;
        }
        g_status.path = opt.status_path;
        g_status.state = gt::kStatusFailed;
        g_status.error = "invalid command line (see --help)";
        publish_status(g_status);
        return 2;
    }
    if (opt.calibration_path.empty()) {
        opt.calibration_path = default_repo_path(std::filesystem::path("config") / "orientation.json");
    }
    if (opt.layout_path.empty()) {
        opt.layout_path =
            default_repo_path(std::filesystem::path("config") / "layouts" / "default.json");
    }
    g_status.path = opt.status_path;
    g_status.mode = opt.virtual_displays ? gt::kModeWorkspace : gt::kModePreview;
    g_status.vdd = opt.virtual_displays;
    g_status.imu = !opt.no_imu;
    publish_status(g_status);

    if (!std::isfinite(opt.fov) || opt.fov < 20.0f || opt.fov > 150.0f ||
        !std::isfinite(opt.seconds) || opt.seconds < 0.0) {
        std::printf("invalid arguments: fov must be 20..150 degrees and seconds must be non-negative\n");
        g_status.state = gt::kStatusFailed;
        g_status.error = "invalid arguments: fov must be 20..150 degrees and seconds must be non-negative";
        publish_status(g_status);
        return 2;
    }

    UniqueHandle instance(CreateMutexW(nullptr, FALSE, L"Local\\RayNeoSpatialDesk"));
    if (instance.get() == nullptr) {
        std::printf("could not create the single-instance guard (Windows error %lu)\n",
                    GetLastError());
        g_status.state = gt::kStatusFailed;
        g_status.error = "could not create the single-instance guard";
        publish_status(g_status);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::printf("spatial_desk is already running\n");
        // The running engine rewrites this file every second, so the stale
        // "failed" marker self-heals within one status interval.
        g_status.state = gt::kStatusFailed;
        g_status.error = "spatial_desk is already running";
        publish_status(g_status);
        return 1;
    }

    std::array<float, 9> sensor_to_head;
    gt::MagCalibration mag_calibration;
    if (!opt.no_imu) {
        std::string calibration_error;
        if (!gt::load_orientation_calibration(opt.calibration_path, sensor_to_head, calibration_error)) {
            std::printf("orientation calibration required: %s (%s)\n", opt.calibration_path.c_str(),
                        calibration_error.c_str());
            std::printf("run orientation_calibrate.exe while wearing the glasses, then start spatial_desk again\n");
            g_status.state = gt::kStatusFailed;
            g_status.error =
                gt::engine_status_sanitize("orientation calibration required: " + calibration_error);
            publish_status(g_status);
            return 1;
        }
        std::string mag_error;
        if (!gt::load_mag_calibration(opt.calibration_path, mag_calibration, mag_error)) {
            // A malformed optional key must not stop the app: run gyro-only.
            std::printf("magnetometer calibration ignored: %s\n", mag_error.c_str());
            mag_calibration = gt::MagCalibration{};
        }
    }
    gt::Layout layout;
    std::string layout_error;
    if (!gt::load_layout(opt.layout_path, layout, layout_error)) {
        std::printf("layout load failed: %s (%s)\n", opt.layout_path.c_str(), layout_error.c_str());
        g_status.state = gt::kStatusFailed;
        g_status.error = gt::engine_status_sanitize("layout load failed: " + layout_error);
        publish_status(g_status);
        return 1;
    }
    if (!opt.fov_explicit) {
        opt.fov = layout.fov_deg;
    }

    gt::VddClient vdd;
    std::vector<gt::ConfiguredDisplay> virtual_displays;
    WorkspacePlan workspace_plan;
    TopologyGuard topology_guard;
    gt::WorkspaceTopology workspace_topo;
    if (opt.virtual_displays) {
        std::string vdd_error;
        std::wstring internal_display;
        WorkspacePlan plan;
        // The pristine arrangement is captured before the VDDs exist:
        // connecting reshuffles it, and the restore pins exactly this back.
        std::vector<gt::DisplayModeSnapshot> pristine;
        bool started = gt::snapshot_attached_displays(pristine, vdd_error);
        if (started) {
            // The laptop panel (if any) is detached by the takeover below, so
            // it must be identified on the pristine topology: connecting the
            // VDDs reshuffles GDI device names, and a post-connect name may
            // not match these snapshots (the window record then finds nothing).
            started = gt::find_internal_display(internal_display, vdd_error);
        }
        gt::MigrationSeed seed;
        if (started && !internal_display.empty()) {
            for (const gt::DisplayModeSnapshot& snapshot : pristine) {
                if (snapshot.device_name != internal_display) {
                    continue;
                }
                seed.laptop_home.left = snapshot.mode.dmPosition.x;
                seed.laptop_home.top = snapshot.mode.dmPosition.y;
                seed.laptop_home.right =
                    seed.laptop_home.left + static_cast<LONG>(snapshot.mode.dmPelsWidth);
                seed.laptop_home.bottom =
                    seed.laptop_home.top + static_cast<LONG>(snapshot.mode.dmPelsHeight);
                seed.have_laptop_home = true;
                gt::snapshot_laptop_windows(seed.laptop_home, seed.windows);
                break;
            }
        }
        if (started && !opt.monitor_explicit) {
            // Fail fast before touching anything: starting a takeover without
            // the glasses just churns the topology and exits at monitor
            // selection.
            std::vector<MonitorEntry> preflight;
            EnumDisplayMonitors(nullptr, nullptr, monitor_enum_proc,
                                reinterpret_cast<LPARAM>(&preflight));
            bool glasses_present = false;
            for (const MonitorEntry& monitor : preflight) {
                if (monitor.glasses) {
                    glasses_present = true;
                    break;
                }
            }
            if (!glasses_present) {
                // Connected but detached ("Disconnect this display"
                // persists): GDI re-attach first, CCD path reactivation for
                // the deeper disconnect (GDI loses the EDID while the target
                // persists), then one rescan before failing.
                std::wstring detached;
                bool recovered = false;
                if (gt::find_detached_glasses_display(detached)) {
                    std::printf("found the disconnected glasses display (%ls); re-attaching\n",
                                detached.c_str());
                    std::string recover_error;
                    recovered = gt::reattach_detached_glasses(recover_error);
                    if (!recovered) {
                        std::printf("glasses re-attach failed: %s\n", recover_error.c_str());
                    }
                }
                if (!recovered) {
                    // Silent unless it works; the failure dump below covers
                    // the rest (same rule as the wait loop).
                    std::string path_error;
                    if (gt::reactivate_glasses_path(path_error)) {
                        std::printf("re-activated the glasses display path\n");
                    }
                }
                preflight.clear();
                EnumDisplayMonitors(nullptr, nullptr, monitor_enum_proc,
                                    reinterpret_cast<LPARAM>(&preflight));
                for (const MonitorEntry& monitor : preflight) {
                    if (monitor.glasses) {
                        glasses_present = true;
                        break;
                    }
                }
            }
            if (!glasses_present) {
                std::printf("the RayNeo glasses display was not found; connect it and use "
                            "Extend mode (Win+P), then start again\n");
                std::printf("display landscape at failure:\n%s",
                            gt::describe_display_landscape().c_str());
                g_status.state = gt::kStatusFailed;
                g_status.error = "the RayNeo glasses display was not found";
                publish_status(g_status);
                return 1;
            }
        }
        if (started && !gt::wait_for_no_virtual_displays(10000)) {
            std::printf("warning: stale virtual displays are still attached; continuing\n");
        }
        if (started) {
            started = vdd.connect(layout.screens.size(), vdd_error);
        }
        if (started) {
            started = plan_workspace_for_layout(layout, vdd.display_indices(), plan, vdd_error);
        }
        if (started) {
            started = gt::apply_workspace_topology(
                pristine, plan.desktops, plan.center_driver, 1920, 1080, 120, internal_display,
                seed, vdd, workspace_topo, virtual_displays, vdd_error);
        }
        if (!started) {
            std::printf("virtual display startup failed: %s\n", vdd_error.c_str());
            if (vdd.connected()) {
                std::printf("if the laptop display looks wrong, use the controller's Recover "
                            "displays or press Win+P (Extend)\n");
            } else {
                std::printf("install the signed Parsec VDD, use Windows Extend mode, or pass "
                            "--no-virtual-displays for the renderer-only diagnostic\n");
            }
            g_status.state = gt::kStatusFailed;
            g_status.error = gt::engine_status_sanitize("virtual display startup failed: " + vdd_error);
            publish_status(g_status);
            vdd.disconnect();
            return 1;
        }
        topology_guard.vdd = &vdd;
        const int placed_windows = workspace_topo.windows_placed;
        const int repaired_takeover = workspace_topo.windows_repaired;
        topology_guard.arm(std::move(workspace_topo));
        g_topology_takeover = true;
        g_status.detached = !internal_display.empty();
        workspace_plan = plan;
        std::printf("created %zu virtual displays (Parsec VDD version %d)\n",
                    virtual_displays.size(), vdd.driver_version());
        for (const auto& display : virtual_displays) {
            std::printf("  VDD[%d] %dx%d@%dHz at (%d,%d)%s\n", display.driver_index,
                        display.width, display.height, display.refresh_hz, display.x, display.y,
                        display.driver_index == plan.center_driver ? " (primary)" : "");
        }
        if (!internal_display.empty()) {
            std::printf("  detached the laptop display; moved %d windows to the center desktop\n",
                        placed_windows);
            if (repaired_takeover > 0) {
                std::printf("  unminimized %d windows after the detach\n", repaired_takeover);
            }
        }
    }

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
            g_status.state = gt::kStatusFailed;
            g_status.error = "invalid monitor index";
            publish_status(g_status);
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
            if (!monitors[i].primary && !monitors[i].virtual_display) {
                selected = static_cast<int>(i);
                break;
            }
        }
    }
    if (selected < 0) {
        for (size_t i = 0; i < monitors.size(); ++i) {
            if (!monitors[i].virtual_display) {
                selected = static_cast<int>(i);
                break;
            }
        }
    }
    if (selected < 0 && !opt.monitor_explicit) {
        // The glasses link flaps under topology churn, and a recalled
        // "Disconnect this display" can land asynchronously after the
        // takeover; give it a bounded window to come back before failing
        // (the takeover already ran, so waiting costs nothing but time).
        // The recovery runs on every pass, not just once: a single shot can
        // fire while the adapter is still flagged attached.
        std::printf("waiting for the glasses display to reappear...\n");
        for (int wait_s = 0; wait_s < 15 && selected < 0; ++wait_s) {
            std::wstring detached;
            bool recovered = false;
            if (gt::find_detached_glasses_display(detached)) {
                std::printf("the glasses display is disconnected; re-attaching...\n");
                std::string recover_error;
                recovered = gt::reattach_detached_glasses(recover_error);
                if (!recovered) {
                    std::printf("glasses re-attach failed: %s\n", recover_error.c_str());
                }
            }
            if (!recovered) {
                // Deeper disconnect (EDID gone, CCD target persists):
                // silent unless it works; the failure dump covers the rest.
                std::string path_error;
                if (gt::reactivate_glasses_path(path_error)) {
                    std::printf("re-activated the glasses display path\n");
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
            monitors.clear();
            EnumDisplayMonitors(nullptr, nullptr, monitor_enum_proc,
                                reinterpret_cast<LPARAM>(&monitors));
            for (size_t i = 0; i < monitors.size(); ++i) {
                if (monitors[i].glasses) {
                    selected = static_cast<int>(i);
                    break;
                }
            }
            // No fallback inside the loop: keep every pass for the glasses
            // (a non-glasses appearance must not preempt a recovery that
            // lands a pass later).
        }
        if (selected < 0) {
            // One fallback pass after the wait: a non-glasses display that
            // appeared mid-wait is better than nothing.
            for (size_t i = 0; i < monitors.size(); ++i) {
                if (!monitors[i].virtual_display) {
                    selected = static_cast<int>(i);
                    break;
                }
            }
        }
        if (selected >= 0) {
            if (monitors[static_cast<size_t>(selected)].glasses) {
                std::printf("glasses display reappeared, continuing\n");
            } else {
                std::printf("glasses display not found; continuing on non-glasses "
                            "fallback [%d]\n",
                            selected);
            }
        } else {
            // Diagnostic, not noise: distinguishes a physical link flap
            // (nothing enumerates) from a deactivation the recovery missed.
            std::printf("display landscape at failure:\n%s",
                        gt::describe_display_landscape().c_str());
        }
    }
    if (selected < 0) {
        if (monitors.empty()) {
            std::printf("no monitors found\n");
            g_status.state = gt::kStatusFailed;
            g_status.error = "no monitors found";
            publish_status(g_status);
        } else {
            std::printf("the RayNeo glasses display was not found; check Extend mode (Win+P), "
                        "or pass --monitor N to choose one explicitly\n");
            g_status.state = gt::kStatusFailed;
            g_status.error = "the RayNeo glasses display was not found";
            publish_status(g_status);
        }
        return 1;
    }
    const MonitorEntry& target = monitors[selected];
    const int width = target.rect.right - target.rect.left;
    const int height = target.rect.bottom - target.rect.top;
    std::printf("rendering on [%d] %dx%d\n", selected, width, height);
    std::printf("controls: Ctrl+Alt+Q quit, Ctrl+Shift+R recenter head tracking\n");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = gt::kEngineWindowClass;
    if (RegisterClassExW(&wc) == 0) {
        std::printf("RegisterClassExW failed (%lu)\n", GetLastError());
        g_status.state = gt::kStatusFailed;
        g_status.error = "RegisterClassExW failed";
        publish_status(g_status);
        return 1;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, gt::kEngineWindowClass, gt::kEngineWindowTitle,
                                WS_POPUP, target.rect.left, target.rect.top, width, height,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        std::printf("CreateWindowExW failed (%lu)\n", GetLastError());
        g_status.state = gt::kStatusFailed;
        g_status.error = "CreateWindowExW failed";
        publish_status(g_status);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    std::printf("global hotkeys: Ctrl+Shift+R recenter, Ctrl+Alt+Y yaw tracking, "
                "Ctrl+Alt+P pitch tracking, Ctrl+Alt+S reading stabilisation, Ctrl+Alt+Q quit, "
                "Ctrl+Shift:\\ exit workspace\n");
    register_global_hotkeys(hwnd);

    gt::Renderer renderer;
    std::string error;
    if (!renderer.init(hwnd, static_cast<uint32_t>(width), static_cast<uint32_t>(height), error)) {
        std::printf("renderer init failed: %s\n", error.c_str());
        g_status.state = gt::kStatusFailed;
        g_status.error = gt::engine_status_sanitize("renderer init failed: " + error);
        publish_status(g_status);
        DestroyWindow(hwnd);
        return 1;
    }
    if (!renderer.set_layout(layout, error)) {
        std::printf("renderer layout failed: %s\n", error.c_str());
        g_status.state = gt::kStatusFailed;
        g_status.error = gt::engine_status_sanitize("renderer layout failed: " + error);
        publish_status(g_status);
        renderer.shutdown();
        DestroyWindow(hwnd);
        return 1;
    }
    std::printf("loaded layout: %s (%zu screens)\n", opt.layout_path.c_str(), layout.screens.size());
    std::printf("capture policy: active %dfps (|yaw delta| <= %.0f deg), mid %dfps, "
                "idle %dfps (|yaw delta| >= %.0f deg)\n",
                layout.capture_policy.active_fps, layout.capture_policy.leave_deg,
                layout.capture_policy.mid_fps, layout.capture_policy.idle_fps,
                layout.capture_policy.enter_deg);
    g_status.state = gt::kStatusReady;
    g_status.screens = static_cast<int>(layout.screens.size());
    g_status.monitor = selected;
    g_status.monitor_width = width;
    g_status.monitor_height = height;
    g_status.error.clear();
    publish_status(g_status);

    std::vector<std::unique_ptr<gt::DesktopDuplicator>> captures;
    if (opt.virtual_displays &&
        !bind_desktop_captures(renderer, layout, virtual_displays, captures, error)) {
        std::printf("desktop capture startup failed: %s\n", error.c_str());
        g_status.state = gt::kStatusFailed;
        g_status.error = gt::engine_status_sanitize("desktop capture startup failed: " + error);
        publish_status(g_status);
        renderer.shutdown();
        DestroyWindow(hwnd);
        return 1;
    }
    std::vector<CaptureTierState> capture_states(captures.size());

    gt::ImuSource imu;
    AppState state;
    state.imu = opt.no_imu ? nullptr : &imu;
    g_app = &state;
    state.screen_count = static_cast<int>(layout.screens.size());
    state.virtual_displays = opt.virtual_displays;
    state.stabilise_level = opt.stabilise;
    gt::apply_reading_hold(state.stabilise_level, state.view_smoother_config);
    state.view_smoother.configure(state.view_smoother_config);
    if (opt.smoothing) {
        std::printf("reading stabilisation: %s (Ctrl+Alt+S cycles off/low/medium/high)\n",
                    gt::reading_hold_name(state.stabilise_level));
    }
    if (!opt.no_imu) {
        imu.set_sensor_to_head(sensor_to_head);
        std::printf("loaded orientation calibration: %s\n", opt.calibration_path.c_str());
        if (opt.mag && mag_calibration.valid) {
            imu.set_mag_calibration(mag_calibration);
            std::printf("magnetometer heading lock: on (hard iron %.2f, %.2f, %.2f uT)\n",
                        mag_calibration.hard_iron_ut.x, mag_calibration.hard_iron_ut.y,
                        mag_calibration.hard_iron_ut.z);
        } else {
            std::printf("magnetometer heading lock: off (%s) - yaw is gyro-only and can drift\n",
                        opt.mag ? "no magnetometer calibration; run orientation_calibrate --mag" : "--no-mag");
        }
        if (!opt.log_path.empty()) {
            const std::string raw_path =
                gt::utf8_from_path(gt::path_from_utf8(opt.log_path).parent_path() / "imu_raw.csv");
            imu.set_raw_log_path(raw_path);
            std::printf("raw IMU log: %s\n", raw_path.c_str());
        }
        imu.start();
    }

    const auto start = SteadyClock::now();
    uint64_t frames = 0;
    uint64_t frames_at_stat = 0;
    double next_stat = 1.0;
    const gt::CameraSigns signs;

    std::ofstream diagnostics;
    if (!opt.log_path.empty()) {
        const std::filesystem::path log_file = gt::path_from_utf8(opt.log_path);
        const bool write_header = !std::filesystem::exists(log_file);
        diagnostics.open(log_file, std::ios::out | std::ios::app);
        if (diagnostics.is_open() && write_header) {
            diagnostics << "elapsed_s,tick_100us,gx_raw,gy_raw,gz_raw,bias_x,bias_y,bias_z,"
                           "view_yaw_deg,view_pitch_deg,view_roll_deg,still,"
                           "rest,adapt_state,corrected_rate_degs,stillness_degs,"
                           "accel_dev_mps2,escape_rollbacks\n";
        }
    }
    int log_rows = 0;
    std::error_code layout_time_error;
    auto layout_write_time =
        std::filesystem::last_write_time(gt::path_from_utf8(opt.layout_path), layout_time_error);
    double next_layout_check = 0.5;
    double next_capture_error_log = 0.0;
    int consecutive_capture_failures = 0;
    // Mid-run fatalities must surface as a failed exit, not a clean stop:
    // the controller maps exit 0 to "exited normally" without consulting the
    // status file.
    bool engine_failed = false;
    std::string fatal_detail;

    double prev_elapsed = -1.0;
    // Late-pose telemetry (1 Hz line): how old the frame-start pose would
    // have been at render time, and how far the view moved in that interval.
    double late_gap_ms_sum = 0.0;
    double late_diff_deg_sum = 0.0;
    int late_samples = 0;
    while (!state.quit) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        // Deliberately no GetAsyncKeyState(VK_ESCAPE) poll: it is
        // process-global and quit the workspace while escaping other apps.
        const double elapsed = std::chrono::duration<double>(SteadyClock::now() - start).count();
        if (opt.seconds > 0.0 && elapsed >= opt.seconds) {
            state.quit = true;
        }

        const bool force_layout_reload = state.reload_layout;
        state.reload_layout = false;
        if (elapsed >= next_layout_check || force_layout_reload) {
            next_layout_check = elapsed + 0.5;
            std::error_code time_error;
            const auto write_time =
                std::filesystem::last_write_time(gt::path_from_utf8(opt.layout_path), time_error);
            const bool mtime_changed =
                !time_error && (layout_time_error || write_time != layout_write_time);
            if (mtime_changed || force_layout_reload) {
                gt::Layout candidate;
                std::string reload_error;
                const size_t previous_display_count = layout.screens.size();
                bool displays_changed = false;
                bool plan_changed = false;
                bool renderer_changed = false;
                const WorkspacePlan previous_plan = workspace_plan;
                WorkspacePlan reload_plan;
                std::vector<std::unique_ptr<gt::DesktopDuplicator>> candidate_captures;
                bool reload_ok = gt::load_layout(opt.layout_path, candidate, reload_error);
                if (reload_ok && opt.virtual_displays) {
                    displays_changed = candidate.screens.size() != previous_display_count;
                    if (displays_changed) {
                        reload_ok = vdd.resize(candidate.screens.size(), reload_error);
                    }
                    if (reload_ok &&
                        plan_workspace_for_layout(candidate, vdd.display_indices(), reload_plan,
                                                  reload_error)) {
                        if (reload_plan.desktops != workspace_plan.desktops ||
                            reload_plan.center_driver != workspace_plan.center_driver) {
                            reload_ok = gt::reposition_virtual_displays(
                                reload_plan.desktops, vdd_device_names(), reload_plan.center_driver,
                                1920, 1080, 120, virtual_displays, reload_error);
                            plan_changed = reload_ok;
                        }
                    } else {
                        reload_ok = false;
                    }
                }
                if (reload_ok) {
                    reload_ok = renderer.set_layout(candidate, reload_error);
                    renderer_changed = reload_ok;
                }
                if (reload_ok && opt.virtual_displays) {
                    reload_ok = bind_desktop_captures(renderer, candidate, virtual_displays,
                                                      candidate_captures, reload_error);
                }
                if (reload_ok) {
                    // Only a successful load advances the watermark: a torn
                    // read (possible from non-atomic hand edits) is retried
                    // on the next check instead of being forgotten.
                    if (!time_error) {
                        layout_write_time = write_time;
                        layout_time_error.clear();
                    }
                    layout = std::move(candidate);
                    if (opt.virtual_displays) {
                        captures = std::move(candidate_captures);
                        capture_states.assign(captures.size(), CaptureTierState{});
                        workspace_plan = reload_plan;
                    }
                    if (!opt.fov_explicit) {
                        opt.fov = layout.fov_deg;
                    }
                    std::printf("reloaded layout (%zu screens)\n", layout.screens.size());
                    state.screen_count = static_cast<int>(layout.screens.size());
                    g_status.screens = state.screen_count;
                    publish_status(g_status);
                } else {
                    if (renderer_changed) {
                        std::string renderer_rollback_error;
                        if (!renderer.set_layout(layout, renderer_rollback_error)) {
                            std::printf("renderer layout rollback failed: %s\n",
                                        renderer_rollback_error.c_str());
                            engine_failed = true;
                            fatal_detail =
                                "renderer layout rollback failed: " + renderer_rollback_error;
                            state.quit = true;
                        }
                    }
                    if (opt.virtual_displays && (displays_changed || plan_changed)) {
                        std::string rollback_error;
                        if (!vdd.resize(previous_display_count, rollback_error) ||
                            !gt::reposition_virtual_displays(
                                previous_plan.desktops, vdd_device_names(),
                                previous_plan.center_driver, 1920, 1080, 120, virtual_displays,
                                rollback_error)) {
                            std::printf("virtual display rollback failed: %s\n",
                                        rollback_error.c_str());
                            engine_failed = true;
                            fatal_detail = "virtual display rollback failed: " + rollback_error;
                            state.quit = true;
                        } else if (!bind_desktop_captures(renderer, layout, virtual_displays,
                                                          captures, rollback_error)) {
                            std::printf("desktop capture rollback failed: %s\n",
                                        rollback_error.c_str());
                            engine_failed = true;
                            fatal_detail = "desktop capture rollback failed: " + rollback_error;
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
            engine_failed = true;
            fatal_detail = "Parsec VDD keepalive failed repeatedly";
            state.quit = true;
        }

        // Yaw/pitch holds applied to a raw pose. Called twice per frame: once
        // here for capture tiering and again after wait_for_frame() with a
        // fresh pose for rendering (see below).
        const auto apply_view_holds = [&state](gt::Quat pose) {
            if (!state.yaw_tracking) {
                const gt::Quat twist = gt::quat_twist_about(pose, 0.0f, 0.0f, 1.0f);
                const gt::Quat swing = gt::quat_multiply(gt::quat_conjugate(twist), pose);
                pose = gt::quat_multiply(state.held_yaw_twist, swing);
            }
            if (!state.pitch_tracking) {
                const gt::Quat twist = gt::quat_twist_about(pose, 1.0f, 0.0f, 0.0f);
                const gt::Quat swing = gt::quat_multiply(gt::quat_conjugate(twist), pose);
                pose = gt::quat_multiply(state.held_pitch_twist, swing);
            }
            return pose;
        };
        gt::Quat head = opt.no_imu ? gt::Quat{} : imu.orientation();
        const SteadyClock::time_point early_pose_time = SteadyClock::now();
        if (state.capture_yaw_hold) {
            state.held_yaw_twist = gt::quat_twist_about(head, 0.0f, 0.0f, 1.0f);
            state.capture_yaw_hold = false;
        }
        if (state.capture_pitch_hold) {
            // Nod (pitch) is about head X (right); head Y (forward) is the tilt
            // axis. Twisting about Y here used to freeze tilt instead of pitch.
            state.held_pitch_twist = gt::quat_twist_about(head, 1.0f, 0.0f, 0.0f);
            state.capture_pitch_hold = false;
        }
        head = apply_view_holds(head);

        const float view_yaw_deg = gt::camera_applied_euler(head, signs).yaw_deg;
        const gt::CapturePolicy& capture_policy = layout.capture_policy;
        const double tier_intervals[3] = {
            1.0 / static_cast<double>(capture_policy.active_fps),
            1.0 / static_cast<double>(capture_policy.mid_fps),
            1.0 / static_cast<double>(capture_policy.idle_fps),
        };

        bool frame_capture_failed = false;
        for (size_t screen_index = 0; screen_index < captures.size(); ++screen_index) {
            CaptureTierState& tier_state = capture_states[screen_index];
            const float yaw_delta = std::fabs(
                wrap_deg(view_yaw_deg - layout.screens[screen_index].yaw_deg));
            // Two-threshold hysteresis: inside the leave cone a screen is active,
            // beyond the enter cone it is idle; the band between keeps the
            // previous tier so a screen near a boundary does not flip tier (and
            // re-poll every frame) when the head yaw jitters.
            if (yaw_delta <= capture_policy.leave_deg) {
                tier_state.tier = 0;
            } else if (yaw_delta >= capture_policy.enter_deg) {
                tier_state.tier = 2;
            }
            if (elapsed < tier_state.next_poll_time) {
                continue;
            }
            tier_state.next_poll_time = elapsed + tier_intervals[tier_state.tier];

            gt::CapturedDesktop captured;
            std::string capture_error;
            const gt::CapturePollResult result = captures[screen_index]->poll(captured, capture_error);
            if (result == gt::CapturePollResult::Failed) {
                if (elapsed >= next_capture_error_log) {
                    std::printf("desktop capture failed for screen '%s': %s\n",
                                layout.screens[screen_index].id.c_str(), capture_error.c_str());
                    next_capture_error_log = elapsed + 1.0;
                }
                continue;
            }
            if (result != gt::CapturePollResult::Frame) {
                continue;
            }
            if (captured.protected_content_masked) {
                // The driver masks protected content to black; blank the screen
                // (drop the live texture) instead of painting that black frame.
                if (!renderer.set_screen_texture(screen_index, nullptr, capture_error)) {
                    if (elapsed >= next_capture_error_log) {
                        std::printf("desktop texture update failed for screen '%s': %s\n",
                                    layout.screens[screen_index].id.c_str(), capture_error.c_str());
                        next_capture_error_log = elapsed + 1.0;
                    }
                    frame_capture_failed = true;
                    if (++consecutive_capture_failures >= kFatalCaptureFailures) {
                        std::printf("desktop capture kept failing; exiting\n");
                        engine_failed = true;
                        fatal_detail = "desktop capture kept failing";
                        state.quit = true;
                    }
                }
                continue;
            }
            if (captured.desktop_updated &&
                !renderer.set_screen_texture(screen_index, captured.texture.Get(), capture_error)) {
                if (elapsed >= next_capture_error_log) {
                    std::printf("desktop texture update failed for screen '%s': %s\n",
                                layout.screens[screen_index].id.c_str(), capture_error.c_str());
                    next_capture_error_log = elapsed + 1.0;
                }
                frame_capture_failed = true;
                if (++consecutive_capture_failures >= kFatalCaptureFailures) {
                    std::printf("desktop capture kept failing; exiting\n");
                    engine_failed = true;
                    fatal_detail = "desktop capture kept failing";
                    state.quit = true;
                }
                continue;
            }
            gt::CursorUpdate cursor;
            cursor.position_updated = captured.pointer.position_updated;
            cursor.visible = captured.pointer.visible;
            cursor.x = captured.pointer.x;
            cursor.y = captured.pointer.y;
            cursor.desktop_width = captured.width;
            cursor.desktop_height = captured.height;
            cursor.shape_updated = captured.pointer.shape_updated;
            cursor.shape_width = captured.pointer.shape.Width;
            cursor.shape_height = captured.pointer.shape.Height;
            cursor.shape_pitch = captured.pointer.shape.Pitch;
            cursor.shape_pixels = captured.pointer.pixels.data();
            cursor.shape_bytes = captured.pointer.pixels.size();
            switch (captured.pointer.shape.Type) {
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
                    cursor.mode = gt::CursorShapeMode::Color;
                    break;
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
                    cursor.mode = gt::CursorShapeMode::MaskedColor;
                    break;
                case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
                    cursor.mode = gt::CursorShapeMode::Monochrome;
                    break;
                default:
                    cursor.shape_updated = false;
                    break;
            }
            if ((cursor.position_updated || cursor.shape_updated) &&
                !renderer.update_screen_cursor(screen_index, cursor, capture_error)) {
                if (elapsed >= next_capture_error_log) {
                    std::printf("desktop cursor update failed for screen '%s': %s\n",
                                layout.screens[screen_index].id.c_str(), capture_error.c_str());
                    next_capture_error_log = elapsed + 1.0;
                }
                frame_capture_failed = true;
                if (++consecutive_capture_failures >= kFatalCaptureFailures) {
                    std::printf("desktop cursor update kept failing; exiting\n");
                    engine_failed = true;
                    fatal_detail = "desktop cursor update kept failing";
                    state.quit = true;
                }
            }
        }
        if (!frame_capture_failed) {
            consecutive_capture_failures = 0;
        }

        renderer.set_signs(signs);
        renderer.wait_for_frame();
        // Late pose sample: the pose read above is older by the whole capture
        // pass plus the swapchain wait (up to a frame). World-locked text
        // swims by head speed x that age, so render from the newest pose.
        if (!opt.no_imu) {
            const gt::Quat late = apply_view_holds(imu.orientation());
            late_gap_ms_sum +=
                std::chrono::duration<double, std::milli>(SteadyClock::now() - early_pose_time).count();
            late_diff_deg_sum += gt::quat_angle_deg(gt::quat_multiply(gt::quat_conjugate(head), late));
            ++late_samples;
            head = late;
        }
        if (!opt.no_imu && opt.smoothing) {
            const float frame_dt =
                (prev_elapsed < 0.0) ? (1.0f / 60.0f) : static_cast<float>(elapsed - prev_elapsed);
            head = state.view_smoother.update(head, frame_dt);
        }
        prev_elapsed = elapsed;
        renderer.render(head, opt.fov, static_cast<float>(elapsed));
        if (!renderer.present()) {
            std::printf("present failed, exiting\n");
            engine_failed = true;
            fatal_detail = "present failed";
            g_status.state = gt::kStatusFailed;
            g_status.error = "present failed";
            publish_status(g_status);
            break;
        }
        ++frames;

        if (diagnostics.is_open()) {
            const gt::Euler le = gt::camera_applied_euler(head, signs);
            const gt::Vec3 g = imu.last_gyro_degs();
            const gt::Vec3 b = imu.gyro_bias_degs();
            char row[256];
            std::snprintf(row, sizeof(row),
                          "%.3f,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%.3f,%.3f,%.3f,%u\n",
                          elapsed, imu.last_tick(), g.x, g.y, g.z, b.x, b.y, b.z, le.yaw_deg,
                          le.pitch_deg, le.roll_deg, imu.still() ? 1 : 0, imu.rest() ? 1 : 0,
                          imu.adapt_state(), imu.corrected_rate_degs(), imu.stillness_degs(),
                          imu.accel_dev_mps2(),
                          static_cast<unsigned>(imu.escape_rollbacks()));
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
                    "[%.0fs] fps=%.1f  imu=%.1fHz (%s)  bias=(%+.2f,%+.2f,%+.2f) drift=%.2f  "
                    "rest=%d adapt=%d corr=%.2f dev=(%.2f,%.2f) rollbacks=%u  "
                    "yaw=%7.2f pitch=%6.2f roll=%6.2f\n",
                    elapsed, fps, imu.sample_rate_hz(), imu.status().c_str(), bias.x, bias.y, bias.z,
                    imu.drift_correction_degs(), imu.rest() ? 1 : 0, imu.adapt_state(),
                    imu.corrected_rate_degs(), imu.stillness_degs(), imu.accel_dev_mps2(),
                    static_cast<unsigned>(imu.escape_rollbacks()),
                    e.yaw_deg, e.pitch_deg, e.roll_deg);
                if (imu.mag_lock_active()) {
                    static const char* const kMagStates[] = {"off", "acquiring", "locked", "disturbed"};
                    const int ms = imu.mag_state();
                    std::printf("  mag_heading: state=%s err=%+.2fdeg int=%+.3fdeg/s |B|=%.1f/%.1fuT "
                                "dip=%.1f/%.1fdeg corr_total=%+.2fdeg reacq=%u temp=%.1fC\n",
                                (ms >= 0 && ms <= 3) ? kMagStates[ms] : "?", imu.mag_error_deg(),
                                imu.mag_integral_degs(), imu.mag_field_ut(), imu.mag_reference_field_ut(),
                                imu.mag_dip_deg(), imu.mag_reference_dip_deg(), imu.mag_total_correction_deg(),
                                static_cast<unsigned>(imu.mag_reacquisitions()), imu.temperature_c());
                }
                if (late_samples > 0) {
                    std::printf("  view: stabilise=%s late-pose gain %.1f ms, %.3f deg/frame\n",
                                opt.smoothing ? gt::reading_hold_name(state.stabilise_level) : "raw",
                                late_gap_ms_sum / late_samples, late_diff_deg_sum / late_samples);
                    late_gap_ms_sum = 0.0;
                    late_diff_deg_sum = 0.0;
                    late_samples = 0;
                }
            }
            frames_at_stat = frames;
            g_status.fps = fps;
            g_status.elapsed_s = elapsed;
            g_status.screens = state.screen_count;
            publish_status(g_status);
            next_stat += 1.0;
        }
    }

    std::printf("shutting down\n");
    captures.clear();
    if (topology_guard.armed) {
        std::printf("restoring the laptop display (removing virtual desktops)...\n");
        std::string topo_error;
        if (!gt::restore_display_topology(topology_guard.topo, vdd, topo_error)) {
            std::printf("display topology restore failed: %s\n", topo_error.c_str());
            engine_failed = true;
            fatal_detail = "display topology restore failed: " + topo_error;
        } else {
            g_topology_takeover = false;
            g_status.detached = false;
            if (topology_guard.topo.windows_restored > 0) {
                std::printf("  moved %d windows back to the laptop display\n",
                            topology_guard.topo.windows_restored);
            }
            if (topology_guard.topo.windows_repaired > 0) {
                std::printf("  unminimized %d windows after the restore\n",
                            topology_guard.topo.windows_repaired);
            }
            if (topology_guard.topo.taskbar_state >= 0) {
                std::printf("  taskbar auto-hide: captured=%d before-restore=%d after=%d%s\n",
                            topology_guard.topo.taskbar_state,
                            topology_guard.topo.taskbar_before,
                            topology_guard.topo.taskbar_after,
                            topology_guard.topo.taskbar_reapplied ? " (re-applied)"
                                                                  : " (already correct)");
            }
        }
        topology_guard.disarm();
    }
    if (engine_failed) {
        g_status.state = gt::kStatusFailed;
        g_status.error = gt::engine_status_sanitize(fatal_detail);
    } else if (g_status.state != gt::kStatusFailed) {
        g_status.state = gt::kStatusStopped;
    }
    g_status.elapsed_s = 0.0;
    publish_status(g_status);
    UnregisterHotKey(hwnd, kHotkeyRecenter);
    UnregisterHotKey(hwnd, kHotkeyToggleYaw);
    UnregisterHotKey(hwnd, kHotkeyTogglePitch);
    UnregisterHotKey(hwnd, kHotkeyCycleStabilise);
    UnregisterHotKey(hwnd, kHotkeyQuit);
    UnregisterHotKey(hwnd, kHotkeyExitWorkspace);
    if (diagnostics.is_open()) {
        diagnostics.close();
    }
    if (!opt.no_imu) {
        imu.stop();
    }
    g_app = nullptr;
    renderer.shutdown();
    DestroyWindow(hwnd);
    return engine_failed ? 1 : 0;
}
