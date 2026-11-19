#include "vdd/display_scale.h"

#include "vdd/display_config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>

namespace gt {
namespace {

// Undocumented DisplayConfig device-info types for the per-monitor scale.
const DISPLAYCONFIG_DEVICE_INFO_TYPE kGetDpiScale = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(0xFFFFFFFDu);
const DISPLAYCONFIG_DEVICE_INFO_TYPE kSetDpiScale = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(0xFFFFFFFCu);

struct GetDpiScaleInfo {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 min_scale_rel;
    INT32 cur_scale_rel;
    INT32 max_scale_rel;
};

struct SetDpiScaleInfo {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 scale_rel;
};

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr,
                            nullptr);
    }
    return out;
}

// The active path's source (adapter LUID + source id) for a GDI device name.
bool find_source(const std::wstring& gdi_name, LUID& adapter, UINT32& source_id, std::string& error) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 4 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (result != ERROR_SUCCESS) {
            break;
        }
        paths.resize(path_count);
        modes.resize(mode_count);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                    nullptr);
        if (result == ERROR_SUCCESS) {
            paths.resize(path_count);
        }
    }
    if (result != ERROR_SUCCESS) {
        error = "QueryDisplayConfig failed (" + std::to_string(result) + ")";
        return false;
    }
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        if (_wcsicmp(source.viewGdiDeviceName, gdi_name.c_str()) == 0) {
            adapter = path.sourceInfo.adapterId;
            source_id = path.sourceInfo.id;
            return true;
        }
    }
    error = "no active display path for " + narrow(gdi_name);
    return false;
}

}  // namespace

int display_scale_step_index(int percent) {
    for (int i = 0; i < kDisplayScaleStepCount; ++i) {
        if (kDisplayScaleSteps[i] == percent) {
            return i;
        }
    }
    return -1;
}

int display_scale_percent_at(const DisplayScaleInfo& info, int rel) {
    const int index = -info.min_rel + rel;
    if (index < 0 || index >= kDisplayScaleStepCount) {
        return 0;
    }
    return kDisplayScaleSteps[index];
}

DisplayScaleTarget plan_display_scale(const DisplayScaleInfo& info, int percent) {
    DisplayScaleTarget target;
    const int index = display_scale_step_index(percent);
    const int recommended = -info.min_rel;
    if (index < 0 || recommended < 0 || recommended >= kDisplayScaleStepCount || info.max_rel < info.min_rel) {
        return target;
    }
    // Windows accepts and reads back a rel above max but clamps the effective
    // DPI (measured: 200% on a 175% display), so never write past max.
    const int lowest = info.min_rel;
    int highest = info.max_rel;
    if (recommended + highest >= kDisplayScaleStepCount) {
        highest = kDisplayScaleStepCount - 1 - recommended;
    }
    int rel = index - recommended;
    if (rel > highest) {
        rel = highest;
        target.clamped = true;
    } else if (rel < lowest) {
        rel = lowest;
        target.clamped = true;
    }
    target.valid = true;
    target.rel = rel;
    target.percent = kDisplayScaleSteps[recommended + rel];
    return target;
}

bool query_display_scale(const std::wstring& gdi_name, DisplayScaleInfo& info, std::string& error) {
    GetDpiScaleInfo request{};
    if (!find_source(gdi_name, request.header.adapterId, request.header.id, error)) {
        return false;
    }
    request.header.type = kGetDpiScale;
    request.header.size = sizeof(request);
    const LONG result = DisplayConfigGetDeviceInfo(&request.header);
    if (result != ERROR_SUCCESS) {
        error = "DisplayConfigGetDeviceInfo(scale) failed (" + std::to_string(result) + ")";
        return false;
    }
    info.min_rel = request.min_scale_rel;
    info.cur_rel = request.cur_scale_rel;
    info.max_rel = request.max_scale_rel;
    return true;
}

bool set_display_scale_rel(const std::wstring& gdi_name, int rel, std::string& error) {
    SetDpiScaleInfo request{};
    if (!find_source(gdi_name, request.header.adapterId, request.header.id, error)) {
        return false;
    }
    request.header.type = kSetDpiScale;
    request.header.size = sizeof(request);
    request.scale_rel = rel;
    const LONG result = DisplayConfigSetDeviceInfo(&request.header);
    if (result != ERROR_SUCCESS) {
        error = "DisplayConfigSetDeviceInfo(scale) failed (" + std::to_string(result) + ")";
        return false;
    }
    return true;
}

DisplayScaleOps win32_display_scale_ops() {
    DisplayScaleOps ops;
    ops.query = query_display_scale;
    ops.set = set_display_scale_rel;
    ops.identities = live_device_identities;
    ops.virtual_names = [] {
        std::vector<std::wstring> names;
        for (const VirtualDisplayInfo& display : enumerate_virtual_displays()) {
            if (display.active && !display.device_name.empty()) {
                names.push_back(display.device_name);
            }
        }
        return names;
    };
    ops.log = [](const std::string& line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    return ops;
}

DisplayScaleSession::DisplayScaleSession(int target_percent, DisplayScaleOps ops)
    : target_(target_percent), ops_(std::move(ops)) {}

DisplayScaleSession::~DisplayScaleSession() {
    try {
        restore_all();
    } catch (...) {
    }
}

void DisplayScaleSession::log(const std::string& line) const {
    if (ops_.log) {
        ops_.log("display scale: " + line);
    }
}

std::wstring DisplayScaleSession::live_name(const std::wstring& monitor_id) const {
    if (monitor_id.empty()) {
        return {};
    }
    std::wstring name;
    for (const auto& [gdi, id] : ops_.identities()) {
        if (_wcsicmp(id.c_str(), monitor_id.c_str()) == 0) {
            name = gdi;
            break;
        }
    }
    if (name.empty()) {
        return {};
    }
    // Only ever write to an active virtual display (never the glasses or the
    // laptop panel), even if an id somehow resolved elsewhere.
    for (const std::wstring& candidate : ops_.virtual_names()) {
        if (_wcsicmp(candidate.c_str(), name.c_str()) == 0) {
            return name;
        }
    }
    return {};
}

void DisplayScaleSession::apply(const std::vector<std::pair<int, std::wstring>>& displays) {
    if (!enabled()) {
        return;
    }
    const auto identities = ops_.identities();
    for (const auto& [driver_index, gdi_name] : displays) {
        const std::string label = narrow(gdi_name) + " (VDD " + std::to_string(driver_index) + ")";
        std::wstring monitor_id;
        for (const auto& [gdi, id] : identities) {
            if (_wcsicmp(gdi.c_str(), gdi_name.c_str()) == 0) {
                monitor_id = id;
                break;
            }
        }
        // Without a stable id the original could not be restored safely.
        if (monitor_id.empty() || live_name(monitor_id) != gdi_name) {
            log(label + ": skipped, no stable monitor id for an active virtual display");
            continue;
        }
        DisplayScaleInfo info;
        std::string error;
        if (!ops_.query(gdi_name, info, error)) {
            log(label + ": query failed: " + error);
            continue;
        }
        const DisplayScaleTarget target = plan_display_scale(info, target_);
        if (!target.valid) {
            log(label + ": skipped, unsupported scale range (min " + std::to_string(info.min_rel) + " cur " +
                std::to_string(info.cur_rel) + " max " + std::to_string(info.max_rel) + ")");
            continue;
        }
        const int current = display_scale_current_percent(info);
        const std::string current_text = current > 0 ? std::to_string(current) + "%" : "unknown (rel " +
                                                                                           std::to_string(info.cur_rel) + ")";
        bool known = false;
        for (Original& original : originals_) {
            if (original.monitor_id == monitor_id) {
                original.driver_index = driver_index;
                known = true;
                break;
            }
        }
        if (target.clamped) {
            log(label + ": clamped " + std::to_string(target_) + "% to " + std::to_string(target.percent) +
                "% (the most Windows allows on this display)");
        }
        if (info.cur_rel == target.rel) {
            if (!known) {
                originals_.push_back({monitor_id, driver_index, info.cur_rel, current});
            }
            log(label + ": already " + std::to_string(target.percent) + "%");
            continue;
        }
        if (!known) {
            originals_.push_back({monitor_id, driver_index, info.cur_rel, current});
        }
        if (!ops_.set(gdi_name, target.rel, error)) {
            log(label + ": set " + std::to_string(target.percent) + "% failed: " + error);
            continue;
        }
        log(label + ": set " + current_text + " -> " + std::to_string(target.percent) + "% (recommended " +
            std::to_string(display_scale_recommended_percent(info)) + "%, max " +
            std::to_string(display_scale_max_percent(info)) + "%)");
    }
}

bool DisplayScaleSession::restore_one(const Original& original) {
    const std::string what = original.percent > 0 ? std::to_string(original.percent) + "%"
                                                  : "rel " + std::to_string(original.rel);
    const std::wstring name = live_name(original.monitor_id);
    if (name.empty()) {
        log("restore of VDD " + std::to_string(original.driver_index) + " to " + what +
            " skipped: its monitor is no longer an active virtual display");
        return false;
    }
    const std::string label = narrow(name) + " (VDD " + std::to_string(original.driver_index) + ")";
    std::string error;
    if (!ops_.set(name, original.rel, error)) {
        log(label + ": restore to " + what + " failed: " + error);
        return false;
    }
    log(label + ": restored " + what);
    return true;
}

bool DisplayScaleSession::restore_driver(int driver_index) {
    for (size_t i = 0; i < originals_.size(); ++i) {
        if (originals_[i].driver_index == driver_index) {
            const Original original = originals_[i];
            originals_.erase(originals_.begin() + static_cast<std::ptrdiff_t>(i));
            return restore_one(original);
        }
    }
    return false;
}

void DisplayScaleSession::restore_all() {
    std::vector<Original> pending;
    pending.swap(originals_);
    for (const Original& original : pending) {
        restore_one(original);
    }
}

}  // namespace gt
