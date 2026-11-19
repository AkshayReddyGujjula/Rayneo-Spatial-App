#pragma once

// Per-monitor Windows display scale (Settings > Display > Scale) for the
// virtual displays. Windows exposes it only through two undocumented
// DisplayConfig device-info types (measured live, docs/PROTOCOL-NOTES.md):
//   get (-3): {header; int32 minScaleRel; int32 curScaleRel; int32 maxScaleRel}
//   set (-4): {header; int32 scaleRel}
// Values are steps relative to the recommended scale, whose table index is
// -minScaleRel. A rel above maxScaleRel reads back but is silently clamped
// by Windows, so callers clamp first and log it.

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gt {

inline constexpr int kDisplayScaleStepCount = 12;
inline constexpr int kDisplayScaleSteps[kDisplayScaleStepCount] = {100, 125, 150, 175, 200, 225,
                                                                   250, 300, 350, 400, 450, 500};

struct DisplayScaleInfo {
    int min_rel = 0;  // -(recommended index)
    int cur_rel = 0;
    int max_rel = 0;
};

// Index of percent in kDisplayScaleSteps, or -1 when it is not a step.
int display_scale_step_index(int percent);
// Percent at a rel for this display, or 0 when it falls outside the table.
int display_scale_percent_at(const DisplayScaleInfo& info, int rel);
inline int display_scale_current_percent(const DisplayScaleInfo& info) {
    return display_scale_percent_at(info, info.cur_rel);
}
inline int display_scale_recommended_percent(const DisplayScaleInfo& info) {
    return display_scale_percent_at(info, 0);
}
inline int display_scale_max_percent(const DisplayScaleInfo& info) {
    return display_scale_percent_at(info, info.max_rel);
}

struct DisplayScaleTarget {
    bool valid = false;    // false: percent not a step, or the display reports an off-table range
    int rel = 0;           // what to write: always within [min_rel, max_rel]
    int percent = 0;       // the scale that rel means
    bool clamped = false;  // the request was above (or below) what this display allows
};
DisplayScaleTarget plan_display_scale(const DisplayScaleInfo& info, int percent);

// Win32 (GDI device name such as \\.\DISPLAY5, resolved to the active path's
// source). Never called by the unit tests.
bool query_display_scale(const std::wstring& gdi_name, DisplayScaleInfo& info, std::string& error);
bool set_display_scale_rel(const std::wstring& gdi_name, int rel, std::string& error);

struct DisplayScaleOps {
    std::function<bool(const std::wstring&, DisplayScaleInfo&, std::string&)> query;
    std::function<bool(const std::wstring&, int, std::string&)> set;
    // Live (GDI name, monitor id) pairs (live_device_identities).
    std::function<std::vector<std::pair<std::wstring, std::wstring>>()> identities;
    // GDI names of the active virtual displays: the only names ever written.
    std::function<std::vector<std::wstring>()> virtual_names;
    std::function<void(const std::string&)> log;
};
// Real Win32 ops; log prints to stdout (the engine log).
DisplayScaleOps win32_display_scale_ops();

// Applies one scale to the virtual displays and puts each display back to
// exactly what it was. Originals are keyed by the stable monitor id (GDI names
// renumber across display churn, bug 15), recorded on first touch only, and
// dropped once restored so a re-added display records afresh. A name is
// resolved from the id at write time and must be an active virtual display;
// otherwise nothing is written. Every failure is logged, none is fatal.
class DisplayScaleSession {
public:
    DisplayScaleSession(int target_percent, DisplayScaleOps ops);
    ~DisplayScaleSession();
    DisplayScaleSession(const DisplayScaleSession&) = delete;
    DisplayScaleSession& operator=(const DisplayScaleSession&) = delete;

    bool enabled() const { return target_ > 0; }
    // Scales each named virtual display; driver_index is kept for remove_driver.
    void apply(const std::vector<std::pair<int, std::wstring>>& displays);
    // Restores the display recorded for this VDD driver index (one about to be removed).
    bool restore_driver(int driver_index);
    // Restores every recorded display. Idempotent.
    void restore_all();
    size_t recorded() const { return originals_.size(); }

private:
    struct Original {
        std::wstring monitor_id;
        int driver_index = -1;
        int rel = 0;
        int percent = 0;
    };
    std::wstring live_name(const std::wstring& monitor_id) const;
    bool restore_one(const Original& original);
    void log(const std::string& line) const;

    int target_ = 0;
    DisplayScaleOps ops_;
    std::vector<Original> originals_;
};

}  // namespace gt
