#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace gt {

class VddClient;

struct VirtualDisplayInfo {
    int driver_index = -1;
    std::wstring device_name;
    std::wstring device_id;
    bool active = false;
    DWORD adapter_flags = 0;
    DWORD monitor_flags = 0;
};

struct ConfiguredDisplay {
    int driver_index = -1;
    std::wstring device_name;
    // Desktop extent as Windows reports it: 1080 x 1920 for a portrait
    // (DMDO_90) desktop, so a rotation change is a mode change for every
    // equality check (capture reuse included).
    int width = 0;
    int height = 0;
    int refresh_hz = 0;
    int x = 0;
    int y = 0;
    // DMDO_DEFAULT (0) or DMDO_90 (1, portrait).
    int orientation = 0;

    bool operator==(const ConfiguredDisplay&) const = default;
};

constexpr int kVirtualDisplayWaitMs = 5000;
int parse_vdd_driver_index(const std::wstring& device_id);
int choose_refresh_rate(const std::vector<int>& available, int preferred);
std::vector<VirtualDisplayInfo> enumerate_virtual_displays();
bool wait_for_virtual_displays(const std::vector<int>& driver_indices, int timeout_ms,
                               const char* phase, std::vector<VirtualDisplayInfo>& displays,
                               std::string& error);
std::map<int, std::wstring> active_virtual_display_names(
    const std::vector<VirtualDisplayInfo>& displays);

// Pure workspace-topology planning (unit-tested in vdd_selftest; no Win32 calls).

// One bound screen as the desktop planner sees it: its direction and angular
// size (width_deg/height_deg = 2*atan(size/2 / distance)). The defaults are
// the default 1.7 x 0.956 m screen at 2 m, so {driver, yaw} is a level screen.
struct ScreenPlacement {
    int driver_index = -1;
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;  // positive = above the horizon
    float width_deg = 46.0f;
    float height_deg = 26.9f;
    // Portrait desktop: the landscape base mode rotated 90 degrees (Windows
    // "Portrait", DMDO_90), so the desktop is base-height wide, base-width tall.
    bool portrait = false;
};

// Picks the primary desktop: the most level screen of the desktop column whose
// base is closest to straight ahead (|yaw|, then |pitch|, then lowest driver
// index), so a screen stacked above the centre never becomes primary.
// Empty input -> -1.
int select_center_driver(const std::vector<ScreenPlacement>& screens);

struct PlannedDesktop {
    int driver_index = -1;
    int x = 0;
    int y = 0;
    // The desktop's own extent (1080 x 1920 when portrait).
    int width = 1920;
    int height = 1080;
    bool portrait = false;
    bool operator==(const PlannedDesktop&) const = default;
};

// Desktop positions that mirror the 3D arrangement, so the mouse crosses to the
// neighbour you see: screens are grouped into columns by yaw (within half a
// screen width and vertically separated), columns are laid out left to right
// by yaw with the center's column at x = 0, and within a column screens stack
// by pitch (higher pitch -> negative y, i.e. above). Every column's base row
// (the center in its column, else the most level screen) sits at y = 0.
// Single-row layouts get exactly the yaw-ordered row: -width, 0, +width, ...
// Input order is preserved; only positions are assigned.
//
// Per-display sizes: `width` x `height` is the landscape base mode; a portrait
// placement gets the rotated extent (height x width). A column is as wide as
// its widest member and members are centred on its base screen; columns abut
// by those widths. Rows stack by cumulative heights. The base screens are
// vertically centred on the centre desktop's centre line, so a portrait
// screen beside a landscape centre sits at y = (1080 - 1920) / 2 = -420.
// The centre desktop stays at (0, 0) whatever its orientation.
std::vector<PlannedDesktop> plan_workspace_desktops(const std::vector<ScreenPlacement>& screens,
                                                    int center_driver, int width,
                                                    int height = 1080);
// True when the desktops tile seamlessly (pure, unit-tested): no two overlap
// in area, and every desktop is reachable from every other across shared
// edges of non-zero length. Windows silently drops an overlapping batch and
// packs a gapped display somewhere else, so a plan failing this check would
// roll the takeover back (or land the mouse in the wrong place).
bool desktops_tile_seamlessly(const std::vector<PlannedDesktop>& desktops, std::string& reason);
// The DEVMODE a planned virtual desktop is staged with (pure, unit-tested):
// its extent, position and refresh rate, and ALWAYS an explicit orientation
// (DMDO_90 for portrait, DMDO_DEFAULT otherwise) because Windows persists a
// monitor's rotation across sessions: a VDD rotated in an earlier session
// would otherwise come back rotated.
DEVMODEW build_virtual_desktop_mode(const PlannedDesktop& desktop, int refresh_hz);
// The centre (primary) desktop's rect (pure, unit-tested): the configured
// display at the origin, sized by its own extent (1080 x 1920 when portrait).
// Falls back to a 1920 x 1080 rect at the origin when none is configured.
RECT center_desktop_rect(const std::vector<ConfiguredDisplay>& displays);

// Picks the laptop panel to detach: (GDI name, is_internal, is_primary).
// Prefers the primary internal panel, else the first internal one; empty when
// there is no internal panel (desktop PCs keep every display).
std::wstring select_internal_display(
    const std::vector<std::tuple<std::wstring, bool, bool>>& displays);
// True when a monitor description/device-id pair identifies the RayNeo
// glasses (pure, unit-tested): branded names match anywhere; an unbranded
// TCL panel only matches as a secondary display.
bool is_glasses_display(const std::wstring& description, const std::wstring& device_id,
                        bool primary);
// True when a CCD target friendly name identifies the glasses (pure,
// unit-tested): the GDI EDID can be lost while the CCD target persists, so
// reactivation matches on this looser name and verifies strictly after.
bool is_glasses_friendly_name(const std::wstring& friendly_name);

// Live topology takeover and restore.
//
// Measured ChangeDisplaySettingsEx rules (see docs/PROTOCOL-NOTES.md): every
// stage call validates the pending batch, so the new primary (at the origin)
// must be staged FIRST; batches with overlaps or without a primary at (0,0)
// are silently dropped (rc=0, nothing applied). A NULL-mode detach is ignored
// on this machine, staged or immediate, so the detach goes through CCD
// (SetDisplayConfig deactivating exactly the internal path, never a canned
// topology). Every phase below is verified against live state.

struct DisplayModeSnapshot {
    std::wstring device_name;
    // Stable per-monitor identity (the monitor DeviceID, e.g. MONITOR\... or
    // \\?\DISPLAY#...): GDI device names renumber across topology churn, so
    // pins resolve through this and fall back to the name when it is empty.
    std::wstring monitor_id;
    DEVMODEW mode{};
    bool primary = false;
};

// Window migration across the detach. The OS migrates laptop windows to the
// wrong screen (the glasses, behind the fullscreen renderer), so the takeover
// places them on the center desktop itself, before the detach, and the
// restore puts them back.
struct MigratedWindow {
    HWND hwnd = nullptr;
    RECT home{};
    bool zoomed = false;
};

struct WorkspaceTopology {
    std::vector<DisplayModeSnapshot> snapshots;
    std::wstring detached_device;
    std::vector<MigratedWindow> migrated_windows;
    HWND focus_window = nullptr;
    int windows_placed = 0;
    int windows_restored = 0;
    // Unminimize repairs from the most recent phase (the detach repair, then
    // the restore repair overwrites it).
    int windows_repaired = 0;
    // Taskbar auto-hide state (ABM_GETSTATE bits) captured at takeover:
    // display churn makes Explorer recreate the taskbar and drop the user's
    // auto-hide preference, so the restore hands it back. -1 = not captured.
    int taskbar_state = -1;
    // Set when the restore actually re-applied the saved taskbar state.
    bool taskbar_reapplied = false;
    // Live state around the restore's re-apply (-1 = not observed): the
    // engine logs captured/before/after so a missed restore is diagnosable.
    int taskbar_before = -1;
    int taskbar_after = -1;
    bool active = false;
};
// Reads the Explorer taskbar state bits (ABM_GETSTATE: ABS_AUTOHIDE and/or
// ABS_ALWAYSONTOP). Read-only; safe to call anywhere.
int query_taskbar_state();

// Pure rect mapping (unit-tested): preserves the window's offset within
// `from` onto `to`, clamped inside `to` (oversize windows pin at its origin).
RECT migrate_window_rect(const RECT& window, const RECT& from, const RECT& to);
// Records visible, non-minimized top-level windows intersecting `laptop_rect`
// (desktop coords). Shell, tool, and transient windows are excluded.
void snapshot_laptop_windows(const RECT& laptop_rect, std::vector<MigratedWindow>& out);
// Moves recorded windows (alive ones) onto `center_rect`, mapped from their
// position within `from_rect`. Returns the count moved.
int place_windows_on_center(const std::vector<MigratedWindow>& windows, const RECT& from_rect,
                            const RECT& center_rect);
// Moves recorded windows (alive ones) back to their home rects. Returns the
// count moved.
int restore_windows_home(const std::vector<MigratedWindow>& windows);
// Records every currently-minimized top-level window (the exclusion list for
// the restore repair below).
void snapshot_iconic_windows(std::set<HWND>& out);
// Unminimizes recorded windows the detach minimized (Windows 11 hides windows
// by stale display-association when a monitor vanishes) and re-places them on
// the center desktop, mapped from their laptop homes; loops until quiet or
// timeout_ms because the wave lands async (an Explorer-side reaction, not part
// of the detach call). Returns the count of distinct windows repaired.
// Non-minimized windows are already placed and are skipped.
int repair_takeover_wave(const std::vector<MigratedWindow>& windows, const RECT& laptop_home,
                         const RECT& center_rect, int timeout_ms);
// Unminimizes windows the restore leg's unplug wave hid, until quiet or
// timeout_ms: recorded windows go home, any other newly-minimized window is
// restored in place and clamped onto `laptop_home` when orphaned off-screen.
// Windows in `was_iconic` (snapshotted fresh at restore entry, before the
// wave) are left alone. Returns the count of distinct windows repaired.
int repair_restore_wave(const std::vector<MigratedWindow>& windows,
                        const std::set<HWND>& was_iconic, const RECT& laptop_home,
                        int timeout_ms);

bool snapshot_attached_displays(std::vector<DisplayModeSnapshot>& snapshots, std::string& error);
bool find_internal_display(std::wstring& device_name, std::string& error);
// Mid-session guard: Windows re-attaches the laptop panel the takeover
// detached whenever the monitor set changes (a virtual display added or
// removed by a layout reload; measured live 2026-11-18 on grow and shrink).
// When an internal panel is active again, the windows on it move to
// `center_rect`, the panel is detached again (not persisted), and windows the
// detach minimized are repaired. Newly recorded windows are appended to
// `migrated` (deduplicated by HWND) with homes mapped onto `pristine_laptop`,
// so the shutdown restore returns them. Returns true when it re-detached;
// false with an empty error when no internal panel is active, false with
// `error` set when the re-detach failed (the caller logs it and carries on).
// A failed detach that leaves the panel attached moves the windows back onto
// it (nothing is recorded: they are home on a visible screen); one that lands
// despite its error is treated as a success, so no moved window goes
// unrecorded.
// `panel_name` is the live GDI name of the panel the takeover detached
// (resolved by monitor id; only that panel is ever touched).
bool redetach_internal_display(const std::wstring& panel_name, const RECT& pristine_laptop,
                               const RECT& center_rect, std::vector<MigratedWindow>& migrated,
                               std::string& error);
// The Win32 steps of the guard above, injectable for the selftest.
struct RedetachOps {
    // Sets an empty name when no internal panel is active.
    std::function<bool(std::wstring&, std::string&)> find_panel;
    std::function<bool(const std::wstring&, RECT&)> read_rect;
    std::function<void(const RECT&, std::vector<MigratedWindow>&)> snapshot_windows;
    std::function<void(const std::vector<MigratedWindow>&, const RECT& from, const RECT& to)>
        place_windows;
    // Moves each window back to its recorded `home` rect.
    std::function<void(const std::vector<MigratedWindow>&)> restore_home;
    std::function<bool(const std::wstring&, std::string&)> detach;
    // True once the panel reports detached (bounded wait).
    std::function<bool(const std::wstring&)> wait_detached;
    std::function<void(const std::vector<MigratedWindow>&)> repair;
};
bool redetach_internal_display(const RedetachOps& ops, const RECT& pristine_laptop,
                               const RECT& center_rect, std::vector<MigratedWindow>& migrated,
                               std::string& error);
// True when the named GDI device is currently attached to the desktop.
// Used to verify a detach or a re-attach actually landed; an unknown name
// reports false.
bool is_display_attached(const std::wstring& device_name);
// Finds a physically-connected but detached (Windows "Disconnect this
// display") glasses display by its monitor identity: detached adapters keep
// their EDID, so the glasses are identifiable even though no active monitor
// matches. Read-only; false when none exists. VDD ghosts never match.
bool find_detached_glasses_display(std::wstring& device_name);
// Re-attaches the display found above as an extended (never primary)
// monitor, in two phases: attach first at an overlap-free staged slot
// (an overlapping slot would be auto-relocated by Windows), then a
// best-effort move back to the stored registry position when it is known
// and free. Verified attached before returning; false with a reason
// otherwise. The contract is attached, not placed.
bool reattach_detached_glasses(std::string& error);
// One-line-per-adapter diagnostic of the live display landscape (GDI
// adapters with monitor identity, plus named inactive QDC paths):
// distinguishes a physical link flap (nothing enumerates) from a
// deactivation (adapter and/or path present but inactive). Read-only.
std::string describe_display_landscape();
// Re-activates an inactive CCD display path for the glasses (the deeper
// disconnect: GDI loses the EDID entirely while the target persists, as a
// recalled "Disconnect this display" does mid-takeover). The path's stored
// modes win when valid, else the target's native preferred mode at a free
// slot (with a stored-then-preferred retry); the fixed topology is saved to
// the database (the poisoned row is overwritten). Verified against the
// activated endpoint with the strict GDI matcher before returning, and
// rolled back (saved) when it is not the glasses. False with a reason when
// no inactive glasses path exists or activation fails.
bool reactivate_glasses_path(std::string& error);
// Waits until no Parsec virtual display enumerates (driver teardown is
// asynchronous, ~10 s observed). Used before connecting (stale displays from
// a previous session reuse UIDs) and after disconnecting. Timeout is an
// honest false, never fatal by itself.
bool wait_for_no_virtual_displays(int timeout_ms);

// Pure aside planning (unit-tested in vdd_selftest; no Win32 calls).

struct PlannedAside {
    std::wstring device_name;
    int x = 0;
    int y = 0;
    bool operator==(const PlannedAside&) const = default;
};

bool display_rects_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh);

// Aside targets for the takeover commit: every attached non-VDD display is
// moved out of the VDD slots, stacked right of everything at y=0. The current
// primary is always moved (it must vacate the origin); other displays move
// only when their snapshot rect intersects a slot. VDD devices (by GDI name)
// are never moved. Deterministic: primary first, then snapshot order.
// Slot rects use each slot's own width/height (portrait slots are tall).
std::vector<PlannedAside> plan_aside_moves(
    const std::vector<DisplayModeSnapshot>& snapshots,
    const std::vector<PlannedDesktop>& slots, const std::set<std::wstring>& vdd_devices);

// Full takeover. Snapshots must be taken BEFORE connecting the VDDs (a truly
// pristine arrangement; connecting reshuffles). Phases, each verified, any
// failure rolling back through restore_pinned_layout (which disconnects the
// VDDs first): wait for the VDDs; resolve modes; commitA (center VDD at the
// origin as primary first, then the other slots, then the aside moves); CCD
// detach of the internal panel (empty detach_device skips it, for desktop
// PCs); re-pin the slots if the detach mangled them. The device names come
// from the post-wait active set, so pre-attach ghosts can never leak into the
// mode query.
// Pre-connect window record, built on the pristine topology and consumed by
// the takeover. Recording must happen before vdd.connect: connecting shoves
// the laptop aside (remembered VDD positions claim the origin), so a
// post-connect record against the pristine home rect matches nothing.
struct MigrationSeed {
    std::vector<MigratedWindow> windows;
    RECT laptop_home{};
    bool have_laptop_home = false;
};

// `width` x `height` is the landscape base mode the VDD must expose; each
// planned desktop carries its own (possibly rotated) extent and orientation.
bool apply_workspace_topology(const std::vector<DisplayModeSnapshot>& snapshots,
                              const std::vector<PlannedDesktop>& planned, int center_driver,
                              int width, int height, int preferred_hz,
                              const std::wstring& detach_device, const MigrationSeed& seed,
                              VddClient& vdd, WorkspaceTopology& topo,
                              std::vector<ConfiguredDisplay>& configured, std::string& error);
// Repositions already-attached VDDs (layout reload): modes, yaw plan, and the
// center primary follow the new layout. Never touches the internal panel.
// The new center stages first (same primary-first rule as the takeover).
bool reposition_virtual_displays(const std::vector<PlannedDesktop>& planned,
                                 const std::map<int, std::wstring>& device_names, int center_driver,
                                 int width, int height, int preferred_hz,
                                 std::vector<ConfiguredDisplay>& configured, std::string& error);
// Restores a snapshotted arrangement, disconnect-first: the VDDs go away and
// drain (Windows recalls the pre-takeover arrangement for the surviving set),
// then R1a attaches the primary snapshot when detached, R1b moves it home as
// primary, and R2 pins the rest. Every phase is verified against live state;
// phases already satisfied are skipped, never re-committed. A stale pending
// batch is flushed before giving up: NORESET stagings survive a failed call
// and would otherwise poison the next commit.
// `reattach_device` (the takeover's detached panel, snapshot name) is
// re-attached and pinned even when it is not the primary snapshot; other
// non-primary displays that are detached are treated as unplugged.
bool restore_pinned_layout(const std::vector<DisplayModeSnapshot>& snapshots, VddClient& vdd,
                           std::string& error, const std::wstring& reattach_device = {});
// Restores the pre-takeover topology (laptop panel back, original primary).
// No-op unless the topology is active; clears active only on success.
// Live (GDI name, monitor id) pairs for every display device Windows
// enumerates, attached or not (a detached display keeps both).
std::vector<std::pair<std::wstring, std::wstring>> live_device_identities();
// Resolves a snapshot to its live GDI name by stable monitor id (pure,
// unit-tested): the first id match wins (attached entries sort first);
// empty or unmatched ids fall back to the snapshot name.
std::wstring resolve_live_device_name(
    const std::vector<std::pair<std::wstring, std::wstring>>& live_devices,
    const std::wstring& snapshot_name, const std::wstring& snapshot_id);
// Drops the refresh rate from a mode (pure, unit-tested): a snapshot can
// capture a transient/VRR rate the driver will not re-accept explicitly.
void strip_display_frequency(DEVMODEW& mode);
bool restore_display_topology(WorkspaceTopology& topo, VddClient& vdd, std::string& error);

}  // namespace gt
