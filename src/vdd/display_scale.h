#pragma once

// Per-monitor Windows display scale (Settings > Display > Scale) for the
// virtual displays. Windows exposes it only through two undocumented
// DisplayConfig device-info types (measured live, docs/PROTOCOL-NOTES.md):
//   get (-3): {header; int32 minScaleRel; int32 curScaleRel; int32 maxScaleRel}
//   set (-4): {header; int32 scaleRel}
// Values are steps relative to the recommended scale, whose table index is
// -minScaleRel. A rel above maxScaleRel reads back but is silently clamped
// by Windows, so callers clamp first and log it.

#include <filesystem>
#include <functional>
#include <map>
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
    // Fresh driver index -> GDI name of the active virtual displays
    // (active_virtual_display_names): the only names ever written.
    std::function<std::map<int, std::wstring>()> virtual_displays;
    std::function<void(const std::string&)> log;
};
// Real Win32 ops; log prints to stdout (the engine log).
DisplayScaleOps win32_display_scale_ops();

// One display's scale before this program touched it.
struct DisplayScaleSaved {
    int rel = 0;
    int percent = 0;  // 0 when off-table (informational only; rel is what is written)
    bool operator==(const DisplayScaleSaved&) const = default;
};
using DisplayScaleBackup = std::map<std::wstring, DisplayScaleSaved>;  // keyed by monitor id

// Crash-safe copy of the originals. load: a missing file is an empty backup.
// save: an empty backup deletes the file. Either may be left empty (no
// persistence, the unit tests' default).
struct DisplayScaleBackupStore {
    std::function<bool(DisplayScaleBackup&, std::string&)> load;
    std::function<bool(const DisplayScaleBackup&, std::string&)> save;
};
// JSON: {"<monitor id>": {"rel": R, "percent": P}, ...} (UTF-8, pure, unit-tested).
std::string serialize_display_scale_backup(const DisplayScaleBackup& backup);
bool parse_display_scale_backup(const std::string& text, DisplayScaleBackup& backup, std::string& error);
// File store: replaced atomically (temp file + rename), deleted when empty.
DisplayScaleBackupStore file_display_scale_backup(const std::filesystem::path& path);

// Applies one scale to the virtual displays and puts each display back to
// exactly what it was. Originals are keyed by the stable monitor id (GDI names
// renumber across display churn, bug 15), recorded on first touch only, and
// kept until a restore succeeds (a failed or skipped restore keeps them for a
// later attempt). Before the first write each original is saved to the backup
// store; a backup entry left by an unclean exit (the engine was killed while a
// display was scaled) is the original in place of the current value, and is
// restored even when scaling is now off. Targets are resolved from a fresh
// enumeration (driver index -> live name -> monitor id) on every apply; a
// restore writes only when exactly one active virtual display carries the id.
// Every failure is logged, none is fatal.
class DisplayScaleSession {
public:
    DisplayScaleSession(int target_percent, DisplayScaleOps ops, DisplayScaleBackupStore store = {});
    ~DisplayScaleSession();
    DisplayScaleSession(const DisplayScaleSession&) = delete;
    DisplayScaleSession& operator=(const DisplayScaleSession&) = delete;

    bool enabled() const { return target_ > 0; }
    // Scaling on, or originals from an unclean exit still waiting for their display.
    bool active() const { return enabled() || !backup_.empty(); }
    // Scales the active virtual display of each VDD driver index (scaling
    // off: only puts back displays with a pending backup entry).
    void apply(const std::vector<int>& driver_indices);
    // Restores the display recorded for this VDD driver index (one about to be removed).
    bool restore_driver(int driver_index);
    // Restores every recorded display. Idempotent; failures stay recorded.
    void restore_all();
    size_t recorded() const { return originals_.size(); }
    const DisplayScaleBackup& backup() const { return backup_; }

private:
    struct Original {
        std::wstring monitor_id;
        int driver_index = -1;
        int rel = 0;
        int percent = 0;
    };
    // The one active virtual display carrying this id; empty (logged by the
    // caller from reason) when none or several do.
    std::wstring live_name(const std::wstring& monitor_id, std::string& reason) const;
    bool restore_one(const Original& original);
    void forget(const std::wstring& monitor_id);
    bool persist(const std::wstring& monitor_id, const DisplayScaleSaved& saved);
    void log(const std::string& line) const;

    int target_ = 0;
    DisplayScaleOps ops_;
    DisplayScaleBackupStore store_;
    DisplayScaleBackup backup_;
    std::vector<Original> originals_;
};

}  // namespace gt
