#include "vdd/display_config.h"
#include "vdd/vdd_client.h"
#include "vdd/workspace_reload.h"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

gt::Layout triple_layout() {
    gt::Layout layout;
    layout.screens = {{"left", 0, -45.0f}, {"center", 1, 0.0f}, {"right", 2, 45.0f}};
    return layout;
}

void test_workspace_reload(const char* label, int arrival_ms, int removal_failures) {
    auto previous = triple_layout();
    auto candidate = previous;
    candidate.screens.push_back({"above", 3, 0.0f, 25.0f});
    std::vector<int> indices{0, 1, 2};
    std::vector<gt::ConfiguredDisplay> configured{
        {0, L"left", 1920, 1080, 120, -1920, 0},
        {1, L"center", 1920, 1080, 120, 0, 0},
        {2, L"right", 1920, 1080, 120, 1920, 0},
    };
    const auto previous_displays = configured;
    std::vector<std::string> events;
    std::vector<int> backoff;
    int removal_attempts = 0;
    int reload_wait_ms = -1;
    int added_active_ms = -1;
    int rollback_active_ms = -1;
    const std::string ioctl_error = "VDD IOCTL 0x0022a008 failed (Windows error 170)";
    std::string error, rollback_error;
    const gt::WorkspaceReloadOps ops{
        [&]() -> const std::vector<int>& { return indices; },
        [&](size_t count, std::string& detail) {
            events.push_back("resize " + std::to_string(count));
            if (count == 4) {
                indices.push_back(3);
                return true;
            }
            const bool removed = gt::retry_vdd_removal(
                indices.back(),
                [&](int index, std::string& attempt_error) {
                    check(index == 3, "rollback removes the newly added index");
                    if (++removal_attempts > removal_failures) return true;
                    attempt_error = ioctl_error;
                    return false;
                },
                [&](int ms) { backoff.push_back(ms); }, detail);
            if (removed) indices.pop_back();
            return removed;
        },
        [&](const std::vector<int>& expected, int timeout_ms, const char* phase,
            std::vector<gt::VirtualDisplayInfo>& displays, std::string& detail) {
            events.push_back(phase);
            const std::string_view phase_name = phase;
            if (phase_name == "reload") reload_wait_ms = timeout_ms;
            if (phase_name == "reload rollback settle") {
                check(expected == std::vector<int>{3} && timeout_ms > 0,
                      "rollback waits on the added index before removal");
            } else {
                check(expected == indices, "wait covers every owned index");
            }
            for (int ms = 0; ms <= timeout_ms; ms += 100) {
                displays = {{0, L"left", L"", true}, {1, L"center", L"", true},
                            {2, L"right", L"", true}, {3, L"registry ghost", L"", false}};
                if (indices.size() == 3) return true;
                if (ms >= arrival_ms && phase_name == "reload") {
                    displays.push_back({3, L"above", L"", true});
                    added_active_ms = ms;
                    return true;
                }
                if (removal_failures == 1 && ms == 200 &&
                    phase_name == "reload rollback settle") {
                    displays.push_back({3, L"above", L"", true});
                    rollback_active_ms = ms;
                    return true;
                }
            }
            detail = std::string("timed out waiting for active Parsec virtual displays (") +
                     phase + ")";
            return false;
        },
        [&](const gt::WorkspacePlan& plan, const std::map<int, std::wstring>& names,
            std::vector<gt::ConfiguredDisplay>& displays, std::string& detail) {
            events.push_back("reposition");
            std::vector<gt::ConfiguredDisplay> positioned;
            for (const auto& desktop : plan.desktops) {
                const auto name = names.find(desktop.driver_index);
                if (name == names.end() ||
                    (desktop.driver_index == 3 && name->second != L"above")) {
                    detail = "a planned virtual display has no Windows display name";
                    return false;
                }
                positioned.push_back({desktop.driver_index, name->second, 1920, 1080, 120,
                                      desktop.x, desktop.y});
            }
            displays = std::move(positioned);
            return true;
        },
        [&](const auto& displays, std::string&) {
            events.push_back("capture apply");
            check(displays.size() == 4, "candidate binds four captures");
            return true;
        },
        [&](const auto& displays, std::string&) {
            events.push_back("capture restore");
            check(displays == previous_displays, "rollback binds the original displays");
            return true;
        },
    };

    const bool reloaded = gt::reload_workspace(previous, candidate, configured, ops, error,
                                               rollback_error);
    std::printf("  %s: result=%s configured=%zu owned=%zu arrival_ms=%d wait_ms=%d "
                "settle_arrival_ms=%d remove_attempts=%d backoff_ms=[",
                label, reloaded ? "applied" : (rollback_error.empty() ? "ignored" : "fatal"),
                configured.size(), indices.size(), added_active_ms, reload_wait_ms,
                rollback_active_ms, removal_attempts);
    for (size_t i = 0; i < backoff.size(); ++i) {
        std::printf("%s%d", i == 0 ? "" : ",", backoff[i]);
    }
    std::printf("] reason='%s' rollback='%s'\n", error.c_str(), rollback_error.c_str());
    if (arrival_ms == 300) {
        check(reloaded && configured.size() == 4 && indices.size() == 4 &&
                  added_active_ms == 300 && removal_attempts == 0 &&
                  events == std::vector<std::string>{
                      "resize 4", "reload", "reposition", "capture apply"},
              "late active display must be awaited before reposition");
    } else if (removal_failures < 3) {
        check(!reloaded && rollback_error.empty() && configured == previous_displays &&
                  indices == std::vector<int>{0, 1, 2} && error.starts_with("timed out") &&
                  removal_attempts == removal_failures + 1 &&
                  rollback_active_ms == (removal_failures == 0 ? -1 : 200) &&
                  backoff == (removal_failures == 0 ? std::vector<int>{} : std::vector<int>{250}) &&
                  events == std::vector<std::string>{"resize 4", "reload",
                      "reload rollback settle", "resize 3", "reload rollback", "reposition",
                      "capture restore"},
              "missing display rejects the reload and rebinds the restored workspace");
    } else {
        check(!reloaded && configured == previous_displays && indices.size() == 4 &&
                  rollback_error == "virtual display rollback failed: failed to remove Parsec "
                                    "virtual display 3: " + ioctl_error &&
                  removal_attempts == 3 && backoff == std::vector<int>{250, 750} &&
                  events == std::vector<std::string>{"resize 4", "reload",
                      "reload rollback settle", "resize 3"},
              "exhausted removal retries are fatal and retain the IOCTL error");
    }
}

void test_stacked_screen_binding() {
    const auto triple = triple_layout();
    auto stacked = triple;
    stacked.screens.insert(stacked.screens.begin(), {"above", 3, 0.0f, 25.0f});
    gt::WorkspacePlan triple_plan, stacked_plan;
    std::string error;
    const std::vector<int> driver_indices{2, 1, 0, 3};
    if (!gt::plan_workspace_for_layout(triple, {2, 1, 0}, triple_plan, error) ||
        !gt::plan_workspace_for_layout(stacked, driver_indices, stacked_plan, error)) {
        check(false, "stacked-screen planner returns a plan");
        return;
    }
    auto configure = [](const gt::WorkspacePlan& plan) {
        std::vector<gt::ConfiguredDisplay> displays;
        for (const auto& desktop : plan.desktops) {
            displays.push_back({desktop.driver_index, L"DISPLAY" + std::to_wstring(desktop.driver_index),
                                1920, 1080, 120, desktop.x, desktop.y});
        }
        return displays;
    };
    const auto before = gt::bind_displays_to_screens(triple, configure(triple_plan));
    const auto after = gt::bind_displays_to_screens(stacked, configure(stacked_plan));
    std::vector<int> positions, paired_indices;
    for (const auto& display : after) {
        positions.push_back(display.x);
        paired_indices.push_back(display.driver_index);
    }
    std::printf("  stacked above/left/center/right: center driver=%d, driver@x=",
                stacked_plan.center_driver);
    for (const auto& display : after) std::printf(" %d@%d", display.driver_index, display.x);
    std::printf("\n");
    check(stacked_plan.center_driver == 1 && paired_indices == std::vector<int>{3, 2, 1, 0} &&
              positions == std::vector<int>{1920, -1920, 0, 3840},
          "equal-yaw screens keep their binding and get four non-overlapping x slots");

    const auto sources = gt::reusable_capture_sources(before, after);
    const auto kept = std::count_if(sources.begin(), sources.end(),
                                   [](const auto& source) { return source.has_value(); });
    std::printf("  capture reuse: kept=%td new_or_moved=%zu\n", kept,
                after.size() - static_cast<size_t>(kept));
    check(sources == std::vector<std::optional<size_t>>{std::nullopt, 0, 1, std::nullopt},
          "grow reuses unchanged outputs across reorder, replacing the moved and new outputs");

    auto changed = before;
    changed[0].refresh_hz = 60;
    changed[1].device_name = L"renumbered";
    auto unavailable = before;
    unavailable[2] = {};
    const auto geometry_only = gt::reusable_capture_sources(before, before);
    const auto mode_or_name_changed = gt::reusable_capture_sources(before, changed);
    const auto released = gt::reusable_capture_sources(unavailable, before);
    std::printf("  capture reuse old slots (geometry only / mode or name changed / released):");
    for (const auto* map : {&geometry_only, &mode_or_name_changed, &released}) {
        std::printf(" [");
        for (const auto& slot : *map) std::printf(" %d", slot ? static_cast<int>(*slot) : -1);
        std::printf(" ]");
    }
    std::printf("\n");
    check(geometry_only == std::vector<std::optional<size_t>>{0, 1, 2} &&
              mode_or_name_changed == std::vector<std::optional<size_t>>{
                  std::nullopt, std::nullopt, 2} &&
              released == std::vector<std::optional<size_t>>{0, 1, std::nullopt},
          "geometry-only reload keeps every live capture, changed modes and names do not match");
}

}  // namespace

int main() {
    std::printf("vdd_selftest: protocol serialization\n");
    const auto remove = gt::vdd_remove_payload(7);
    check(remove[0] == 0 && remove[1] == 7, "remove index uses the driver's two-byte big-endian form");
    bool remaining_zero = true;
    for (size_t i = 2; i < remove.size(); ++i) remaining_zero = remaining_zero && remove[i] == 0;
    check(remaining_zero, "remove payload padding is zero-filled");

    const std::vector<int> order = gt::vdd_cleanup_order({2, 5, 1});
    check(order == std::vector<int>({1, 5, 2}), "display cleanup reverses addition order");

    const gt::VddDriverStatus status = gt::VddClient::driver_status();
    std::printf("  detected driver status: %s\n", gt::vdd_driver_status_text(status));
    check(status != gt::VddDriverStatus::Inaccessible, "Windows display class is accessible");

    const int taskbar_state = gt::query_taskbar_state();
    std::printf("  detected taskbar state: %d\n", taskbar_state);
    check(taskbar_state >= 0 && taskbar_state <= 3,
          "taskbar state query returns valid ABM_GETSTATE bits");

    std::printf("vdd_selftest: glasses display matching\n");
    check(gt::is_glasses_display(L"Generic PnP Monitor",
                                 LR"(MONITOR\TCL03D4\{4d36e96e-e325-11ce-bfc1-08002be10318}\0003)",
                                 false),
          "unbranded TCL panel matches as a secondary display");
    check(!gt::is_glasses_display(L"Generic PnP Monitor",
                                  LR"(MONITOR\TCL03D4\{4d36e96e-e325-11ce-bfc1-08002be10318}\0003)",
                                  true),
          "TCL panel does not match as the primary display");
    check(gt::is_glasses_display(L"SmartGlasses", L"", true),
          "branded SmartGlasses matches even as primary");
    check(gt::is_glasses_display(L"RayNeo Air 3s", L"", false), "branded RayNeo matches");
    check(gt::is_glasses_display(L"generic pnp monitor", LR"(monitor\tcl1234\0000)", false),
          "glasses matching is case-insensitive");
    check(!gt::is_glasses_display(L"Lenovo DisplayHDR",
                                  LR"(MONITOR\LEN8AC3\{4d36e96e-e325-11ce-bfc1-08002be10318}\0002)",
                                  false),
          "laptop panel does not match as glasses");
    check(!gt::is_glasses_display(L"Parsec Virtual Display",
                                  LR"(\\?\DISPLAY#PSCCDD0#5&abc&UID263#{e6f07b5f-ffff})", false),
          "a VDD never matches as glasses");
    check(!gt::is_glasses_display(L"Generic PnP Monitor",
                                  LR"(MONITOR\DELA123\{4d36e96e-e325-11ce-bfc1-08002be10318}\0000)",
                                  false),
          "an unknown monitor does not match as glasses");
    check(gt::is_glasses_friendly_name(L"SmartGlasses"),
          "CCD friendly name SmartGlasses matches");
    check(gt::is_glasses_friendly_name(L"RayNeo Air 3s"), "CCD friendly name RayNeo matches");
    check(gt::is_glasses_friendly_name(L"TCL 4K TV"), "CCD friendly name TCL matches");
    check(gt::is_glasses_friendly_name(L"smartglasses"),
          "CCD friendly-name matching is case-insensitive");
    check(!gt::is_glasses_friendly_name(L"ParsecVDA"),
          "CCD friendly name ParsecVDA does not match");
    check(!gt::is_glasses_friendly_name(L"Generic PnP Monitor"),
          "a generic CCD friendly name does not match");
    check(!gt::is_glasses_friendly_name(L""),
          "an empty CCD friendly name does not match");
    check(gt::is_glasses_display(L"TCL 55-inch TV", L"", false),
          "TCL in the description alone matches as a secondary display");
    check(gt::is_glasses_display(L"RayNeo Air", L"", true),
          "branded RayNeo matches even as primary");
    check(gt::is_glasses_display(L"Generic PnP Monitor", LR"(MONITOR\RAYNEO1234\0000)", false),
          "a branded device id matches with a generic description");
    check(!gt::is_glasses_display(L"", L"", false),
          "empty description and id never match");
    std::wstring detached_glasses;
    const bool have_detached = gt::find_detached_glasses_display(detached_glasses);
    std::printf("  detached glasses: %ls\n",
                have_detached ? detached_glasses.c_str() : L"(none)");
    check(!have_detached || !gt::is_display_attached(detached_glasses),
          "a found detached display is really detached");
    const std::string landscape = gt::describe_display_landscape();
    std::printf("  landscape bytes: %zu\n", landscape.size());
    check(!landscape.empty() && landscape.find("DISPLAY") != std::string::npos,
          "display landscape dump names live adapters");
    check(landscape.find("qdc active paths:") != std::string::npos,
          "display landscape dump counts active CCD paths");

    std::printf("vdd_selftest: display identity and mode planning\n");
    check(gt::parse_vdd_driver_index(
              LR"(\\?\DISPLAY#PSCCDD0#5&abc&UID263#{e6f07b5f-ffff})") == 7,
          "monitor UID maps back to driver index");
    check(gt::parse_vdd_driver_index(
              LR"(\\?\display#psccdd0#5&abc&uid263#{e6f07b5f-ffff})") == 7,
          "monitor UID parsing is case-insensitive");
    check(gt::choose_refresh_rate({24, 60, 120, 144}, 120) == 120,
          "preferred refresh rate wins when available");
    check(gt::choose_refresh_rate({24, 60, 144}, 120) == 60,
          "refresh selection falls back without exceeding the preference");

    std::printf("vdd_selftest: workspace topology planning\n");
    check(gt::select_center_driver({{0, -45.0f}, {1, 0.0f}, {2, 45.0f}}) == 1,
          "center primary is the screen closest to straight ahead");
    check(gt::select_center_driver({{0, -30.0f}, {1, 30.0f}}) == 0,
          "center ties resolve to the lowest driver index");
    check(gt::select_center_driver({}) == -1, "empty binding has no center");
    {
        const auto planned =
            gt::plan_workspace_desktops({{0, -45.0f}, {1, 0.0f}, {2, 45.0f}}, 1, 1920);
        check(planned.size() == 3 && planned[0].x == -1920 && planned[1].x == 0 &&
                  planned[2].x == 1920,
              "triple arc maps left/center/right around the origin");
        check(planned[0].driver_index == 0 && planned[1].driver_index == 1 &&
                  planned[2].driver_index == 2,
              "planning preserves bind order");
    }
    {
        const auto planned = gt::plan_workspace_desktops(
            {{0, -60.0f}, {1, -30.0f}, {2, 0.0f}, {3, 30.0f}, {4, 60.0f}}, 2, 1920);
        check(planned[0].x == -3840 && planned[1].x == -1920 && planned[2].x == 0 &&
                  planned[3].x == 1920 && planned[4].x == 3840,
              "wider arcs stack outward from the center");
    }
    {
        const auto planned = gt::plan_workspace_desktops(
            {{2, 45.0f}, {0, -45.0f}, {1, 0.0f}}, 1, 1920);
        check(planned.size() == 3 && planned[0].driver_index == 2 && planned[0].x == 1920 &&
                      planned[1].driver_index == 0 && planned[1].x == -1920 &&
                      planned[2].driver_index == 1 && planned[2].x == 0,
              "shuffled bind order keeps positions by yaw, order by input");
    }
    {
        const auto planned = gt::plan_workspace_desktops(
            {{1, -30.0f}, {0, -60.0f}, {2, 0.0f}}, 2, 1920);
        check(planned[0].x == -1920 && planned[1].x == -3840 && planned[2].x == 0,
              "same-side screens stack by yaw closeness, not input order");
    }
    check(gt::select_internal_display(
              {{L"\\\\.\\DISPLAY1", false, true}, {L"\\\\.\\DISPLAY2", true, false}}) ==
              L"\\\\.\\DISPLAY2",
          "the internal panel is picked even when it is not primary");
    check(gt::select_internal_display(
              {{L"A", true, false}, {L"B", true, true}}) == L"B",
          "a primary internal panel wins over a secondary one");
    check(gt::select_internal_display({{L"A", false, true}}).empty(),
          "desktop PCs without an internal panel detach nothing");

    std::printf("vdd_selftest: layout hot reload\n");
    test_stacked_screen_binding();
    test_workspace_reload("late arrival", 300, 0);
    test_workspace_reload("never arrives", 99999, 0);
    test_workspace_reload("one removal failure", 99999, 1);
    test_workspace_reload("permanent removal failure", 99999, 3);

    std::printf("vdd_selftest: aside planning\n");
    check(gt::display_rects_intersect(0, 0, 10, 10, 5, 5, 10, 10), "overlap is detected");
    check(!gt::display_rects_intersect(0, 0, 10, 10, 10, 0, 10, 10),
          "touching edges do not overlap");
    check(!gt::display_rects_intersect(0, 0, 10, 10, 0, 10, 10, 10),
          "stacked edges do not overlap");
    const auto make_snapshot = [](const wchar_t* name, int x, int y, int w, int h, bool primary) {
        gt::DisplayModeSnapshot snapshot;
        snapshot.device_name = name;
        snapshot.mode.dmSize = sizeof(snapshot.mode);
        snapshot.mode.dmPelsWidth = static_cast<DWORD>(w);
        snapshot.mode.dmPelsHeight = static_cast<DWORD>(h);
        snapshot.mode.dmPosition.x = x;
        snapshot.mode.dmPosition.y = y;
        snapshot.primary = primary;
        return snapshot;
    };
    {
        // The live machine: laptop primary at the origin, glasses above.
        const std::vector<gt::DisplayModeSnapshot> snapshots = {
            make_snapshot(L"\\\\.\\DISPLAY1", 0, 0, 2880, 1800, true),
            make_snapshot(L"\\\\.\\DISPLAY2", 457, -1080, 1920, 1080, false),
        };
        const std::vector<gt::PlannedDesktop> slots = {{0, -1920, 0}, {1, 0, 0}, {2, 1920, 0}};
        const auto moves = gt::plan_aside_moves(snapshots, slots, 1920, 1080, {});
        check(moves.size() == 1 && moves[0].device_name == L"\\\\.\\DISPLAY1" &&
                  moves[0].x == 3840 && moves[0].y == 0,
              "only the overlapping primary moves aside, right of everything");
    }
    {
        // An overlapping secondary stacks after the primary.
        const std::vector<gt::DisplayModeSnapshot> snapshots = {
            make_snapshot(L"A", 0, 0, 1920, 1080, true),
            make_snapshot(L"B", 0, 0, 1920, 1080, false),
        };
        const std::vector<gt::PlannedDesktop> slots = {{0, -1920, 0}, {1, 0, 0}, {2, 1920, 0}};
        const auto moves = gt::plan_aside_moves(snapshots, slots, 1920, 1080, {});
        check(moves.size() == 2 && moves[0].device_name == L"A" && moves[0].x == 3840 &&
                  moves[1].device_name == L"B" && moves[1].x == 5760,
              "overlapping displays stack right of the slots, primary first");
    }
    {
        // VDDs are never moved, even when a stale snapshot overlaps a slot.
        const std::vector<gt::DisplayModeSnapshot> snapshots = {
            make_snapshot(L"V", 0, 0, 1920, 1080, false),
        };
        const std::vector<gt::PlannedDesktop> slots = {{0, -1920, 0}, {1, 0, 0}, {2, 1920, 0}};
        check(gt::plan_aside_moves(snapshots, slots, 1920, 1080, {L"V"}).empty(),
              "virtual displays are excluded from the aside plan");
    }

    std::printf("vdd_selftest: window migration\n");
    const auto make_rect = [](int l, int t, int r, int b) {
        RECT rect{};
        rect.left = l;
        rect.top = t;
        rect.right = r;
        rect.bottom = b;
        return rect;
    };
    const auto rect_eq = [](const RECT& a, const RECT& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    };
    {
        // Offset preserved: a window on the laptop-aside frame lands at the
        // same offset on the center desktop.
        const RECT mapped = gt::migrate_window_rect(make_rect(3940, 200, 4340, 600),
                                                    make_rect(3840, 0, 6720, 1800),
                                                    make_rect(0, 0, 1920, 1080));
        check(rect_eq(mapped, make_rect(100, 200, 500, 600)),
              "migration preserves the window offset");
    }
    {
        // Clamped inside: a window whose offset would overhang the smaller
        // center desktop is pulled back inside it.
        const RECT mapped = gt::migrate_window_rect(make_rect(6500, 1600, 6700, 1750),
                                                    make_rect(3840, 0, 6720, 1800),
                                                    make_rect(0, 0, 1920, 1080));
        check(rect_eq(mapped, make_rect(1720, 930, 1920, 1080)),
              "migration clamps inside the target");
    }
    {
        // Oversize windows pin at the target origin instead of clamping to a
        // negative offset.
        const RECT mapped = gt::migrate_window_rect(make_rect(3840, 0, 6720, 1800),
                                                    make_rect(3840, 0, 6720, 1800),
                                                    make_rect(0, 0, 1920, 1080));
        check(rect_eq(mapped, make_rect(0, 0, 2880, 1800)), "oversize windows pin at the origin");
    }
    {
        // Clamp-only identity (from == to): the restore repair's orphan clamp.
        const RECT mapped = gt::migrate_window_rect(make_rect(-100, -100, 100, 100),
                                                    make_rect(0, 0, 2880, 1800),
                                                    make_rect(0, 0, 2880, 1800));
        check(rect_eq(mapped, make_rect(0, 0, 200, 200)),
              "a from==to map preserves the offset and clamps inside");
    }
    {
        const RECT mapped = gt::migrate_window_rect(make_rect(2800, 1700, 3000, 1900),
                                                    make_rect(0, 0, 2880, 1800),
                                                    make_rect(0, 0, 2880, 1800));
        check(rect_eq(mapped, make_rect(2680, 1600, 2880, 1800)),
              "a from==to map pulls a bottom-right overhang back inside");
    }

    std::printf("vdd_selftest: display identity resolution\n");
    {
        // A renumbered display resolves by stable monitor id, not by name.
        const std::vector<std::pair<std::wstring, std::wstring>> live = {
            {L"\\\\.\\DISPLAY9", L"MONITOR\\LEN8AC3\\0002"},
        };
        check(gt::resolve_live_device_name(live, L"\\\\.\\DISPLAY2", L"MONITOR\\LEN8AC3\\0002") ==
                  L"\\\\.\\DISPLAY9",
              "resolution follows the monitor id across renumbers");
    }
    {
        // Empty or unmatched ids fall back to the snapshot name.
        const std::vector<std::pair<std::wstring, std::wstring>> live = {
            {L"\\\\.\\DISPLAY9", L"MONITOR\\LEN8AC3\\0002"},
        };
        check(gt::resolve_live_device_name(live, L"\\\\.\\DISPLAY2", L"") == L"\\\\.\\DISPLAY2",
              "empty snapshot ids fall back to the snapshot name");
        check(gt::resolve_live_device_name(live, L"\\\\.\\DISPLAY2", L"MONITOR\\OTHER\\0009") ==
                  L"\\\\.\\DISPLAY2",
              "unmatched ids fall back to the snapshot name");
    }
    {
        // Frequency strip keeps geometry, drops the rate.
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        mode.dmPelsWidth = 2880;
        mode.dmPelsHeight = 1800;
        mode.dmDisplayFrequency = 59;
        mode.dmPosition.x = 0;
        mode.dmPosition.y = 0;
        mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_POSITION;
        gt::strip_display_frequency(mode);
        check(mode.dmDisplayFrequency == 0 && (mode.dmFields & DM_DISPLAYFREQUENCY) == 0 &&
                  mode.dmPelsWidth == 2880 && mode.dmPelsHeight == 1800 &&
                  (mode.dmFields & DM_POSITION) != 0,
              "frequency strip drops the rate and keeps the geometry");
    }

    std::printf("vdd_selftest: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
