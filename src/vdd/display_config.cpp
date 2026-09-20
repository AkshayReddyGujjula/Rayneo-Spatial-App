#include "vdd/display_config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <map>
#include <set>
#include <thread>

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

bool primary_mode(DEVMODEW& mode, std::string& error) {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index) {
        if ((device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0) {
            mode = DEVMODEW{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) {
                return true;
            }
            break;
        }
        device = DISPLAY_DEVICEW{};
        device.cb = sizeof(device);
    }
    error = "could not read the primary display mode";
    return false;
}

bool restore_modes(const std::vector<std::pair<std::wstring, DEVMODEW>>& snapshots) {
    bool staged = true;
    for (const auto& [device_name, mode] : snapshots) {
        DEVMODEW restore = mode;
        if (ChangeDisplaySettingsExW(device_name.c_str(), &restore, nullptr,
                                     CDS_UPDATEREGISTRY | CDS_NORESET,
                                     nullptr) != DISP_CHANGE_SUCCESSFUL) {
            staged = false;
        }
    }
    return staged &&
           ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) ==
               DISP_CHANGE_SUCCESSFUL;
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
                    by_index[driver_index] = std::move(info);
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

bool configure_virtual_displays(const std::vector<int>& driver_indices, int width, int height,
                                int preferred_hz, std::vector<ConfiguredDisplay>& configured,
                                std::string& error) {
    const LONG topology_result =
        SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
    if (topology_result != ERROR_SUCCESS) {
        error = "Windows could not activate the extended display topology (code " +
                std::to_string(topology_result) + ")";
        return false;
    }

    std::vector<VirtualDisplayInfo> displays;
    if (!wait_for_virtual_displays(driver_indices, 5000, displays, error)) {
        return false;
    }
    std::map<int, VirtualDisplayInfo> by_index;
    for (VirtualDisplayInfo& display : displays) {
        by_index.emplace(display.driver_index, std::move(display));
    }

    DEVMODEW primary{};
    if (!primary_mode(primary, error)) {
        return false;
    }
    int next_x = primary.dmPosition.x + static_cast<int>(primary.dmPelsWidth);
    const int y = primary.dmPosition.y;
    struct PlannedMode {
        int driver_index;
        std::wstring device_name;
        int refresh_hz;
        int x;
    };
    std::vector<PlannedMode> plans;
    plans.reserve(driver_indices.size());
    std::vector<std::pair<std::wstring, DEVMODEW>> snapshots;
    snapshots.reserve(driver_indices.size());

    for (int driver_index : driver_indices) {
        const auto found = by_index.find(driver_index);
        if (found == by_index.end() || found->second.device_name.empty()) {
            error = "a Parsec virtual display has no Windows display name";
            return false;
        }
        DEVMODEW previous{};
        previous.dmSize = sizeof(previous);
        if (!EnumDisplaySettingsExW(found->second.device_name.c_str(), ENUM_CURRENT_SETTINGS,
                                    &previous, 0)) {
            error = "could not snapshot a virtual display before configuration";
            return false;
        }
        snapshots.emplace_back(found->second.device_name, previous);
        const auto rates = refresh_rates(found->second.device_name, width, height);
        const int refresh = choose_refresh_rate(rates, preferred_hz);
        if (refresh == 0) {
            error = "the Parsec VDD does not expose the requested display mode";
            return false;
        }
        plans.push_back(PlannedMode{driver_index, found->second.device_name, refresh, next_x});
        next_x += width;
    }

    std::vector<ConfiguredDisplay> candidate;
    candidate.reserve(plans.size());
    for (const PlannedMode& plan : plans) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        mode.dmPelsWidth = static_cast<DWORD>(width);
        mode.dmPelsHeight = static_cast<DWORD>(height);
        mode.dmDisplayFrequency = static_cast<DWORD>(plan.refresh_hz);
        mode.dmPosition.x = plan.x;
        mode.dmPosition.y = y;
        mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
        const LONG changed = ChangeDisplaySettingsExW(
            plan.device_name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET,
            nullptr);
        if (changed != DISP_CHANGE_SUCCESSFUL) {
            error = "Windows rejected the virtual display mode (code " + std::to_string(changed) + ")";
            if (!restore_modes(snapshots)) {
                error += "; restoring the previous display modes also failed";
            }
            return false;
        }
        candidate.push_back(ConfiguredDisplay{plan.driver_index, plan.device_name, width, height,
                                              plan.refresh_hz, plan.x, y});
    }
    if (ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) != DISP_CHANGE_SUCCESSFUL) {
        error = "Windows failed to commit the virtual display arrangement";
        if (!restore_modes(snapshots)) {
            error += "; restoring the previous display modes also failed";
        }
        return false;
    }
    configured = std::move(candidate);
    return true;
}

}  // namespace gt
