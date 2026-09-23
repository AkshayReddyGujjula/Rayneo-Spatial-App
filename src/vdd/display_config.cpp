#include "vdd/display_config.h"
#include "vdd/vdd_client.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <map>
#include <set>
#include <thread>
#include <tuple>

namespace gt {
namespace {

bool contains_case_insensitive(const std::wstring& text, const wchar_t* needle) {
    std::wstring lower_text = text;
    std::wstring lower_needle = needle;
    std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), std::towlower);
    std::transform(lower_needle.begin(), lower_needle.end(), lower_needle.begin(), std::towlower);
    return lower_text.find(lower_needle) != std::wstring::npos;
}

std::vector<int> refresh_rates(const std::wstring& device_name, int width, int height) {
    std::set<int> unique;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsExW(device_name.c_str(), index, &mode, 0); ++index) {
        if (static_cast<int>(mode.dmPelsWidth) == width &&
            static_cast<int>(mode.dmPelsHeight) == height && mode.dmDisplayFrequency > 1) {
            unique.insert(static_cast<int>(mode.dmDisplayFrequency));
        }
        mode = DEVMODEW{};
        mode.dmSize = sizeof(mode);
    }
    return std::vector<int>(unique.begin(), unique.end());
}

bool adapter_is_primary(const std::wstring& device_name) {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index) {
        if (device_name == device.DeviceName) {
            return (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        }
        device = DISPLAY_DEVICEW{};
        device.cb = sizeof(device);
    }
    return false;
}

// Queries display paths with the sizing/query race retried: a display
// arriving mid-call surfaces as ERROR_INSUFFICIENT_BUFFER and simply
// re-sizes. Vectors are trimmed to the returned counts so they can be
// submitted straight back to SetDisplayConfig.
bool query_display_config(DWORD flags, std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                          std::vector<DISPLAYCONFIG_MODE_INFO>& modes, std::string& error) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    LONG query_result = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 4; ++attempt) {
        UINT32 sized_paths = 0;
        UINT32 sized_modes = 0;
        const LONG size_result = GetDisplayConfigBufferSizes(flags, &sized_paths, &sized_modes);
        if (size_result != ERROR_SUCCESS) {
            error =
                "could not query the display topology (code " + std::to_string(size_result) + ")";
            return false;
        }
        if (sized_paths == 0 || sized_modes == 0) {
            error = "no active display paths found";
            return false;
        }
        paths.assign(sized_paths, DISPLAYCONFIG_PATH_INFO{});
        modes.assign(sized_modes, DISPLAYCONFIG_MODE_INFO{});
        path_count = sized_paths;
        mode_count = sized_modes;
        query_result = QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count,
                                          modes.data(), nullptr);
        if (query_result != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
    }
    if (query_result != ERROR_SUCCESS) {
        error = "could not query the display topology (code " + std::to_string(query_result) + ")";
        return false;
    }
    paths.resize(path_count);
    modes.resize(mode_count);
    return true;
}

}  // namespace

int parse_vdd_driver_index(const std::wstring& device_id) {
    std::wstring upper = device_id;
    std::transform(upper.begin(), upper.end(), upper.begin(), std::towupper);
    const size_t marker = upper.find(L"UID");
    if (marker == std::wstring::npos) {
        return -1;
    }
    size_t cursor = marker + 3;
    int uid = 0;
    bool have_digit = false;
    while (cursor < upper.size() && upper[cursor] >= L'0' && upper[cursor] <= L'9') {
        have_digit = true;
        uid = uid * 10 + (upper[cursor] - L'0');
        ++cursor;
    }
    if (!have_digit || uid < 0x100 || uid >= 0x110) {
        return -1;
    }
    return uid - 0x100;
}

int choose_refresh_rate(const std::vector<int>& available, int preferred) {
    if (available.empty()) {
        return 0;
    }
    if (std::find(available.begin(), available.end(), preferred) != available.end()) {
        return preferred;
    }
    int best_below = 0;
    int lowest_above = 0;
    for (int rate : available) {
        if (rate <= preferred) {
            best_below = std::max(best_below, rate);
        } else if (lowest_above == 0 || rate < lowest_above) {
            lowest_above = rate;
        }
    }
    return best_below != 0 ? best_below : lowest_above;
}

std::vector<VirtualDisplayInfo> enumerate_virtual_displays() {
    std::map<int, VirtualDisplayInfo> by_index;
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    for (DWORD adapter_index = 0; EnumDisplayDevicesW(nullptr, adapter_index, &adapter, 0);
         ++adapter_index) {
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        for (DWORD monitor_index = 0;
             EnumDisplayDevicesW(adapter.DeviceName, monitor_index, &monitor,
                                 EDD_GET_DEVICE_INTERFACE_NAME);
             ++monitor_index) {
            const std::wstring id = monitor.DeviceID;
            if (contains_case_insensitive(id, L"PSCCDD0")) {
                const int driver_index = parse_vdd_driver_index(id);
                if (driver_index >= 0) {
                    VirtualDisplayInfo info;
                    info.driver_index = driver_index;
                    info.device_name = adapter.DeviceName;
                    info.device_id = id;
                    info.active = (adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0 &&
                                  (monitor.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0;
                    // Disconnects leave registry ghosts behind that carry the
                    // same UID as a freshly-connected display. The live entry
                    // always wins: a ghost name has no mode list and can never
                    // be configured.
                    const auto existing = by_index.find(driver_index);
                    if (existing == by_index.end() ||
                        (!existing->second.active && info.active)) {
                        by_index[driver_index] = std::move(info);
                    }
                }
            }
            monitor = DISPLAY_DEVICEW{};
            monitor.cb = sizeof(monitor);
        }
        adapter = DISPLAY_DEVICEW{};
        adapter.cb = sizeof(adapter);
    }
    std::vector<VirtualDisplayInfo> result;
    for (auto& entry : by_index) {
        result.push_back(std::move(entry.second));
    }
    return result;
}

bool wait_for_virtual_displays(const std::vector<int>& driver_indices, int timeout_ms,
                               std::vector<VirtualDisplayInfo>& displays, std::string& error) {
    const std::set<int> expected(driver_indices.begin(), driver_indices.end());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        displays = enumerate_virtual_displays();
        std::set<int> found;
        for (const VirtualDisplayInfo& display : displays) {
            if (expected.contains(display.driver_index) && display.active) {
                found.insert(display.driver_index);
            }
        }
        if (found == expected) {
            std::erase_if(displays, [&](const VirtualDisplayInfo& display) {
                return !expected.contains(display.driver_index);
            });
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    error = "timed out waiting for the Parsec virtual displays to enumerate";
    return false;
}

bool wait_for_no_virtual_displays(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        bool any_active = false;
        for (const VirtualDisplayInfo& display : enumerate_virtual_displays()) {
            if (display.active) {
                any_active = true;
                break;
            }
        }
        if (!any_active) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

int select_center_driver(const std::vector<std::pair<int, float>>& driver_yaw) {
    int best = -1;
    float best_abs = 0.0f;
    for (const auto& [driver, yaw] : driver_yaw) {
        const float magnitude = std::fabs(yaw);
        if (best < 0 || magnitude < best_abs || (magnitude == best_abs && driver < best)) {
            best = driver;
            best_abs = magnitude;
        }
    }
    return best;
}

std::vector<PlannedDesktop> plan_workspace_desktops(
    const std::vector<std::pair<int, float>>& driver_yaw, int center_driver, int width) {
    std::vector<std::pair<float, int>> left;  // (yaw, driver), ascending yaw
    std::vector<std::pair<float, int>> right;
    for (const auto& [driver, yaw] : driver_yaw) {
        if (driver == center_driver) {
            continue;
        }
        if (yaw < 0.0f) {
            left.emplace_back(yaw, driver);
        } else {
            right.emplace_back(yaw, driver);
        }
    }
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    std::map<int, int> position;
    position[center_driver] = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        position[left[i].second] = -width * static_cast<int>(left.size() - i);
    }
    for (size_t i = 0; i < right.size(); ++i) {
        position[right[i].second] = width * static_cast<int>(i + 1);
    }
    std::vector<PlannedDesktop> planned;
    planned.reserve(driver_yaw.size());
    for (const auto& [driver, yaw] : driver_yaw) {
        (void)yaw;
        PlannedDesktop desktop;
        desktop.driver_index = driver;
        const auto found = position.find(driver);
        desktop.x = found == position.end() ? 0 : found->second;
        desktop.y = 0;
        planned.push_back(desktop);
    }
    return planned;
}

std::wstring select_internal_display(
    const std::vector<std::tuple<std::wstring, bool, bool>>& displays) {
    std::wstring first_internal;
    for (const auto& [name, internal, primary] : displays) {
        if (!internal) {
            continue;
        }
        if (primary) {
            return name;
        }
        if (first_internal.empty()) {
            first_internal = name;
        }
    }
    return first_internal;
}

bool display_rects_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

std::vector<PlannedAside> plan_aside_moves(
    const std::vector<DisplayModeSnapshot>& snapshots,
    const std::vector<PlannedDesktop>& slots, int slot_width, int slot_height,
    const std::set<std::wstring>& vdd_devices) {
    int base_x = 0;
    for (const DisplayModeSnapshot& snapshot : snapshots) {
        base_x = std::max(base_x, static_cast<int>(snapshot.mode.dmPosition.x) +
                                        static_cast<int>(snapshot.mode.dmPelsWidth));
    }
    for (const PlannedDesktop& slot : slots) {
        base_x = std::max(base_x, slot.x + slot_width);
    }
    const auto needs_aside = [&](const DisplayModeSnapshot& snapshot) {
        if (vdd_devices.contains(snapshot.device_name)) {
            return false;
        }
        if (snapshot.primary) {
            return true;
        }
        const int x = static_cast<int>(snapshot.mode.dmPosition.x);
        const int y = static_cast<int>(snapshot.mode.dmPosition.y);
        const int w = static_cast<int>(snapshot.mode.dmPelsWidth);
        const int h = static_cast<int>(snapshot.mode.dmPelsHeight);
        for (const PlannedDesktop& slot : slots) {
            if (display_rects_intersect(x, y, w, h, slot.x, slot.y, slot_width, slot_height)) {
                return true;
            }
        }
        return false;
    };
    std::vector<PlannedAside> moves;
    int cursor = base_x;
    for (bool want_primary : {true, false}) {
        for (const DisplayModeSnapshot& snapshot : snapshots) {
            if (snapshot.primary != want_primary || !needs_aside(snapshot)) {
                continue;
            }
            PlannedAside move;
            move.device_name = snapshot.device_name;
            move.x = cursor;
            move.y = 0;
            cursor += static_cast<int>(snapshot.mode.dmPelsWidth);
            moves.push_back(std::move(move));
        }
    }
    return moves;
}

RECT migrate_window_rect(const RECT& window, const RECT& from, const RECT& to) {
    const int width = static_cast<int>(window.right - window.left);
    const int height = static_cast<int>(window.bottom - window.top);
    int x = static_cast<int>(to.left + (window.left - from.left));
    int y = static_cast<int>(to.top + (window.top - from.top));
    const int bound_w = static_cast<int>(to.right - to.left);
    const int bound_h = static_cast<int>(to.bottom - to.top);
    if (width >= bound_w) {
        x = static_cast<int>(to.left);
    } else {
        x = (std::max)(static_cast<int>(to.left), (std::min)(x, static_cast<int>(to.right - width)));
    }
    if (height >= bound_h) {
        y = static_cast<int>(to.top);
    } else {
        y = (std::max)(static_cast<int>(to.top),
                       (std::min)(y, static_cast<int>(to.bottom - height)));
    }
    RECT out{};
    out.left = x;
    out.top = y;
    out.right = x + width;
    out.bottom = y + height;
    return out;
}

namespace {

struct WindowSnapshotContext {
    RECT laptop{};
    std::vector<MigratedWindow>* out = nullptr;
};

// Top-level, non-shell, non-transient: the windows the migration owns. The
// visibility/minimized state is checked by each caller (the repair passes
// deliberately handle minimized windows).
bool window_is_managed(HWND hwnd) {
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return false;
    }
    wchar_t klass[64]{};
    if (GetClassNameW(hwnd, klass, static_cast<int>(std::size(klass))) > 0) {
        if (wcscmp(klass, L"Shell_TrayWnd") == 0 || wcscmp(klass, L"Shell_SecondaryTrayWnd") == 0 ||
            wcscmp(klass, L"Progman") == 0 || wcscmp(klass, L"WorkerW") == 0) {
            return false;
        }
    }
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return (style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) == 0;
}

BOOL CALLBACK snapshot_window_proc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WindowSnapshotContext*>(lParam);
    if (IsWindowVisible(hwnd) == FALSE || IsIconic(hwnd) != FALSE) {
        return TRUE;
    }
    if (!window_is_managed(hwnd)) {
        return TRUE;
    }
    RECT rect{};
    if (GetWindowRect(hwnd, &rect) == FALSE) {
        return TRUE;
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return TRUE;
    }
    if (!display_rects_intersect(
            static_cast<int>(rect.left), static_cast<int>(rect.top),
            static_cast<int>(rect.right - rect.left), static_cast<int>(rect.bottom - rect.top),
            static_cast<int>(ctx->laptop.left), static_cast<int>(ctx->laptop.top),
            static_cast<int>(ctx->laptop.right - ctx->laptop.left),
            static_cast<int>(ctx->laptop.bottom - ctx->laptop.top))) {
        return TRUE;
    }
    MigratedWindow entry;
    entry.hwnd = hwnd;
    entry.home = rect;
    entry.zoomed = IsZoomed(hwnd) != FALSE;
    ctx->out->push_back(entry);
    return TRUE;
}

// Bounded unminimize+unmaximize: ShowWindow restores one step per call (a
// minimized-maximized window lands maximized first), and a hung window never
// settles, so cap the iterations and report honestly.
bool restore_window_state(HWND hwnd) {
    for (int i = 0; i < 3 && (IsIconic(hwnd) != FALSE || IsZoomed(hwnd) != FALSE); ++i) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    return IsIconic(hwnd) == FALSE && IsZoomed(hwnd) == FALSE;
}

bool move_recorded_window(HWND hwnd, const RECT& target, bool was_zoomed) {
    if (IsWindow(hwnd) == FALSE) {
        return false;
    }
    // No minimized skip: SetWindowPos on an iconic window returns success
    // while ignoring the position, which would count a window that never
    // moved. Settle it to normal first so the move below is effective.
    if (!restore_window_state(hwnd)) {
        return false;
    }
    const int width = static_cast<int>(target.right - target.left);
    const int height = static_cast<int>(target.bottom - target.top);
    if (SetWindowPos(hwnd, nullptr, static_cast<int>(target.left), static_cast<int>(target.top),
                     width, height, SWP_NOZORDER | SWP_NOACTIVATE) == FALSE) {
        return false;
    }
    if (was_zoomed) {
        ShowWindow(hwnd, SW_MAXIMIZE);
    }
    return true;
}

struct IconicSnapshotContext {
    std::set<HWND>* out = nullptr;
};

BOOL CALLBACK iconic_snapshot_proc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<IconicSnapshotContext*>(lParam);
    if (IsIconic(hwnd) != FALSE) {
        ctx->out->insert(hwnd);
    }
    return TRUE;
}

BOOL CALLBACK monitor_hit_proc(HMONITOR monitor, HDC /*dc*/, LPRECT /*clip*/, LPARAM lParam) {
    const auto* rect = reinterpret_cast<const RECT*>(lParam);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return TRUE;
    }
    if (display_rects_intersect(
            static_cast<int>(rect->left), static_cast<int>(rect->top),
            static_cast<int>(rect->right - rect->left), static_cast<int>(rect->bottom - rect->top),
            static_cast<int>(info.rcMonitor.left), static_cast<int>(info.rcMonitor.top),
            static_cast<int>(info.rcMonitor.right - info.rcMonitor.left),
            static_cast<int>(info.rcMonitor.bottom - info.rcMonitor.top))) {
        return FALSE;
    }
    return TRUE;
}

// True when the rect touches any live display right now (no cache: the set is
// mid-churn during the repairs).
bool rect_hits_live_displays(const RECT& rect) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return false;
    }
    return EnumDisplayMonitors(nullptr, nullptr, monitor_hit_proc,
                               reinterpret_cast<LPARAM>(&rect)) == FALSE;
}

bool window_normal_rect(HWND hwnd, RECT& out) {
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement) == FALSE) {
        return false;
    }
    out.left = placement.rcNormalPosition.left;
    out.top = placement.rcNormalPosition.top;
    out.right = placement.rcNormalPosition.right;
    out.bottom = placement.rcNormalPosition.bottom;
    return true;
}

struct WaveRepairContext {
    const std::set<HWND>* was_iconic = nullptr;
    const std::set<HWND>* recorded = nullptr;
    std::set<HWND>* repaired_set = nullptr;
    RECT laptop_home{};
    int repaired = 0;
};

BOOL CALLBACK wave_repair_proc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WaveRepairContext*>(lParam);
    if (IsIconic(hwnd) == FALSE || ctx->recorded->contains(hwnd) || !window_is_managed(hwnd)) {
        return TRUE;
    }
    // Pre-iconic is only an exclusion when the window hides on-screen (a
    // deliberate minimize): an off-screen normal rect means a wave victim a
    // previous pass missed (aside/dead-display coords), which is repaired.
    // Without this the record skips them (iconic) and the repair skips them
    // (pre-iconic) forever, stranding one more window per run.
    if (ctx->was_iconic->contains(hwnd)) {
        RECT normal{};
        if (!window_normal_rect(hwnd, normal) || rect_hits_live_displays(normal)) {
            return TRUE;
        }
    }
    if (!restore_window_state(hwnd)) {
        return TRUE;
    }
    // Newly-minimized and not ours: restore in place, clamped onto the laptop
    // when the wave orphaned it off-screen (a from==to map is clamp-only).
    // Zoom is not recovered here (unstored for unrecorded windows); one click.
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement) == FALSE) {
        return TRUE;
    }
    RECT home{};
    home.left = placement.rcNormalPosition.left;
    home.top = placement.rcNormalPosition.top;
    home.right = placement.rcNormalPosition.right;
    home.bottom = placement.rcNormalPosition.bottom;
    const RECT target = migrate_window_rect(home, ctx->laptop_home, ctx->laptop_home);
    const int width = static_cast<int>(target.right - target.left);
    const int height = static_cast<int>(target.bottom - target.top);
    if (SetWindowPos(hwnd, nullptr, static_cast<int>(target.left), static_cast<int>(target.top),
                     width, height, SWP_NOZORDER | SWP_NOACTIVATE) == FALSE) {
        return TRUE;
    }
    ++ctx->repaired;
    ctx->repaired_set->insert(hwnd);
    return TRUE;
}

}  // namespace

void snapshot_laptop_windows(const RECT& laptop_rect, std::vector<MigratedWindow>& out) {
    WindowSnapshotContext ctx;
    ctx.laptop = laptop_rect;
    ctx.out = &out;
    EnumWindows(snapshot_window_proc, reinterpret_cast<LPARAM>(&ctx));
}

int place_windows_on_center(const std::vector<MigratedWindow>& windows, const RECT& from_rect,
                            const RECT& center_rect) {
    int moved = 0;
    for (const MigratedWindow& entry : windows) {
        if (IsIconic(entry.hwnd) != FALSE) {
            continue;  // Unexpected before the detach; the repair maps from home.
        }
        RECT current{};
        if (GetWindowRect(entry.hwnd, &current) == FALSE) {
            continue;
        }
        if (move_recorded_window(entry.hwnd, migrate_window_rect(current, from_rect, center_rect),
                                 entry.zoomed)) {
            ++moved;
        }
    }
    return moved;
}

int restore_windows_home(const std::vector<MigratedWindow>& windows) {
    int moved = 0;
    for (const MigratedWindow& entry : windows) {
        if (move_recorded_window(entry.hwnd, entry.home, entry.zoomed)) {
            ++moved;
        }
    }
    return moved;
}

void snapshot_iconic_windows(std::set<HWND>& out) {
    IconicSnapshotContext ctx;
    ctx.out = &out;
    EnumWindows(iconic_snapshot_proc, reinterpret_cast<LPARAM>(&ctx));
}

int repair_takeover_wave(const std::vector<MigratedWindow>& windows, const RECT& laptop_home,
                         const RECT& center_rect, int timeout_ms) {
    std::set<HWND> repaired_all;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int quiet = 0;
    while (quiet < 3 && std::chrono::steady_clock::now() < deadline) {
        int repaired = 0;
        for (const MigratedWindow& entry : windows) {
            if (IsWindow(entry.hwnd) == FALSE || IsIconic(entry.hwnd) == FALSE) {
                continue;
            }
            if (move_recorded_window(entry.hwnd,
                                     migrate_window_rect(entry.home, laptop_home, center_rect),
                                     entry.zoomed)) {
                ++repaired;
                repaired_all.insert(entry.hwnd);
            }
        }
        if (repaired == 0) {
            ++quiet;
        } else {
            quiet = 0;
        }
        if (quiet < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    return static_cast<int>(repaired_all.size());
}

int repair_restore_wave(const std::vector<MigratedWindow>& windows, const std::set<HWND>& was_iconic,
                        const RECT& laptop_home, int timeout_ms) {
    std::set<HWND> recorded;
    for (const MigratedWindow& entry : windows) {
        recorded.insert(entry.hwnd);
    }
    std::set<HWND> repaired_all;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int quiet = 0;
    while (quiet < 3 && std::chrono::steady_clock::now() < deadline) {
        int repaired = 0;
        for (const MigratedWindow& entry : windows) {
            if (IsWindow(entry.hwnd) != FALSE && IsIconic(entry.hwnd) != FALSE &&
                move_recorded_window(entry.hwnd, entry.home, entry.zoomed)) {
                ++repaired;
                repaired_all.insert(entry.hwnd);
            }
        }
        WaveRepairContext ctx;
        ctx.was_iconic = &was_iconic;
        ctx.recorded = &recorded;
        ctx.laptop_home = laptop_home;
        ctx.repaired_set = &repaired_all;
        EnumWindows(wave_repair_proc, reinterpret_cast<LPARAM>(&ctx));
        repaired += ctx.repaired;
        if (repaired == 0) {
            ++quiet;
        } else {
            quiet = 0;
        }
        if (quiet < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    return static_cast<int>(repaired_all.size());
}

bool snapshot_attached_displays(std::vector<DisplayModeSnapshot>& snapshots, std::string& error) {
    std::vector<DisplayModeSnapshot> candidate;
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index) {
        if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) {
            device = DISPLAY_DEVICEW{};
            device.cb = sizeof(device);
            continue;
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) {
            error = "could not snapshot the current display modes";
            return false;
        }
        DisplayModeSnapshot snapshot;
        snapshot.device_name = device.DeviceName;
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(device.DeviceName, 0, &monitor, 0)) {
            snapshot.monitor_id = monitor.DeviceID;
        }
        snapshot.mode = mode;
        snapshot.primary = (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        candidate.push_back(std::move(snapshot));
        device = DISPLAY_DEVICEW{};
        device.cb = sizeof(device);
    }
    if (candidate.empty()) {
        error = "no attached displays found to snapshot";
        return false;
    }
    snapshots = std::move(candidate);
    return true;
}

bool is_display_attached(const std::wstring& device_name) {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index) {
        if (device_name == device.DeviceName) {
            return (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        }
        device = DISPLAY_DEVICEW{};
        device.cb = sizeof(device);
    }
    return false;
}

std::vector<std::pair<std::wstring, std::wstring>> live_device_identities() {
    std::vector<std::pair<std::wstring, std::wstring>> attached;
    std::vector<std::pair<std::wstring, std::wstring>> detached;
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index) {
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        std::wstring id;
        if (EnumDisplayDevicesW(device.DeviceName, 0, &monitor, 0)) {
            id = monitor.DeviceID;
        }
        if (!id.empty()) {
            if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0) {
                attached.emplace_back(device.DeviceName, id);
            } else {
                detached.emplace_back(device.DeviceName, id);
            }
        }
        device = DISPLAY_DEVICEW{};
        device.cb = sizeof(device);
    }
    // Attached first: a ghost twin of a live display must not shadow it.
    attached.insert(attached.end(), detached.begin(), detached.end());
    return attached;
}

std::wstring resolve_live_device_name(
    const std::vector<std::pair<std::wstring, std::wstring>>& live_devices,
    const std::wstring& snapshot_name, const std::wstring& snapshot_id) {
    if (!snapshot_id.empty()) {
        for (const auto& [name, id] : live_devices) {
            if (!id.empty() && id == snapshot_id) {
                return name;
            }
        }
    }
    return snapshot_name;
}

void strip_display_frequency(DEVMODEW& mode) {
    mode.dmFields &= static_cast<DWORD>(~DM_DISPLAYFREQUENCY);
    mode.dmDisplayFrequency = 0;
}

bool find_internal_display(std::wstring& device_name, std::string& error) {
    device_name.clear();
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!query_display_config(QDC_ONLY_ACTIVE_PATHS, paths, modes, error)) {
        return false;
    }
    std::vector<std::tuple<std::wstring, bool, bool>> candidates;
    for (size_t i = 0; i < paths.size(); ++i) {
        const DISPLAYCONFIG_PATH_INFO& path = paths[i];
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
            continue;
        }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = path.sourceInfo.adapterId;
        name.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) {
            continue;
        }
        const bool internal =
            path.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL;
        candidates.emplace_back(name.viewGdiDeviceName, internal,
                                adapter_is_primary(name.viewGdiDeviceName));
    }
    device_name = select_internal_display(candidates);
    return true;
}

namespace {

std::set<std::wstring> virtual_device_names() {
    std::set<std::wstring> names;
    for (const VirtualDisplayInfo& display : enumerate_virtual_displays()) {
        if (!display.device_name.empty()) {
            names.insert(display.device_name);
        }
    }
    return names;
}

// Polls for an expected attach state after a mode commit. The commit itself
// is synchronous, but a bounded re-check keeps a transient enumeration lag
// from failing a takeover or a restore. A device that no longer enumerates
// counts as detached.
bool wait_for_attach_state(const std::wstring& device_name, bool attached) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (is_display_attached(device_name) == attached) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

LONG commit_display_changes() {
    return ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
}

// True when the display currently reports the expected geometry (and, when
// asked, the expected primary flag). Single shot; the waiter below polls it.
bool display_layout_matches(const std::wstring& device_name, int x, int y, int w, int h,
                            bool check_primary, bool want_primary) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    return EnumDisplaySettingsExW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0) != 0 &&
           static_cast<int>(mode.dmPosition.x) == x && static_cast<int>(mode.dmPosition.y) == y &&
           static_cast<int>(mode.dmPelsWidth) == w && static_cast<int>(mode.dmPelsHeight) == h &&
           (!check_primary || adapter_is_primary(device_name) == want_primary);
}

// Polls the layout check: commits apply synchronously, but a bounded re-check
// absorbs transient re-layout.
bool wait_for_display_layout(const std::wstring& device_name, int x, int y, int w, int h,
                             bool check_primary, bool want_primary) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (display_layout_matches(device_name, x, y, w, h, check_primary, want_primary)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// Polls until the display is attached and its rect overlaps none of the
// slots. Asides are verified by non-overlap (not exact coordinates) because
// Windows packs a gapped display adjacent instead of honoring the gap.
bool wait_for_aside_clear(const std::wstring& device_name,
                          const std::vector<PlannedDesktop>& slots, int slot_width,
                          int slot_height) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (is_display_attached(device_name) &&
            EnumDisplaySettingsExW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
            bool clear = true;
            for (const PlannedDesktop& slot : slots) {
                if (display_rects_intersect(
                        static_cast<int>(mode.dmPosition.x), static_cast<int>(mode.dmPosition.y),
                        static_cast<int>(mode.dmPelsWidth),
                        static_cast<int>(mode.dmPelsHeight), slot.x, slot.y, slot_width,
                        slot_height)) {
                    clear = false;
                    break;
                }
            }
            if (clear) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// Deactivates exactly the internal display path (CCD). A NULL-mode
// ChangeDisplaySettingsEx detach is silently ignored on this machine, staged
// or immediate, so the detach goes through here. Deliberately NOT persisted
// (no SDC_SAVE_TO_DATABASE): a reboot returns to the pristine topology.
bool deactivate_internal_path(const std::wstring& detach_device, std::string& error) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!query_display_config(QDC_ONLY_ACTIVE_PATHS, paths, modes, error)) {
        return false;
    }
    size_t target = paths.size();
    for (size_t i = 0; i < paths.size(); ++i) {
        if ((paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) == 0 ||
            paths[i].targetInfo.outputTechnology != DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL) {
            continue;
        }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = paths[i].sourceInfo.adapterId;
        name.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) {
            continue;
        }
        if (detach_device == name.viewGdiDeviceName) {
            target = i;
            break;
        }
    }
    if (target == paths.size()) {
        error = "the laptop display left the active topology before it could be detached";
        return false;
    }
    paths[target].flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
    const LONG apply_result =
        SetDisplayConfig(static_cast<UINT32>(paths.size()), paths.data(),
                         static_cast<UINT32>(modes.size()), modes.data(),
                         SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG);
    if (apply_result != ERROR_SUCCESS) {
        error = "Windows refused to detach the laptop display (code " +
                std::to_string(apply_result) + ")";
        return false;
    }
    return true;
}

bool commit_topology_snapshots(const std::vector<DisplayModeSnapshot>& snapshots) {
    // Primary first: every stage call validates the pending batch, which must
    // never transiently lack a primary at the origin.
    bool staged = true;
    for (bool want_primary : {true, false}) {
        for (const DisplayModeSnapshot& snapshot : snapshots) {
            if (snapshot.primary != want_primary) {
                continue;
            }
            DEVMODEW mode = snapshot.mode;
            DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
            if (snapshot.primary) {
                flags |= CDS_SET_PRIMARY;
            }
            if (ChangeDisplaySettingsExW(snapshot.device_name.c_str(), &mode, nullptr, flags,
                                         nullptr) != DISP_CHANGE_SUCCESSFUL) {
                staged = false;
            }
        }
    }
    return staged && commit_display_changes() == DISP_CHANGE_SUCCESSFUL;
}

}  // namespace

bool restore_display_topology(WorkspaceTopology& topo, VddClient& vdd, std::string& error);

bool apply_workspace_topology(const std::vector<DisplayModeSnapshot>& snapshots,
                              const std::vector<PlannedDesktop>& planned, int center_driver,
                              int width, int height, int preferred_hz,
                              const std::wstring& detach_device, const MigrationSeed& seed,
                              VddClient& vdd, WorkspaceTopology& topo,
                              std::vector<ConfiguredDisplay>& configured, std::string& error) {
    topo = WorkspaceTopology{};
    // Snapshot the taskbar preference before any churn: the restore hands it
    // back (Explorer recreates the taskbar across the detach and drops it).
    const int taskbar_state = query_taskbar_state();
    topo.taskbar_state = taskbar_state;
    if (planned.empty()) {
        error = "no virtual displays planned";
        return false;
    }
    if (snapshots.empty()) {
        error = "no display snapshots to roll back to";
        return false;
    }
    std::vector<int> indices;
    indices.reserve(planned.size());
    for (const PlannedDesktop& desktop : planned) {
        indices.push_back(desktop.driver_index);
    }
    // The mode query below needs live devices: a freshly-connected VDD only
    // exposes its mode list once Windows has attached it. The name map is
    // rebuilt from that same active set, never from a pre-wait enumeration
    // that may still point at registry ghosts.
    std::map<int, std::wstring> device_names;
    {
        std::vector<VirtualDisplayInfo> enumerated;
        if (!wait_for_virtual_displays(indices, 5000, enumerated, error)) {
            return false;
        }
        for (const VirtualDisplayInfo& display : enumerated) {
            device_names[display.driver_index] = display.device_name;
        }
    }
    const std::set<std::wstring> vdd_devices = virtual_device_names();

    struct StagedMode {
        std::wstring device;
        DEVMODEW mode;
        bool primary = false;
        int driver = -1;  // VDD driver index, or -1 for an aside move
        int hz = 0;
        int x = 0;
        int y = 0;
    };
    // Resolve every mode before touching anything: a failure here changes nothing.
    std::vector<StagedMode> staged;
    staged.reserve(planned.size());
    for (const PlannedDesktop& desktop : planned) {
        const auto name = device_names.find(desktop.driver_index);
        if (name == device_names.end() || name->second.empty()) {
            error = "a planned virtual display has no Windows display name";
            return false;
        }
        const int refresh = choose_refresh_rate(refresh_rates(name->second, width, height),
                                                preferred_hz);
        if (refresh == 0) {
            error = "the Parsec VDD does not expose the requested display mode";
            return false;
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        mode.dmPelsWidth = static_cast<DWORD>(width);
        mode.dmPelsHeight = static_cast<DWORD>(height);
        mode.dmDisplayFrequency = static_cast<DWORD>(refresh);
        mode.dmPosition.x = static_cast<LONG>(desktop.x);
        mode.dmPosition.y = static_cast<LONG>(desktop.y);
        mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
        StagedMode item;
        item.device = name->second;
        item.mode = mode;
        item.primary = desktop.driver_index == center_driver;
        item.driver = desktop.driver_index;
        item.hz = refresh;
        item.x = desktop.x;
        item.y = desktop.y;
        staged.push_back(item);
    }
    const auto live_aside = live_device_identities();
    for (const PlannedAside& aside : plan_aside_moves(snapshots, planned, width, height,
                                                      vdd_devices)) {
        const DisplayModeSnapshot* snapshot = nullptr;
        for (const DisplayModeSnapshot& candidate : snapshots) {
            if (candidate.device_name == aside.device_name) {
                snapshot = &candidate;
                break;
            }
        }
        if (snapshot == nullptr) {
            error = "an aside target has no snapshot";
            return false;
        }
        DEVMODEW mode = snapshot->mode;
        mode.dmPosition.x = static_cast<LONG>(aside.x);
        mode.dmPosition.y = static_cast<LONG>(aside.y);
        mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
        StagedMode item;
        item.device =
            resolve_live_device_name(live_aside, aside.device_name, snapshot->monitor_id);
        item.mode = mode;
        item.primary = false;
        staged.push_back(item);
    }

    // The window record arrives pre-built (see MigrationSeed): recording here
    // would run post-connect, when the laptop may already sit asided and the
    // pristine home rect matches nothing.
    std::vector<MigratedWindow> migrated = seed.windows;
    const RECT laptop_home = seed.laptop_home;
    const bool have_laptop_home = seed.have_laptop_home;
    // Who has focus when the takeover starts: the restore hands it back.
    HWND focus_window = GetForegroundWindow();
    bool touched = false;
    const auto rollback = [&]() {
        // A failed stage/commit leaves its batch pending system-wide, where it
        // would poison the restore commits below; flush it first (a no-op when
        // the pending set is already empty).
        commit_display_changes();
        if (!touched) {
            vdd.disconnect();
            return;
        }
        // The full shutdown path on a temp topology (pins, window move-back,
        // minimize repair): a mid-takeover failure converges identically.
        WorkspaceTopology partial;
        partial.snapshots = snapshots;
        partial.migrated_windows = migrated;
        partial.focus_window = focus_window;
        partial.taskbar_state = taskbar_state;
        partial.active = true;
        std::string restore_error;
        if (!restore_display_topology(partial, vdd, restore_error)) {
            error += "; restoring the previous display arrangement also failed: " + restore_error;
        }
    };
    // commitA in dependency order: the new center (primary at the origin)
    // first, then the other slots, then the aside moves. Staged in any other
    // order the batch fails validation (-1) or is silently dropped.
    for (int pass = 0; pass < 3; ++pass) {
        for (const StagedMode& item : staged) {
            const int item_pass = item.primary ? 0 : (item.driver >= 0 ? 1 : 2);
            if (item_pass != pass) {
                continue;
            }
            DEVMODEW mode = item.mode;
            DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
            if (item.primary) {
                flags |= CDS_SET_PRIMARY;
            }
            const LONG stage_result = ChangeDisplaySettingsExW(item.device.c_str(), &mode, nullptr,
                                                               flags, nullptr);
            if (stage_result != DISP_CHANGE_SUCCESSFUL) {
                error = "Windows rejected the workspace display mode (code " +
                        std::to_string(stage_result) + ")";
                rollback();
                return false;
            }
        }
    }
    const LONG commit_result = commit_display_changes();
    touched = true;
    if (commit_result != DISP_CHANGE_SUCCESSFUL) {
        error = "Windows failed to commit the workspace display arrangement (code " +
                std::to_string(commit_result) + ")";
        rollback();
        return false;
    }
    {
        std::vector<VirtualDisplayInfo> active;
        if (!wait_for_virtual_displays(indices, 5000, active, error)) {
            rollback();
            return false;
        }
    }
    for (const StagedMode& item : staged) {
        if (item.driver < 0) {
            continue;
        }
        if (!wait_for_display_layout(item.device, item.x, item.y, width, height, true,
                                     item.primary)) {
            error = item.primary ? "the center desktop did not become the primary display"
                                 : "a virtual desktop did not reach its arranged position";
            rollback();
            return false;
        }
    }
    for (const StagedMode& item : staged) {
        if (item.driver >= 0) {
            continue;
        }
        if (!wait_for_aside_clear(item.device, planned, width, height)) {
            error = "a display did not move out of the workspace slots";
            rollback();
            return false;
        }
    }
    // The windows rode commitA aside with the laptop; move them onto the
    // center desktop now, while every display is alive and no automatic
    // migration is in flight. Best-effort: on a query failure the windows
    // stay put and the restore's move-back still collects the survivors.
    int windows_placed = 0;
    int takeover_repaired = 0;
    RECT center_rect{};
    bool have_center = false;
    if (!detach_device.empty() && !migrated.empty()) {
        int center_x = 0;
        int center_y = 0;
        for (const PlannedDesktop& slot : planned) {
            if (slot.driver_index == center_driver) {
                center_x = slot.x;
                center_y = slot.y;
            }
        }
        DEVMODEW aside_mode{};
        aside_mode.dmSize = sizeof(aside_mode);
        if (EnumDisplaySettingsExW(detach_device.c_str(), ENUM_CURRENT_SETTINGS, &aside_mode, 0) !=
            FALSE) {
            RECT from{};
            from.left = aside_mode.dmPosition.x;
            from.top = aside_mode.dmPosition.y;
            from.right = from.left + static_cast<LONG>(aside_mode.dmPelsWidth);
            from.bottom = from.top + static_cast<LONG>(aside_mode.dmPelsHeight);
            center_rect.left = static_cast<LONG>(center_x);
            center_rect.top = static_cast<LONG>(center_y);
            center_rect.right = center_rect.left + static_cast<LONG>(width);
            center_rect.bottom = center_rect.top + static_cast<LONG>(height);
            have_center = true;
            windows_placed = place_windows_on_center(migrated, from, center_rect);
        }
    }
    if (!detach_device.empty()) {
        if (!deactivate_internal_path(detach_device, error)) {
            rollback();
            return false;
        }
        if (!wait_for_attach_state(detach_device, false)) {
            error = "the laptop display is still attached after the workspace takeover";
            rollback();
            return false;
        }
        // The detach must not shuffle the slots; re-pin them if it did.
        const auto slots_hold = [&]() {
            for (const StagedMode& item : staged) {
                if (item.driver < 0) {
                    continue;
                }
                if (!wait_for_display_layout(item.device, item.x, item.y, width, height, true,
                                             item.primary)) {
                    return false;
                }
            }
            return true;
        };
        if (!slots_hold()) {
            for (int pass = 0; pass < 2; ++pass) {
                for (const StagedMode& item : staged) {
                    if (item.driver < 0 || (item.primary ? 0 : 1) != pass) {
                        continue;
                    }
                    DEVMODEW mode = item.mode;
                    DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
                    if (item.primary) {
                        flags |= CDS_SET_PRIMARY;
                    }
                    if (ChangeDisplaySettingsExW(item.device.c_str(), &mode, nullptr, flags,
                                                 nullptr) != DISP_CHANGE_SUCCESSFUL) {
                        error = "Windows rejected the workspace display mode while re-pinning";
                        rollback();
                        return false;
                    }
                }
            }
            if (commit_display_changes() != DISP_CHANGE_SUCCESSFUL) {
                error = "Windows failed to re-pin the workspace arrangement";
                rollback();
                return false;
            }
            if (!slots_hold()) {
                error = "a virtual desktop did not reach its arranged position";
                rollback();
                return false;
            }
        }
        // The detach minimizes windows by stale display-association (Windows 11
        // "minimize on disconnect", on by default): unminimize ours back onto
        // the center desktop, mapped from their laptop homes (an iconic window
        // reports (-32000,-32000), which carries no offset).
        if (have_laptop_home && have_center && !migrated.empty()) {
            takeover_repaired =
                repair_takeover_wave(migrated, laptop_home, center_rect, 10000);
        }
    }
    topo.snapshots = snapshots;
    topo.detached_device = detach_device;
    topo.migrated_windows = std::move(migrated);
    topo.focus_window = focus_window;
    topo.windows_placed = windows_placed;
    topo.windows_repaired = takeover_repaired;
    topo.active = true;
    std::vector<ConfiguredDisplay> candidate;
    candidate.reserve(planned.size());
    for (const StagedMode& item : staged) {
        if (item.driver < 0) {
            continue;
        }
        candidate.push_back(
            ConfiguredDisplay{item.driver, item.device, width, height, item.hz, item.x, 0});
    }
    configured = std::move(candidate);
    return true;
}

bool reposition_virtual_displays(const std::vector<PlannedDesktop>& planned,
                                 const std::map<int, std::wstring>& device_names, int center_driver,
                                 int width, int height, int preferred_hz,
                                 std::vector<ConfiguredDisplay>& configured, std::string& error) {
    if (planned.empty()) {
        error = "no virtual displays planned";
        return false;
    }
    struct StagedMode {
        std::wstring device;
        DEVMODEW mode;
        bool primary = false;
        int driver = -1;
        int hz = 0;
        int x = 0;
    };
    std::vector<StagedMode> staged;
    staged.reserve(planned.size());
    std::vector<DisplayModeSnapshot> snapshots;
    snapshots.reserve(planned.size());
    for (const PlannedDesktop& desktop : planned) {
        const auto name = device_names.find(desktop.driver_index);
        if (name == device_names.end() || name->second.empty()) {
            error = "a planned virtual display has no Windows display name";
            return false;
        }
        DEVMODEW previous{};
        previous.dmSize = sizeof(previous);
        if (!EnumDisplaySettingsExW(name->second.c_str(), ENUM_CURRENT_SETTINGS, &previous, 0)) {
            error = "could not snapshot a virtual display before configuration";
            return false;
        }
        DisplayModeSnapshot snapshot;
        snapshot.device_name = name->second;
        snapshot.mode = previous;
        snapshot.primary = adapter_is_primary(name->second);
        snapshots.push_back(std::move(snapshot));
        const int refresh = choose_refresh_rate(refresh_rates(name->second, width, height),
                                                preferred_hz);
        if (refresh == 0) {
            error = "the Parsec VDD does not expose the requested display mode";
            return false;
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        mode.dmPelsWidth = static_cast<DWORD>(width);
        mode.dmPelsHeight = static_cast<DWORD>(height);
        mode.dmDisplayFrequency = static_cast<DWORD>(refresh);
        mode.dmPosition.x = static_cast<LONG>(desktop.x);
        mode.dmPosition.y = static_cast<LONG>(desktop.y);
        mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
        StagedMode item;
        item.device = name->second;
        item.mode = mode;
        item.primary = desktop.driver_index == center_driver;
        item.driver = desktop.driver_index;
        item.hz = refresh;
        item.x = desktop.x;
        staged.push_back(item);
    }
    // The new center stages first (same primary-first rule as the takeover).
    for (bool want_primary : {true, false}) {
        for (const StagedMode& item : staged) {
            if (item.primary != want_primary) {
                continue;
            }
            DEVMODEW mode = item.mode;
            DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
            if (item.primary) {
                flags |= CDS_SET_PRIMARY;
            }
            if (ChangeDisplaySettingsExW(item.device.c_str(), &mode, nullptr, flags, nullptr) !=
                DISP_CHANGE_SUCCESSFUL) {
                error = "Windows rejected the virtual display mode";
                if (!commit_topology_snapshots(snapshots)) {
                    error += "; restoring the previous display modes also failed";
                }
                return false;
            }
        }
    }
    if (ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) !=
        DISP_CHANGE_SUCCESSFUL) {
        error = "Windows failed to commit the virtual display arrangement";
        if (!commit_topology_snapshots(snapshots)) {
            error += "; restoring the previous display modes also failed";
        }
        return false;
    }
    std::vector<ConfiguredDisplay> candidate;
    candidate.reserve(staged.size());
    for (const StagedMode& item : staged) {
        candidate.push_back(
            ConfiguredDisplay{item.driver, item.device, width, height, item.hz, item.x, 0});
    }
    configured = std::move(candidate);
    return true;
}

namespace {
// Stages one restore pin, retrying once without the refresh rate when the
// driver rejects the snapshot mode as unsupported (BADMODE): the snapshot
// can capture a transient/VRR rate, and a stale rate must not fail the
// whole restore.
LONG stage_restore_mode(const std::wstring& device, DEVMODEW mode, DWORD flags) {
    const LONG first = ChangeDisplaySettingsExW(device.c_str(), &mode, nullptr, flags, nullptr);
    if (first != DISP_CHANGE_BADMODE) {
        return first;
    }
    strip_display_frequency(mode);
    return ChangeDisplaySettingsExW(device.c_str(), &mode, nullptr, flags, nullptr);
}
}  // namespace

bool is_glasses_display(const std::wstring& description, const std::wstring& device_id,
                        bool primary) {
    const std::wstring searchable = description + L" " + device_id;
    if (contains_case_insensitive(searchable, L"smartglasses") ||
        contains_case_insensitive(searchable, L"rayneo")) {
        return true;
    }
    return !primary && contains_case_insensitive(searchable, L"tcl");
}

bool is_glasses_friendly_name(const std::wstring& friendly_name) {
    return contains_case_insensitive(friendly_name, L"smartglasses") ||
           contains_case_insensitive(friendly_name, L"rayneo") ||
           contains_case_insensitive(friendly_name, L"tcl");
}

bool find_detached_glasses_display(std::wstring& device_name) {
    const std::set<std::wstring> vdd_devices = virtual_device_names();
    std::wstring tcl_fallback;
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &adapter, 0); ++index) {
        const std::wstring name = adapter.DeviceName;
        const bool attached =
            (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        adapter = DISPLAY_DEVICEW{};
        adapter.cb = sizeof(adapter);
        if (attached || vdd_devices.contains(name)) {
            continue;
        }
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(name.c_str(), 0, &monitor, 0) == 0) {
            continue;  // Empty port or stale ghost: no EDID, nothing to match.
        }
        // A detached display is never primary; match as a secondary so an
        // unbranded TCL panel is found (branded names match regardless).
        // Branded wins immediately; a TCL-only match waits as fallback so a
        // detached TCL TV never shadows detached glasses.
        if (!is_glasses_display(monitor.DeviceString, monitor.DeviceID, false)) {
            continue;
        }
        const std::wstring searchable =
            std::wstring(monitor.DeviceString) + L" " + monitor.DeviceID;
        if (contains_case_insensitive(searchable, L"smartglasses") ||
            contains_case_insensitive(searchable, L"rayneo")) {
            device_name = name;
            return true;
        }
        if (tcl_fallback.empty()) {
            tcl_fallback = name;
        }
    }
    if (!tcl_fallback.empty()) {
        device_name = tcl_fallback;
        return true;
    }
    return false;
}

bool reattach_detached_glasses(std::string& error) {
    std::wstring target;
    if (!find_detached_glasses_display(target)) {
        error = "no disconnected glasses display is present";
        return false;
    }
    DEVMODEW stored{};
    stored.dmSize = sizeof(stored);
    const bool have_stored =
        EnumDisplaySettingsExW(target.c_str(), ENUM_CURRENT_SETTINGS, &stored, 0) != 0 &&
        stored.dmPelsWidth > 0 && stored.dmPelsHeight > 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (have_stored) {
        mode = stored;
    } else {
        // No usable registry mode (never attached under this install): ask
        // the driver for 1080p at a sane rate instead of guessing blind.
        const int refresh = choose_refresh_rate(refresh_rates(target, 1920, 1080), 60);
        if (refresh == 0) {
            error = "the disconnected glasses display exposes no usable mode";
            return false;
        }
        mode.dmPelsWidth = 1920;
        mode.dmPelsHeight = 1080;
        mode.dmDisplayFrequency = static_cast<DWORD>(refresh);
    }
    // Phase 1 (attach): like R1a, attach and move are separate commits - an
    // overlapping staged slot is auto-relocated by Windows instead of
    // honored (measured: staging (0,0) landed at (-1920,0)), so stage right
    // of the desktop, which is overlap-free by construction (a free staged
    // slot is honored: (2880,0) landed at (2880,0)). Never takes primary.
    mode.dmPosition.x =
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    mode.dmPosition.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
    LONG stage_result =
        stage_restore_mode(target, mode, CDS_UPDATEREGISTRY | CDS_NORESET);
    if (stage_result != DISP_CHANGE_SUCCESSFUL && have_stored) {
        // A stale registry mode (driver update changed the mode list) must
        // not brick the recovery: retry once with a driver-queried mode.
        commit_display_changes();
        mode.dmPelsWidth = 1920;
        mode.dmPelsHeight = 1080;
        mode.dmDisplayFrequency =
            static_cast<DWORD>(choose_refresh_rate(refresh_rates(target, 1920, 1080), 60));
        stage_result =
            stage_restore_mode(target, mode, CDS_UPDATEREGISTRY | CDS_NORESET);
    }
    if (stage_result != DISP_CHANGE_SUCCESSFUL) {
        commit_display_changes();
        error = "Windows rejected the glasses re-attach mode (code " +
                std::to_string(stage_result) + ")";
        return false;
    }
    if (commit_display_changes() != DISP_CHANGE_SUCCESSFUL) {
        commit_display_changes();
        error = "Windows failed to re-attach the glasses display";
        return false;
    }
    if (!wait_for_attach_state(target, true)) {
        error = "the glasses display did not re-attach";
        return false;
    }
    // Phase 2 (move, best-effort): the attach lands wherever Windows puts
    // it; when the registry still holds the pre-disconnect position and its
    // slot is free, move back there. The contract is attached, not placed:
    // a failed move still returns success.
    if (have_stored) {
        DEVMODEW current{};
        current.dmSize = sizeof(current);
        if (EnumDisplaySettingsExW(target.c_str(), ENUM_CURRENT_SETTINGS, &current, 0) != 0) {
            const int w = static_cast<int>(current.dmPelsWidth);
            const int h = static_cast<int>(current.dmPelsHeight);
            const int x = stored.dmPosition.x;
            const int y = stored.dmPosition.y;
            bool blocked = false;
            DISPLAY_DEVICEW other{};
            other.cb = sizeof(other);
            for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &other, 0); ++index) {
                const std::wstring name = other.DeviceName;
                const bool attached =
                    (other.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
                other = DISPLAY_DEVICEW{};
                other.cb = sizeof(other);
                if (!attached || name == target) {
                    continue;
                }
                DEVMODEW other_mode{};
                other_mode.dmSize = sizeof(other_mode);
                if (EnumDisplaySettingsExW(name.c_str(), ENUM_CURRENT_SETTINGS, &other_mode,
                                           0) == 0) {
                    continue;
                }
                if (display_rects_intersect(x, y, w, h, other_mode.dmPosition.x,
                                            other_mode.dmPosition.y,
                                            static_cast<int>(other_mode.dmPelsWidth),
                                            static_cast<int>(other_mode.dmPelsHeight))) {
                    blocked = true;
                    break;
                }
            }
            if (!blocked) {
                DEVMODEW home = current;
                home.dmPosition.x = x;
                home.dmPosition.y = y;
                home.dmFields =
                    DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
                if (stage_restore_mode(target, home, CDS_UPDATEREGISTRY | CDS_NORESET) ==
                    DISP_CHANGE_SUCCESSFUL) {
                    commit_display_changes();
                } else {
                    commit_display_changes();
                }
            }
        }
    }
    return true;
}

bool reactivate_glasses_path(std::string& error) {
    std::vector<DISPLAYCONFIG_PATH_INFO> all_paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> all_modes;
    if (!query_display_config(QDC_ALL_PATHS, all_paths, all_modes, error)) {
        return false;
    }
    // Inactive glasses paths, branded targets first (a TCL match could be a
    // TV; the strict post-activation verify plus rollback is the backstop).
    std::vector<size_t> candidates;
    for (size_t i = 0; i < all_paths.size(); ++i) {
        const DISPLAYCONFIG_PATH_INFO& path = all_paths[i];
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0) {
            continue;
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
            continue;
        }
        if (!is_glasses_friendly_name(target.monitorFriendlyDeviceName)) {
            continue;
        }
        const bool branded =
            contains_case_insensitive(target.monitorFriendlyDeviceName, L"smartglasses") ||
            contains_case_insensitive(target.monitorFriendlyDeviceName, L"rayneo");
        candidates.insert(branded ? candidates.begin() : candidates.end(), i);
    }
    if (candidates.empty()) {
        error = "no inactive glasses display path exists";
        return false;
    }
    const auto same_endpoint = [](const LUID& a, UINT32 a_id, const LUID& b, UINT32 b_id) {
        return a.HighPart == b.HighPart && a.LowPart == b.LowPart && a_id == b_id;
    };
    std::string last_error = "no inactive glasses path could be activated";
    for (size_t candidate : candidates) {
        const DISPLAYCONFIG_PATH_INFO& template_path = all_paths[candidate];
        std::vector<DISPLAYCONFIG_PATH_INFO> paths;
        std::vector<DISPLAYCONFIG_MODE_INFO> modes;
        if (!query_display_config(QDC_ONLY_ACTIVE_PATHS, paths, modes, error)) {
            return false;
        }
        bool conflicted = false;
        for (const DISPLAYCONFIG_PATH_INFO& active : paths) {
            if (same_endpoint(active.sourceInfo.adapterId, active.sourceInfo.id,
                              template_path.sourceInfo.adapterId,
                              template_path.sourceInfo.id) ||
                same_endpoint(active.targetInfo.adapterId, active.targetInfo.id,
                              template_path.targetInfo.adapterId,
                              template_path.targetInfo.id)) {
                conflicted = true;  // A source/target drives one active path.
                break;
            }
        }
        if (conflicted) {
            last_error = "all inactive glasses paths conflict with active ones";
            continue;
        }
        DISPLAYCONFIG_PATH_INFO submit = template_path;
        submit.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        const UINT32 stored_src = template_path.sourceInfo.modeInfoIdx;
        const UINT32 stored_dst = template_path.targetInfo.modeInfoIdx;
        // QDC_ALL_PATHS marks inactive-path indexes INVALID per spec, so this
        // branch is defensive; when indexes are present they must name modes
        // for this path's own endpoints (and must be real indexes, not the
        // virtual-mode union). Anything else falls back to preferred mode.
        const bool indexes_usable =
            (template_path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) == 0 &&
            stored_src != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            stored_dst != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            stored_src < all_modes.size() && stored_dst < all_modes.size();
        const bool have_stored =
            indexes_usable &&
            all_modes[stored_src].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
            all_modes[stored_dst].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET &&
            same_endpoint(all_modes[stored_src].adapterId, all_modes[stored_src].id,
                          template_path.sourceInfo.adapterId, template_path.sourceInfo.id) &&
            same_endpoint(all_modes[stored_dst].adapterId, all_modes[stored_dst].id,
                          template_path.targetInfo.adapterId, template_path.targetInfo.id);
        // Preferred-mode construction shared by the first attempt (no stored
        // modes) and the retry (stored modes refused): target-native mode at
        // a free slot right of the desktop.
        const auto build_preferred_modes = [&](DISPLAYCONFIG_PATH_INFO& out_submit,
                                               std::vector<DISPLAYCONFIG_MODE_INFO>& out_modes) {
            DISPLAYCONFIG_TARGET_PREFERRED_MODE preferred{};
            preferred.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
            preferred.header.size = sizeof(preferred);
            preferred.header.adapterId = template_path.targetInfo.adapterId;
            preferred.header.id = template_path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&preferred.header) != ERROR_SUCCESS ||
                preferred.width == 0 || preferred.height == 0) {
                return false;
            }
            DISPLAYCONFIG_MODE_INFO source{};
            source.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
            source.adapterId = template_path.sourceInfo.adapterId;
            source.id = template_path.sourceInfo.id;
            source.sourceMode.width = preferred.width;
            source.sourceMode.height = preferred.height;
            source.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
            source.sourceMode.position.x = GetSystemMetrics(SM_XVIRTUALSCREEN) +
                                           GetSystemMetrics(SM_CXVIRTUALSCREEN);
            source.sourceMode.position.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
            DISPLAYCONFIG_MODE_INFO target{};
            target.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
            target.adapterId = template_path.targetInfo.adapterId;
            target.id = template_path.targetInfo.id;
            target.targetMode = preferred.targetMode;
            out_submit.sourceInfo.modeInfoIdx = static_cast<UINT32>(out_modes.size());
            out_modes.push_back(source);
            out_submit.targetInfo.modeInfoIdx = static_cast<UINT32>(out_modes.size());
            out_modes.push_back(target);
            return true;
        };
        const size_t base_modes = modes.size();
        bool used_stored = false;
        if (have_stored) {
            submit.sourceInfo.modeInfoIdx = static_cast<UINT32>(modes.size());
            modes.push_back(all_modes[stored_src]);
            submit.targetInfo.modeInfoIdx = static_cast<UINT32>(modes.size());
            modes.push_back(all_modes[stored_dst]);
            used_stored = true;
        } else if (!build_preferred_modes(submit, modes)) {
            last_error = "an inactive glasses path has no usable mode";
            continue;
        }
        // Snapshots for rollback (non-const: SetDisplayConfig takes mutable
        // pointers, though it does not modify the submission).
        std::vector<DISPLAYCONFIG_PATH_INFO> pre_paths = paths;
        std::vector<DISPLAYCONFIG_MODE_INFO> pre_modes = modes;
        paths.push_back(submit);
        LONG apply_result = SetDisplayConfig(
            static_cast<UINT32>(paths.size()), paths.data(), static_cast<UINT32>(modes.size()),
            modes.data(), SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE);
        if (apply_result != ERROR_SUCCESS && used_stored) {
            // Stored modes refused (stale row): rebuild preferred and retry
            // once before moving to the next candidate.
            modes.resize(base_modes);
            submit = template_path;
            submit.flags |= DISPLAYCONFIG_PATH_ACTIVE;
            if (!build_preferred_modes(submit, modes)) {
                last_error = "an inactive glasses path has no usable mode";
                continue;
            }
            paths.back() = submit;
            apply_result = SetDisplayConfig(
                static_cast<UINT32>(paths.size()), paths.data(),
                static_cast<UINT32>(modes.size()), modes.data(),
                SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE);
        }
        if (apply_result != ERROR_SUCCESS) {
            last_error = "Windows refused to activate the glasses display path (code " +
                         std::to_string(apply_result) + ")";
            continue;
        }
        // Strict verify against the ACTIVATED endpoint (not any-attached
        // display): map the path back to its GDI device and require the
        // strict matcher there, else roll back (wrong display).
        bool verified = false;
        for (int attempt = 0; attempt < 20 && !verified; ++attempt) {
            std::vector<DISPLAYCONFIG_PATH_INFO> verify_paths;
            std::vector<DISPLAYCONFIG_MODE_INFO> verify_modes;
            std::string verify_error;
            if (query_display_config(QDC_ONLY_ACTIVE_PATHS, verify_paths, verify_modes,
                                     verify_error)) {
                for (const DISPLAYCONFIG_PATH_INFO& active : verify_paths) {
                    if (!same_endpoint(active.targetInfo.adapterId, active.targetInfo.id,
                                       template_path.targetInfo.adapterId,
                                       template_path.targetInfo.id)) {
                        continue;
                    }
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    source.header.size = sizeof(source);
                    source.header.adapterId = active.sourceInfo.adapterId;
                    source.header.id = active.sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
                        is_display_attached(source.viewGdiDeviceName)) {
                        DISPLAY_DEVICEW adapter{};
                        adapter.cb = sizeof(adapter);
                        bool primary = false;
                        const std::wstring source_device = source.viewGdiDeviceName;
                        for (DWORD index = 0;
                             EnumDisplayDevicesW(nullptr, index, &adapter, 0); ++index) {
                            if (source_device == adapter.DeviceName) {
                                primary = (adapter.StateFlags &
                                           DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
                                break;
                            }
                            adapter = DISPLAY_DEVICEW{};
                            adapter.cb = sizeof(adapter);
                        }
                        DISPLAY_DEVICEW monitor{};
                        monitor.cb = sizeof(monitor);
                        if (EnumDisplayDevicesW(source.viewGdiDeviceName, 0, &monitor, 0) !=
                                0 &&
                            is_glasses_display(monitor.DeviceString, monitor.DeviceID,
                                               primary)) {
                            verified = true;
                        }
                    }
                    break;  // One active path per target; no need to look on.
                }
            }
            if (!verified) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        if (verified) {
            return true;
        }
        // Not the glasses (a TCL TV, or the modes did not stick): resubmit
        // the pre-attempt set, SAVED (the apply above persisted the wrong
        // config; leaving it would poison the next recall).
        const LONG rollback_result = SetDisplayConfig(
            static_cast<UINT32>(pre_paths.size()), pre_paths.data(),
            static_cast<UINT32>(pre_modes.size()), pre_modes.data(),
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE);
        last_error = rollback_result == ERROR_SUCCESS
                         ? "an activated display path did not resolve to the glasses"
                         : "rollback failed after a wrong-path activation (code " +
                               std::to_string(rollback_result) + ")";
    }
    error = last_error;
    return false;
}

namespace {

std::string narrow_display_string(const wchar_t* wide) {
    if (wide == nullptr || wide[0] == L'\0') {
        return "(none)";
    }
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return "(?)";
    }
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

}  // namespace

std::string describe_display_landscape() {
    std::string out;
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &adapter, 0); ++index) {
        const std::wstring name = adapter.DeviceName;
        const bool attached =
            (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        const bool primary =
            (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        adapter = DISPLAY_DEVICEW{};
        adapter.cb = sizeof(adapter);
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        const bool have_monitor = EnumDisplayDevicesW(name.c_str(), 0, &monitor, 0) != 0;
        out += "  gdi " + narrow_display_string(name.c_str()) +
               (attached ? " attached" : " detached") + (primary ? " primary" : "") +
               " monitor=" +
               (have_monitor ? narrow_display_string(monitor.DeviceString) + " " +
                                   narrow_display_string(monitor.DeviceID)
                             : "(no EDID)") +
               "\n";
    }
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count) !=
            ERROR_SUCCESS ||
        path_count == 0) {
        out += "  qdc: query unavailable\n";
        return out;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &path_count, paths.data(), &mode_count,
                           modes.data(), nullptr) != ERROR_SUCCESS) {
        out += "  qdc: query failed\n";
        return out;
    }
    int active_paths = 0;
    std::set<std::string> inactive_named;
    for (UINT32 i = 0; i < path_count; ++i) {
        const DISPLAYCONFIG_PATH_INFO& path = paths[i];
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0) {
            ++active_paths;
            continue;
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS ||
            target.monitorFriendlyDeviceName[0] == L'\0') {
            continue;
        }
        const char* tech =
            path.targetInfo.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL
                ? "internal"
                : "external";
        inactive_named.insert("  qdc inactive " + std::string(tech) + " " +
                              narrow_display_string(target.monitorFriendlyDeviceName));
    }
    out += "  qdc active paths: " + std::to_string(active_paths) + "\n";
    for (const std::string& line : inactive_named) {
        out += line + "\n";
    }
    return out;
}

bool restore_pinned_layout(const std::vector<DisplayModeSnapshot>& snapshots, VddClient& vdd,
                           std::string& error) {
    if (snapshots.empty()) {
        error = "no display snapshots to restore";
        return false;
    }
    const DisplayModeSnapshot* primary = nullptr;
    for (const DisplayModeSnapshot& snapshot : snapshots) {
        if (snapshot.primary) {
            primary = &snapshot;
            break;
        }
    }
    if (primary == nullptr) {
        error = "no primary display snapshot to restore";
        return false;
    }
    const auto layout_of = [](const DisplayModeSnapshot& snapshot, int& x, int& y, int& w,
                              int& h) {
        x = static_cast<int>(snapshot.mode.dmPosition.x);
        y = static_cast<int>(snapshot.mode.dmPosition.y);
        w = static_cast<int>(snapshot.mode.dmPelsWidth);
        h = static_cast<int>(snapshot.mode.dmPelsHeight);
    };
    // VDDs out first: Windows recalls the pre-takeover arrangement for the
    // surviving set once the virtual displays are gone. The pins below then
    // converge whatever is left (usually nothing moves: the fast paths skip).
    vdd.disconnect();
    const bool drained = wait_for_no_virtual_displays(15000);
    const auto flush_pending = []() { commit_display_changes(); };
    // R1a: attach the primary snapshot when detached. Staging any mode
    // re-attaches it at its last position; the move home is R1b's (a commit
    // cannot move and re-primary a display it just attached). Names renumber
    // across churn, so each phase resolves through the stable monitor id.
    const std::wstring primary_r1a = resolve_live_device_name(
        live_device_identities(), primary->device_name, primary->monitor_id);
    if (!is_display_attached(primary_r1a)) {
        DEVMODEW mode = primary->mode;
        const LONG stage_result =
            stage_restore_mode(primary_r1a, mode, CDS_UPDATEREGISTRY | CDS_NORESET);
        if (stage_result != DISP_CHANGE_SUCCESSFUL) {
            flush_pending();
            error = "Windows rejected the primary display attach mode (code " +
                    std::to_string(stage_result) + ")";
            return false;
        }
        if (commit_display_changes() != DISP_CHANGE_SUCCESSFUL) {
            error = "Windows failed to re-attach the primary display";
            return false;
        }
        if (!wait_for_attach_state(primary_r1a, true)) {
            error = "the laptop display did not re-attach during the topology restore";
            return false;
        }
    }
    // R1b: the primary snapshot home, alone. Fresh map: R1a's attach commit
    // above is exactly the kind of churn that renumbers names.
    const std::wstring primary_r1b = resolve_live_device_name(
        live_device_identities(), primary->device_name, primary->monitor_id);
    {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        layout_of(*primary, x, y, w, h);
        if (!display_layout_matches(primary_r1b, x, y, w, h, true, true)) {
            DEVMODEW mode = primary->mode;
            const LONG stage_result = stage_restore_mode(
                primary_r1b, mode, CDS_UPDATEREGISTRY | CDS_NORESET | CDS_SET_PRIMARY);
            if (stage_result != DISP_CHANGE_SUCCESSFUL) {
                flush_pending();
                error = "Windows rejected the primary display home mode (code " +
                        std::to_string(stage_result) + ")";
                return false;
            }
            if (commit_display_changes() != DISP_CHANGE_SUCCESSFUL) {
                error = "Windows failed to restore the primary display";
                return false;
            }
            if (!wait_for_display_layout(primary_r1b, x, y, w, h, true, true)) {
                error = "the primary display did not return to its position";
                return false;
            }
        }
    }
    // R2: pin every other snapshot (never a VDD: anything still attached here
    // is stale and belongs to someone else).
    const std::set<std::wstring> virtual_devices = virtual_device_names();
    const auto live_r2 = live_device_identities();
    for (const DisplayModeSnapshot& snapshot : snapshots) {
        if (snapshot.primary) {
            continue;
        }
        const std::wstring live = resolve_live_device_name(live_r2, snapshot.device_name,
                                                           snapshot.monitor_id);
        if (virtual_devices.contains(live)) {
            continue;
        }
        if (!is_display_attached(live)) {
            continue;  // Unplugged since the snapshot; nothing to pin.
        }
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        layout_of(snapshot, x, y, w, h);
        if (display_layout_matches(live, x, y, w, h, true, false)) {
            continue;
        }
        DEVMODEW mode = snapshot.mode;
        const LONG stage_result =
            stage_restore_mode(live, mode, CDS_UPDATEREGISTRY | CDS_NORESET);
        if (stage_result != DISP_CHANGE_SUCCESSFUL) {
            flush_pending();
            error = "Windows rejected a display mode while restoring (code " +
                    std::to_string(stage_result) + ")";
            return false;
        }
    }
    if (commit_display_changes() != DISP_CHANGE_SUCCESSFUL) {
        error = "Windows failed to restore the display positions";
        return false;
    }
    for (const DisplayModeSnapshot& snapshot : snapshots) {
        if (snapshot.primary) {
            continue;
        }
        const std::wstring live = resolve_live_device_name(live_r2, snapshot.device_name,
                                                           snapshot.monitor_id);
        if (virtual_devices.contains(live)) {
            continue;
        }
        if (!is_display_attached(live)) {
            continue;
        }
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        layout_of(snapshot, x, y, w, h);
        if (!wait_for_display_layout(live, x, y, w, h, true, false)) {
            error = "a display did not return to its position";
            return false;
        }
    }
    if (!drained) {
        error = "some virtual displays are still attached";
        return false;
    }
    return true;
}

int query_taskbar_state() {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    return static_cast<int>(SHAppBarMessage(ABM_GETSTATE, &data));
}

namespace {

// Re-applies a taskbar state captured at takeover. Best-effort, never fatal:
// the taskbar must not fail a display restore. Explorer recreates the tray
// asynchronously after display churn (the minimize waves prove its reactions
// lag display events by seconds), so a single set-and-check can pass and
// then flip: require the wanted state to hold steady for 3 s, re-setting on
// every deviation, within a 15 s budget. Returns true when a set was needed
// and the final state matches.
int masked_taskbar_state() {
    return query_taskbar_state() & (ABS_AUTOHIDE | ABS_ALWAYSONTOP);
}

bool reapply_taskbar_state(int saved_state) {
    const int want = saved_state & (ABS_AUTOHIDE | ABS_ALWAYSONTOP);
    bool applied = false;
    int steady_ms = 0;
    for (int elapsed = 0; elapsed < 15000; elapsed += 500) {
        if (masked_taskbar_state() == want) {
            steady_ms += 500;
            if (steady_ms >= 3000) {
                return applied;
            }
        } else {
            steady_ms = 0;
            APPBARDATA data{};
            data.cbSize = sizeof(data);
            data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
            data.lParam = want;
            SHAppBarMessage(ABM_SETSTATE, &data);
            applied = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return applied && masked_taskbar_state() == want;
}

void refresh_saved_taskbar_state(WorkspaceTopology& topo) {
    if (topo.taskbar_state < 0) {
        return;
    }
    topo.taskbar_before = query_taskbar_state();
    topo.taskbar_reapplied = reapply_taskbar_state(topo.taskbar_state);
    topo.taskbar_after = query_taskbar_state();
}

}  // namespace

bool restore_display_topology(WorkspaceTopology& topo, VddClient& vdd, std::string& error) {
    if (!topo.active) {
        return true;
    }
    // Fresh pre-wave iconic state: a takeover-era snapshot would be stale by
    // the whole session, and unminimizing a window the user minimized mid-run
    // would fight them.
    std::set<HWND> was_iconic;
    snapshot_iconic_windows(was_iconic);
    if (!restore_pinned_layout(topo.snapshots, vdd, error)) {
        // Pins failed, but the user's taskbar preference is still ours to
        // hand back: the churn already happened either way.
        refresh_saved_taskbar_state(topo);
        return false;
    }
    topo.windows_restored = restore_windows_home(topo.migrated_windows);
    // The restore leg's unplug wave re-minimizes after the move-back (stale
    // center-association, one wave per vanishing device): unminimize until
    // quiet, then hand focus back where the takeover found it.
    topo.windows_repaired = 0;
    const DisplayModeSnapshot* primary = nullptr;
    for (const DisplayModeSnapshot& snapshot : topo.snapshots) {
        if (snapshot.primary) {
            primary = &snapshot;
            break;
        }
    }
    if (primary != nullptr) {
        RECT laptop_home{};
        laptop_home.left = primary->mode.dmPosition.x;
        laptop_home.top = primary->mode.dmPosition.y;
        laptop_home.right = laptop_home.left + static_cast<LONG>(primary->mode.dmPelsWidth);
        laptop_home.bottom = laptop_home.top + static_cast<LONG>(primary->mode.dmPelsHeight);
        topo.windows_repaired =
            repair_restore_wave(topo.migrated_windows, was_iconic, laptop_home, 10000);
    }
    if (topo.focus_window != nullptr) {
        SetForegroundWindow(topo.focus_window);
    }
    refresh_saved_taskbar_state(topo);
    topo.active = false;
    return true;
}

}  // namespace gt
