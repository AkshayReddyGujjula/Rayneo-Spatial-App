#include "vdd/display_config.h"
#include "vdd/display_scale.h"
#include "vdd/vdd_client.h"
#include "vdd/workspace_reload.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <cstdio>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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
    } else if (removal_failures < 4) {
        const std::vector<int> all_backoff{250, 750, 1500};
        const std::vector<int> expected_backoff(all_backoff.begin(),
                                                all_backoff.begin() + removal_failures);
        check(!reloaded && rollback_error.empty() && configured == previous_displays &&
                  indices == std::vector<int>{0, 1, 2} && error.starts_with("timed out") &&
                  removal_attempts == removal_failures + 1 &&
                  rollback_active_ms == (removal_failures == 1 ? 200 : -1) &&
                  backoff == expected_backoff &&
                  events == std::vector<std::string>{"resize 4", "reload",
                      "reload rollback settle", "resize 3", "reload rollback", "reposition",
                      "capture restore"},
              "missing display rejects the reload and rebinds the restored workspace");
    } else {
        check(!reloaded && configured == previous_displays && indices.size() == 4 &&
                  rollback_error == "virtual display rollback failed: failed to remove Parsec "
                                    "virtual display 3: " + ioctl_error &&
                  removal_attempts == 4 && backoff == std::vector<int>{250, 750, 1500} &&
                  events == std::vector<std::string>{"resize 4", "reload",
                      "reload rollback settle", "resize 3"},
              "exhausted removal retries are fatal and retain the IOCTL error");
    }
}

// The mid-session laptop re-detach guard against faked Win32 steps.
// `detach_reports` is what the CCD call returns; `lands` is whether the panel
// then really reports detached.
void test_redetach(const char* label, bool panel_active, bool detach_reports, bool lands) {
    std::printf("  redetach: %s\n", label);
    const auto hwnd = [](uintptr_t id) { return reinterpret_cast<HWND>(id); };
    const RECT pristine{0, 0, 2880, 1800};
    const RECT current{3840, 0, 6720, 1800};
    const RECT center{0, 0, 1920, 1080};
    std::vector<gt::MigratedWindow> migrated{{hwnd(1), RECT{100, 100, 500, 400}, false}};
    std::vector<std::string> calls;
    std::vector<HWND> restored;
    std::vector<HWND> repaired;
    gt::RedetachOps ops;
    ops.find_panel = [&](std::wstring& panel, std::string&) {
        panel = panel_active ? L"\\\\.\\DISPLAY1" : L"";
        return true;
    };
    ops.read_rect = [&](const std::wstring&, RECT& rect) {
        rect = current;
        return true;
    };
    ops.snapshot_windows = [&](const RECT& from, std::vector<gt::MigratedWindow>& out) {
        check(from.left == current.left, "windows are recorded on the panel's live rect");
        // hwnd 1 is already recorded since the takeover; hwnd 2 is new.
        out = {{hwnd(1), RECT{3940, 100, 4340, 400}, false},
               {hwnd(2), RECT{4840, 200, 5240, 600}, false}};
    };
    ops.place_windows = [&](const std::vector<gt::MigratedWindow>&, const RECT& from,
                            const RECT& to) {
        calls.push_back(from.left == current.left && to.left == center.left ? "place" : "place?");
    };
    ops.restore_home = [&](const std::vector<gt::MigratedWindow>& windows) {
        calls.push_back("restore");
        for (const auto& entry : windows) {
            restored.push_back(entry.hwnd);
            if (entry.hwnd == hwnd(2)) {
                check(entry.home.left == 4840, "a rolled-back window returns to its panel rect");
            }
        }
    };
    ops.detach = [&](const std::wstring&, std::string& detail) {
        calls.push_back("detach");
        if (!detach_reports) detail = "SetDisplayConfig failed (87)";
        return detach_reports;
    };
    ops.wait_detached = [&](const std::wstring&) { return lands; };
    ops.repair = [&](const std::vector<gt::MigratedWindow>& windows) {
        calls.push_back("repair");
        for (const auto& entry : windows) repaired.push_back(entry.hwnd);
    };
    std::string error;
    const bool ok = gt::redetach_internal_display(ops, pristine, center, migrated, error);
    if (!panel_active) {
        check(!ok && error.empty() && calls.empty(), "no active panel: nothing happens");
        return;
    }
    check(!calls.empty() && calls.front() == "place" && calls.size() >= 2 && calls[1] == "detach",
          "windows leave the panel before the detach");
    if (lands) {
        check(ok, "a detach that lands is a success even when the call reported an error");
        check(std::count(repaired.begin(), repaired.end(), hwnd(2)) == 1,
              "the detach's minimize wave is repaired");
        check(migrated.size() == 2, "a new window is recorded once, a known one not again");
        check(migrated.size() == 2 && migrated[0].home.left == 100 &&
                  migrated[1].home.left == 1000 && migrated[1].home.top == 200,
              "the new window's home maps onto the pristine laptop rect; the old one keeps its own");
        check(restored.empty(), "nothing is moved back after a successful detach");
    } else {
        check(!ok && !error.empty(), "a detach that does not land reports an error");
        check(calls.back() == "restore" && restored.size() == 2,
              "windows go back onto the still-attached panel");
        check(migrated.size() == 1 && repaired.empty(),
              "nothing is recorded or repaired while the panel stays attached");
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
    std::vector<std::pair<int, int>> positions;
    std::vector<int> paired_indices;
    for (const auto& display : after) {
        positions.emplace_back(display.x, display.y);
        paired_indices.push_back(display.driver_index);
    }
    std::printf("  stacked above/left/center/right: center driver=%d, driver@(x,y)=",
                stacked_plan.center_driver);
    for (const auto& display : after) {
        std::printf(" %d@(%d,%d)", display.driver_index, display.x, display.y);
    }
    std::printf("\n");
    // The stacked screen's desktop sits directly above the centre's, so the
    // mouse goes up into it and right into the right screen (field request
    // 2026-11-18; it used to land at x=1920 and push the right screen out).
    check(stacked_plan.center_driver == 1 && paired_indices == std::vector<int>{3, 2, 1, 0} &&
              positions == std::vector<std::pair<int, int>>{
                               {0, -1080}, {-1920, 0}, {0, 0}, {1920, 0}},
          "a screen stacked above the centre gets the desktop directly above it");

    const auto sources = gt::reusable_capture_sources(before, after);
    const auto kept = std::count_if(sources.begin(), sources.end(),
                                   [](const auto& source) { return source.has_value(); });
    std::printf("  capture reuse: kept=%td new_or_moved=%zu\n", kept,
                after.size() - static_cast<size_t>(kept));
    check(sources == std::vector<std::optional<size_t>>{std::nullopt, 0, 1, 2},
          "growing a stacked screen keeps every existing capture and opens only the new one");

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

std::pair<int, int> desktop_at(const std::vector<gt::PlannedDesktop>& planned, int driver) {
    for (const auto& desktop : planned) {
        if (desktop.driver_index == driver) return {desktop.x, desktop.y};
    }
    return {-99999, -99999};
}

const gt::PlannedDesktop* desktop_of(const std::vector<gt::PlannedDesktop>& planned, int driver) {
    for (const auto& desktop : planned) {
        if (desktop.driver_index == driver) return &desktop;
    }
    return nullptr;
}

void print_plan(const char* label, const std::vector<gt::PlannedDesktop>& planned) {
    std::printf("  %s:", label);
    for (const auto& desktop : planned) {
        std::printf(" %d@(%d,%d %dx%d%s)", desktop.driver_index, desktop.x, desktop.y,
                    desktop.width, desktop.height, desktop.portrait ? " portrait" : "");
    }
    std::printf("\n");
}

bool seamless(const std::vector<gt::PlannedDesktop>& planned) {
    std::string reason;
    const bool ok = gt::desktops_tile_seamlessly(planned, reason);
    if (!ok) std::printf("    not seamless: %s\n", reason.c_str());
    return ok;
}

// Portrait (DMDO_90) desktops: 1080 wide x 1920 tall, from the 1920x1080 mode.
void test_portrait_planning() {
    using P = gt::ScreenPlacement;
    // A portrait screen's angular size is the landscape one turned on its side.
    const auto portrait = [](int driver, float yaw, float pitch = 0.0f) {
        return P{driver, yaw, pitch, 26.9f, 46.0f, true};
    };
    {
        const std::vector<P> screens{portrait(0, -45.0f), {1, 0.0f}, {2, 45.0f}};
        const auto planned = gt::plan_workspace_desktops(screens, 1, 1920, 1080);
        print_plan("portrait left", planned);
        const auto* left = desktop_of(planned, 0);
        check(left != nullptr && left->portrait && left->width == 1080 && left->height == 1920,
              "a portrait screen plans a 1080 x 1920 desktop");
        check(desktop_at(planned, 0) == std::make_pair(-1080, -420) &&
                  desktop_at(planned, 1) == std::make_pair(0, 0) &&
                  desktop_at(planned, 2) == std::make_pair(1920, 0),
              "portrait left abuts the centre at x = -1080, vertically centred (y = -420)");
        check(seamless(planned), "portrait left tiles seamlessly");
    }
    {
        const std::vector<P> screens{{0, -45.0f}, {1, 0.0f}, portrait(2, 45.0f)};
        const auto planned = gt::plan_workspace_desktops(screens, 1, 1920, 1080);
        print_plan("portrait right", planned);
        check(desktop_at(planned, 0) == std::make_pair(-1920, 0) &&
                  desktop_at(planned, 2) == std::make_pair(1920, -420),
              "portrait right sits at (1920, -420)");
        check(seamless(planned), "portrait right tiles seamlessly");
    }
    {
        const std::vector<P> screens{{0, -45.0f}, portrait(1, 0.0f), {2, 45.0f}};
        const auto planned = gt::plan_workspace_desktops(screens, 1, 1920, 1080);
        print_plan("portrait centre", planned);
        const auto* center = desktop_of(planned, 1);
        check(center != nullptr && center->x == 0 && center->y == 0 && center->width == 1080 &&
                  center->height == 1920,
              "a portrait centre stays at the origin, 1080 x 1920");
        check(desktop_at(planned, 0) == std::make_pair(-1920, 420) &&
                  desktop_at(planned, 2) == std::make_pair(1080, 420),
              "landscape neighbours centre on a portrait centre (y = 420), right at x = 1080");
        check(seamless(planned), "portrait centre tiles seamlessly");
    }
    {
        const std::vector<P> screens{{0, -45.0f}, {1, 0.0f}, {2, 45.0f}, portrait(3, 0.0f, 28.0f)};
        const int center = gt::select_center_driver(screens);
        const auto planned = gt::plan_workspace_desktops(screens, center, 1920, 1080);
        print_plan("portrait above the centre", planned);
        check(center == 1 && desktop_at(planned, 3) == std::make_pair(420, -1920),
              "a portrait screen stacked above the centre is centred over it (420, -1920)");
        check(desktop_at(planned, 0) == std::make_pair(-1920, 0) &&
                  desktop_at(planned, 2) == std::make_pair(1920, 0),
              "the row under a portrait stack is unchanged");
        check(seamless(planned), "a portrait stack tiles seamlessly");
    }
    {
        // Two portrait screens left of the centre stay adjacent.
        const std::vector<P> screens{portrait(0, -60.0f), portrait(1, -30.0f), {2, 0.0f}};
        const auto planned = gt::plan_workspace_desktops(screens, 2, 1920, 1080);
        check(desktop_at(planned, 0) == std::make_pair(-2160, -420) &&
                  desktop_at(planned, 1) == std::make_pair(-1080, -420) && seamless(planned),
              "consecutive portrait columns abut by their own widths");
    }
    {
        // A landscape screen above a portrait centre widens the column past
        // its base: the row would have a gap, which the checker must refuse.
        const std::vector<P> screens{{0, -45.0f}, portrait(1, 0.0f), {2, 0.0f, 40.0f}};
        const auto planned = gt::plan_workspace_desktops(screens, 1, 1920, 1080);
        print_plan("landscape above a portrait centre", planned);
        std::string reason;
        check(!gt::desktops_tile_seamlessly(planned, reason) && !reason.empty(),
              "a column wider than its base leaves a gap and is rejected");
        gt::Layout layout;
        layout.screens = {{"left", 0, -45.0f}, {"center", 1, 0.0f}, {"above", 2, 0.0f, 40.0f}};
        layout.screens[1].portrait = true;
        std::swap(layout.screens[1].width_m, layout.screens[1].height_m);
        gt::WorkspacePlan plan;
        std::string error;
        check(!gt::plan_workspace_for_layout(layout, {0, 1, 2}, plan, error) &&
                  error.find("seamless") != std::string::npos,
              "the layout planner refuses a gapped arrangement before touching displays");
    }
    {
        // Review finding: a landscape screen above a portrait centre AND a
        // landscape screen above the left base. The upper row connects
        // everything, hiding a 420 px gap between the left base (ends at
        // x = -420) and the portrait centre (starts at x = 0).
        const std::vector<P> screens{
            {0, -45.0f}, {4, -45.0f, 40.0f}, portrait(1, 0.0f), {2, 0.0f, 40.0f}};
        const auto planned = gt::plan_workspace_desktops(screens, 1, 1920, 1080);
        print_plan("stacks over a landscape side and a portrait centre", planned);
        std::string reason;
        const bool connected = gt::desktops_tile_seamlessly(planned, reason);
        const auto row = gt::workspace_base_row(screens, 1);
        check(row == std::vector<int>{0, 1}, "the base row is the left base then the centre");
        const bool accepted = gt::desktops_tile_seamlessly(planned, reason, row);
        std::printf("    connectivity-only verdict %s; with the base row: %s\n",
                    connected ? "accepted" : "refused", reason.c_str());
        check(connected && !accepted && reason.find("420 px") != std::string::npos,
              "a gap between neighbouring bases is refused even when the stacked row connects");
        gt::Layout layout;
        layout.screens = {{"left", 0, -45.0f},
                          {"center", 1, 0.0f},
                          {"above", 2, 0.0f, 40.0f},
                          {"above-left", 3, -45.0f, 40.0f}};
        layout.screens[1].portrait = true;
        std::swap(layout.screens[1].width_m, layout.screens[1].height_m);
        std::string why;
        check(!gt::workspace_layout_is_arrangeable(layout, why) &&
                  why.find("main row") != std::string::npos,
              "the pure preflight refuses it (no display calls)");
        std::printf("    preflight reason: %s\n", why.c_str());
    }
    {
        // The review's earlier example: a portrait screen above the left
        // landscape side screen, next to a landscape screen above the centre:
        // the upper row has a 420 px gap. Accepted by decision: the bases
        // abut and each stacked screen touches its own base, so Windows has
        // nothing to repack; only the mouse cannot cross that upper gap.
        const std::vector<P> screens{
            {0, -45.0f}, portrait(3, -45.0f, 40.0f), {1, 0.0f}, {2, 0.0f, 40.0f}};
        const auto planned = gt::plan_workspace_desktops(screens, 1, 1920, 1080);
        print_plan("portrait above the side, landscape above the centre", planned);
        std::string reason;
        check(desktop_at(planned, 3) == std::make_pair(-1500, -1920) &&
                  desktop_at(planned, 2) == std::make_pair(0, -1080) &&
                  gt::desktops_tile_seamlessly(planned, reason,
                                               gt::workspace_base_row(screens, 1)),
              "an upper-row gap between two stacked screens is accepted (bases abut)");
    }
    {
        // Yaw seam: +180 and -180 deg are one direction and must group (and
        // plan) identically.
        const auto seam = [&](float behind_yaw) {
            return std::vector<P>{
                {0, 0.0f}, portrait(1, behind_yaw), {2, 170.0f}, {3, -179.0f, 40.0f}};
        };
        const auto plus = seam(180.0f);
        const auto minus = seam(-180.0f);
        const auto plus_plan = gt::plan_workspace_desktops(plus, 0, 1920, 1080);
        const auto minus_plan = gt::plan_workspace_desktops(minus, 0, 1920, 1080);
        print_plan("seam +180", plus_plan);
        print_plan("seam -180", minus_plan);
        std::string plus_reason;
        std::string minus_reason;
        const bool plus_ok = gt::desktops_tile_seamlessly(plus_plan, plus_reason,
                                                          gt::workspace_base_row(plus, 0));
        const bool minus_ok = gt::desktops_tile_seamlessly(minus_plan, minus_reason,
                                                           gt::workspace_base_row(minus, 0));
        check(plus_plan == minus_plan && plus_ok == minus_ok &&
                  gt::workspace_base_row(plus, 0) == gt::workspace_base_row(minus, 0),
              "a level portrait at +180 and at -180 deg give the same plan and verdict");
    }
    {
        // Reload/re-pin readback verdict: only a position shift is tolerated.
        using V = gt::ReadbackVerdict;
        const gt::DisplayReadback planned{1920, -420, 1080, 1920, DMDO_90, false};
        auto moved = planned;
        moved.x = 1900;
        auto kept_landscape = planned;
        kept_landscape.width = 1920;
        kept_landscape.height = 1080;
        kept_landscape.orientation = DMDO_DEFAULT;
        auto rotation_only = planned;
        rotation_only.orientation = DMDO_DEFAULT;
        auto became_primary = planned;
        became_primary.primary = true;
        check(gt::classify_display_readback(planned, planned) == V::Match,
              "readback: an exact match is accepted");
        check(gt::classify_display_readback(planned, moved) == V::PositionOnly,
              "readback: a position-only shift is tolerated");
        check(gt::classify_display_readback(planned, kept_landscape) == V::Mismatch &&
                  gt::classify_display_readback(planned, rotation_only) == V::Mismatch &&
                  gt::classify_display_readback(planned, became_primary) == V::Mismatch,
              "readback: Windows keeping landscape, the old rotation or another primary fails");
    }
    {
        gt::Layout triple;
        triple.screens = {{"left", 0, -45.0f}, {"center", 1, 0.0f}, {"right", 2, 45.0f}};
        std::string why;
        check(gt::workspace_layout_is_arrangeable(triple, why) && why.empty(),
              "the landscape triple passes the preflight");
    }
    {
        std::string reason;
        check(!gt::desktops_tile_seamlessly({{0, 0, 0}, {1, 1000, 0}}, reason),
              "overlapping desktops are not seamless");
        check(!gt::desktops_tile_seamlessly({{0, 0, 0}, {1, 1920, 1080}}, reason),
              "corner-only contact is not seamless");
        check(gt::desktops_tile_seamlessly({{0, 0, 0}, {1, 1920, 0}, {2, -1920, 0}}, reason),
              "the landscape triple is seamless");
    }
    {
        // Mode builder: explicit orientation both ways (Windows persists it).
        const auto tall = gt::build_virtual_desktop_mode({1, -1080, -420, 1080, 1920, true}, 60);
        check(tall.dmPelsWidth == 1080 && tall.dmPelsHeight == 1920 &&
                  tall.dmDisplayOrientation == DMDO_90 && tall.dmDisplayFrequency == 60 &&
                  tall.dmPosition.x == -1080 && tall.dmPosition.y == -420 &&
                  (tall.dmFields & DM_DISPLAYORIENTATION) != 0 &&
                  (tall.dmFields & DM_POSITION) != 0,
              "a portrait desktop stages 1080x1920 with DMDO_90");
        const auto wide = gt::build_virtual_desktop_mode({0, 1920, 0}, 120);
        check(wide.dmPelsWidth == 1920 && wide.dmPelsHeight == 1080 &&
                  wide.dmDisplayOrientation == DMDO_DEFAULT &&
                  (wide.dmFields & DM_DISPLAYORIENTATION) != 0,
              "a landscape desktop states DMDO_DEFAULT explicitly");
        const RECT tall_center = gt::center_desktop_rect(
            {{0, L"L", 1920, 1080, 120, -1920, 420}, {1, L"C", 1080, 1920, 120, 0, 0, DMDO_90}});
        const RECT default_center = gt::center_desktop_rect({});
        check(tall_center.left == 0 && tall_center.top == 0 && tall_center.right == 1080 &&
                  tall_center.bottom == 1920 && default_center.right == 1920 &&
                  default_center.bottom == 1080,
              "the centre rect follows a portrait centre's own extent");
    }
}

// Toggling portrait on an existing screen (same screen count) must re-mode
// that display and rebind only its capture.
void test_portrait_toggle_reload() {
    const auto previous = triple_layout();
    auto candidate = previous;
    candidate.screens[0].portrait = true;
    std::swap(candidate.screens[0].width_m, candidate.screens[0].height_m);
    std::vector<int> indices{0, 1, 2};
    std::vector<gt::ConfiguredDisplay> configured{
        {0, L"left", 1920, 1080, 120, -1920, 0},
        {1, L"center", 1920, 1080, 120, 0, 0},
        {2, L"right", 1920, 1080, 120, 1920, 0},
    };
    const auto before = configured;
    bool repositioned = false;
    bool resized = false;
    std::vector<gt::ConfiguredDisplay> applied;
    const gt::WorkspaceReloadOps ops{
        [&]() -> const std::vector<int>& { return indices; },
        [&](size_t, std::string&) {
            resized = true;
            return true;
        },
        [&](const std::vector<int>&, int, const char*, std::vector<gt::VirtualDisplayInfo>& displays,
            std::string&) {
            displays = {{0, L"left", L"", true}, {1, L"center", L"", true},
                        {2, L"right", L"", true}};
            return true;
        },
        [&](const gt::WorkspacePlan& plan, const std::map<int, std::wstring>& names,
            std::vector<gt::ConfiguredDisplay>& displays, std::string&) {
            repositioned = true;
            displays.clear();
            for (const auto& desktop : plan.desktops) {
                const DEVMODEW mode = gt::build_virtual_desktop_mode(desktop, 120);
                displays.push_back({desktop.driver_index, names.at(desktop.driver_index),
                                    static_cast<int>(mode.dmPelsWidth),
                                    static_cast<int>(mode.dmPelsHeight), 120, desktop.x,
                                    desktop.y, static_cast<int>(mode.dmDisplayOrientation)});
            }
            return true;
        },
        [&](const auto& displays, std::string&) {
            applied = displays;
            return true;
        },
        [&](const auto&, std::string&) { return true; },
    };
    std::string error, rollback_error;
    const bool reloaded =
        gt::reload_workspace(previous, candidate, configured, ops, error, rollback_error);
    std::printf("  portrait toggle reload: result=%s repositioned=%d", reloaded ? "applied" : "rejected",
                repositioned ? 1 : 0);
    for (const auto& display : configured) {
        std::printf(" %d@(%d,%d %dx%d o%d)", display.driver_index, display.x, display.y,
                    display.width, display.height, display.orientation);
    }
    std::printf("\n");
    check(reloaded && repositioned && !resized,
          "a portrait toggle with an unchanged screen count re-modes the displays");
    check(configured.size() == 3 && configured[0].width == 1080 && configured[0].height == 1920 &&
              configured[0].orientation == DMDO_90 && configured[0].x == -1080 &&
              configured[0].y == -420,
          "the toggled display is configured portrait at (-1080, -420)");
    const auto sources = gt::reusable_capture_sources(before, configured);
    check(sources == std::vector<std::optional<size_t>>{std::nullopt, 1, 2},
          "only the rotated display's capture is rebound");
}

}  // namespace

// Fake display world for DisplayScaleSession: name -> monitor id, per-id
// scale state, the active virtual displays by driver index, and an in-memory
// backup file.
struct FakeScaleWorld {
    std::vector<std::pair<std::wstring, std::wstring>> identities;
    std::map<int, std::wstring> live;  // driver index -> active virtual display name
    std::map<std::wstring, gt::DisplayScaleInfo> by_id;  // keyed by monitor id
    std::vector<std::pair<std::wstring, int>> sets;      // (name, rel) writes
    std::vector<std::string> lines;
    std::vector<std::string> events;  // "set" / "save" in call order
    bool fail_sets = false;
    gt::DisplayScaleBackup backup_file;
    bool backup_exists = false;
    bool fail_saves = false;

    std::wstring id_of(const std::wstring& name) const {
        for (const auto& [gdi, id] : identities) {
            if (gdi == name) return id;
        }
        return {};
    }
    gt::DisplayScaleOps ops() {
        gt::DisplayScaleOps ops;
        ops.query = [this](const std::wstring& name, gt::DisplayScaleInfo& info, std::string& error) {
            const auto it = by_id.find(id_of(name));
            if (it == by_id.end()) {
                error = "no path";
                return false;
            }
            info = it->second;
            return true;
        };
        ops.set = [this](const std::wstring& name, int rel, std::string& error) {
            sets.emplace_back(name, rel);
            events.push_back("set");
            if (fail_sets) {
                error = "simulated failure";
                return false;
            }
            by_id[id_of(name)].cur_rel = rel;
            return true;
        };
        ops.identities = [this] { return identities; };
        ops.virtual_displays = [this] { return live; };
        ops.log = [this](const std::string& line) { lines.push_back(line); };
        return ops;
    }
    gt::DisplayScaleBackupStore store() {
        gt::DisplayScaleBackupStore store;
        store.load = [this](gt::DisplayScaleBackup& backup, std::string&) {
            backup = backup_exists ? backup_file : gt::DisplayScaleBackup{};
            return true;
        };
        store.save = [this](const gt::DisplayScaleBackup& backup, std::string& error) {
            events.push_back("save");
            if (fail_saves) {
                error = "simulated save failure";
                return false;
            }
            backup_file = backup;
            backup_exists = !backup.empty();
            return true;
        };
        return store;
    }
    bool logged(std::string_view needle) const {
        for (const std::string& line : lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

void test_display_scale() {
    std::printf("vdd_selftest: virtual display scale\n");
    check(gt::display_scale_step_index(100) == 0 && gt::display_scale_step_index(175) == 3 &&
              gt::display_scale_step_index(500) == 11 && gt::display_scale_step_index(110) == -1 &&
              gt::display_scale_step_index(0) == -1,
          "scale step table: 100/175/500 found, 110 and 0 are not steps");

    // Measured on the VDDs: recommended 100%, current 100%, max 175%.
    const gt::DisplayScaleInfo vdd{0, 0, 3};
    check(gt::display_scale_recommended_percent(vdd) == 100 && gt::display_scale_current_percent(vdd) == 100 &&
              gt::display_scale_max_percent(vdd) == 175,
          "VDD range min 0 cur 0 max 3 reads 100/100/175%");
    auto t = gt::plan_display_scale(vdd, 150);
    check(t.valid && t.rel == 2 && t.percent == 150 && !t.clamped, "150% on the VDD writes rel 2");
    t = gt::plan_display_scale(vdd, 200);
    check(t.valid && t.rel == 3 && t.percent == 175 && t.clamped,
          "200% on a 175%-max display clamps to rel 3 (175%), never above max");
    t = gt::plan_display_scale(vdd, 500);
    check(t.valid && t.rel == vdd.max_rel && t.clamped, "500% clamps to max too");
    t = gt::plan_display_scale(vdd, 130);
    check(!t.valid, "a percent outside the step list is rejected");
    // A laptop-style panel: recommended 150% (min -2), current 125%, max 225%.
    const gt::DisplayScaleInfo panel{-2, -1, 3};
    check(gt::display_scale_recommended_percent(panel) == 150 && gt::display_scale_current_percent(panel) == 125 &&
              gt::display_scale_max_percent(panel) == 225,
          "recommended 150% maps: min -2 cur -1 max 3 reads 150/125/225%");
    t = gt::plan_display_scale(panel, 100);
    check(t.valid && t.rel == -2 && t.percent == 100 && !t.clamped, "100% with a 150% recommendation writes rel -2");
    t = gt::plan_display_scale(panel, 175);
    check(t.valid && t.rel == 1 && t.percent == 175, "175% with a 150% recommendation writes rel 1");
    const gt::DisplayScaleInfo odd{0, 14, 14};
    check(gt::display_scale_current_percent(odd) == 0, "an off-table current scale reads as unknown (0)");
    const gt::DisplayScaleInfo broken{5, 0, 3};
    check(!gt::plan_display_scale(broken, 150).valid, "a positive min (no recommended step) is not written");

    // Session: two VDDs, the glasses, and the laptop panel.
    FakeScaleWorld world;
    world.identities = {{L"\\\\.\\DISPLAY5", L"MONITOR\\PSCCDD0\\1"},
                        {L"\\\\.\\DISPLAY6", L"MONITOR\\PSCCDD0\\2"},
                        {L"\\\\.\\DISPLAY2", L"MONITOR\\TCL\\glasses"},
                        {L"\\\\.\\DISPLAY1", L"MONITOR\\LAPTOP\\panel"}};
    world.live = {{0, L"\\\\.\\DISPLAY5"}, {1, L"\\\\.\\DISPLAY6"}};
    world.by_id[L"MONITOR\\PSCCDD0\\1"] = {0, 0, 3};
    world.by_id[L"MONITOR\\PSCCDD0\\2"] = {0, 1, 3};  // was left at 125%
    world.by_id[L"MONITOR\\TCL\\glasses"] = {0, 0, 3};
    world.by_id[L"MONITOR\\LAPTOP\\panel"] = {-2, 0, 3};
    {
        gt::DisplayScaleSession session(200, world.ops());
        session.apply({0, 1, 9});
        check(world.sets.size() == 2 && world.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel == 3 &&
                  world.by_id[L"MONITOR\\PSCCDD0\\2"].cur_rel == 3,
              "apply writes both VDDs at the clamped max");
        check(world.by_id[L"MONITOR\\TCL\\glasses"].cur_rel == 0 &&
                  world.by_id[L"MONITOR\\LAPTOP\\panel"].cur_rel == 0,
              "a driver index that is not an active virtual display is never written");
        check(world.logged("clamped 200% to 175%") && world.logged("skipped"),
              "the clamp and the refused write are logged");
        check(session.recorded() == 2, "two originals recorded");
        // A reload re-applies: the recorded original must stay the pre-session value.
        session.apply({0, 1});
        check(session.recorded() == 2 && world.sets.size() == 2, "re-apply keeps the originals and writes nothing");

        // Display churn renumbers the GDI names (bug 15): restore follows the id.
        world.identities = {{L"\\\\.\\DISPLAY7", L"MONITOR\\PSCCDD0\\1"},
                            {L"\\\\.\\DISPLAY8", L"MONITOR\\PSCCDD0\\2"},
                            {L"\\\\.\\DISPLAY5", L"MONITOR\\LAPTOP\\panel"}};
        world.live = {{0, L"\\\\.\\DISPLAY7"}, {1, L"\\\\.\\DISPLAY8"}};
        world.sets.clear();
        session.restore_driver(1);
        check(world.sets.size() == 1 && world.sets[0].first == L"\\\\.\\DISPLAY8" && world.sets[0].second == 1 &&
                  world.by_id[L"MONITOR\\PSCCDD0\\2"].cur_rel == 1,
              "restore_driver puts the removed VDD back to its original 125% by monitor id");
        check(session.recorded() == 1 && world.by_id[L"MONITOR\\LAPTOP\\panel"].cur_rel == 0,
              "the restored record is dropped; the renumbered laptop name is untouched");
        session.restore_driver(1);
        check(world.sets.size() == 1, "a second restore of the same driver writes nothing");
        // The destructor restores the rest.
    }
    check(world.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel == 0 && world.logged("restored 100%"),
          "session end restores the remaining VDD to 100%");

    // An id that no longer resolves to an active virtual display: zero writes.
    FakeScaleWorld gone;
    gone.identities = {{L"\\\\.\\DISPLAY5", L"MONITOR\\PSCCDD0\\1"}};
    gone.live = {{0, L"\\\\.\\DISPLAY5"}};
    gone.by_id[L"MONITOR\\PSCCDD0\\1"] = {0, 0, 3};
    {
        gt::DisplayScaleSession session(150, gone.ops());
        session.apply({0});
        check(gone.sets.size() == 1 && gone.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel == 2, "150% applied");
        gone.live.clear();  // the VDD went away before the restore
        gone.sets.clear();
        session.restore_all();
        check(gone.sets.empty() && gone.logged("skipped"), "an unresolved id is skipped with zero writes");
        check(session.recorded() == 1, "a skipped restore keeps the record");
        // It re-appears under another GDI name: the original is still restorable.
        gone.identities = {{L"\\\\.\\DISPLAY9", L"MONITOR\\PSCCDD0\\1"}};
        gone.live = {{0, L"\\\\.\\DISPLAY9"}};
        session.restore_all();
        check(gone.sets.size() == 1 && gone.sets[0].first == L"\\\\.\\DISPLAY9" && gone.sets[0].second == 0 &&
                  session.recorded() == 0,
              "a display that re-appears after a skipped restore is put back to 100%");
        session.restore_all();
        check(gone.sets.size() == 1, "restore_all is idempotent once restored");
    }

    // Set failures are logged, never fatal; 0 means Windows is never touched.
    FakeScaleWorld failing;
    failing.identities = {{L"\\\\.\\DISPLAY5", L"MONITOR\\PSCCDD0\\1"}};
    failing.live = {{0, L"\\\\.\\DISPLAY5"}};
    failing.by_id[L"MONITOR\\PSCCDD0\\1"] = {0, 0, 3};
    failing.fail_sets = true;
    {
        gt::DisplayScaleSession session(125, failing.ops());
        session.apply({0});
        check(failing.logged("failed: simulated failure"), "a failed set is logged and the session continues");
    }
    FakeScaleWorld off;
    off.identities = failing.identities;
    off.live = failing.live;
    off.by_id[L"MONITOR\\PSCCDD0\\1"] = {0, 0, 3};
    {
        gt::DisplayScaleSession session(0, off.ops());
        session.apply({0});
        check(off.sets.empty() && session.recorded() == 0 && !session.enabled() && !session.active(),
              "target 0 (Windows default) touches nothing");
    }

    // A failed restore keeps the original: the next restore still writes it,
    // and a later apply never records the scaled value as the original.
    FakeScaleWorld flaky;
    flaky.identities = {{L"\\\\.\\DISPLAY5", L"MONITOR\\PSCCDD0\\1"}};
    flaky.live = {{0, L"\\\\.\\DISPLAY5"}};
    flaky.by_id[L"MONITOR\\PSCCDD0\\1"] = {0, 0, 3};
    {
        gt::DisplayScaleSession session(175, flaky.ops());
        session.apply({0});
        check(flaky.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel == 3, "175% applied");
        flaky.fail_sets = true;
        check(!session.restore_driver(0) && session.recorded() == 1, "a failed restore_driver keeps the record");
        session.restore_all();
        check(session.recorded() == 1, "a failed restore_all keeps the record");
        flaky.fail_sets = false;
        session.apply({0});  // a reload re-applies while the display is still at 175%
        flaky.sets.clear();
        check(session.restore_driver(0) && flaky.sets.size() == 1 && flaky.sets[0].second == 0 &&
                  flaky.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel == 0 && session.recorded() == 0,
              "the next restore still writes the original 100%");
    }

    // Apply resolves names fresh: after Windows renumbers (here swaps) the
    // GDI names, each driver index still gets its own display and original.
    FakeScaleWorld renamed;
    renamed.identities = {{L"\\\\.\\DISPLAY5", L"MONITOR\\PSCCDD0\\1"}, {L"\\\\.\\DISPLAY6", L"MONITOR\\PSCCDD0\\2"}};
    renamed.live = {{0, L"\\\\.\\DISPLAY5"}, {1, L"\\\\.\\DISPLAY6"}};
    renamed.by_id[L"MONITOR\\PSCCDD0\\1"] = {0, 0, 3};  // 100%
    renamed.by_id[L"MONITOR\\PSCCDD0\\2"] = {0, 1, 3};  // 125%
    {
        gt::DisplayScaleSession session(150, renamed.ops());
        session.apply({0});
        renamed.identities = {{L"\\\\.\\DISPLAY5", L"MONITOR\\PSCCDD0\\2"},
                              {L"\\\\.\\DISPLAY6", L"MONITOR\\PSCCDD0\\1"}};
        renamed.live = {{0, L"\\\\.\\DISPLAY6"}, {1, L"\\\\.\\DISPLAY5"}};
        session.apply({0, 1});  // a cached name list would scale VDD 1 as VDD 0
        check(renamed.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel == 2 && renamed.by_id[L"MONITOR\\PSCCDD0\\2"].cur_rel == 2,
              "after renumbering both VDDs are at 150%");
        renamed.sets.clear();
        check(session.restore_driver(1) && renamed.sets.size() == 1 &&
                  renamed.sets[0].first == L"\\\\.\\DISPLAY5" && renamed.sets[0].second == 1,
              "VDD 1 (now DISPLAY5) is restored to its own 125%");
        check(session.restore_driver(0) && renamed.sets.size() == 2 &&
                  renamed.sets[1].first == L"\\\\.\\DISPLAY6" && renamed.sets[1].second == 0,
              "VDD 0 (now DISPLAY6) is restored to its own 100%");
    }

    // Two active virtual displays sharing one monitor id: no write either way.
    // A detached ghost twin (not an active virtual display) is not ambiguous.
    FakeScaleWorld twins;
    twins.identities = {{L"\\\\.\\DISPLAY5", L"MONITOR\\PSCCDD0\\1"},
                        {L"\\\\.\\DISPLAY6", L"MONITOR\\PSCCDD0\\2"},
                        {L"\\\\.\\DISPLAY9", L"MONITOR\\PSCCDD0\\1"}};  // detached ghost
    twins.live = {{0, L"\\\\.\\DISPLAY5"}, {1, L"\\\\.\\DISPLAY6"}};
    twins.by_id[L"MONITOR\\PSCCDD0\\1"] = {0, 0, 3};
    twins.by_id[L"MONITOR\\PSCCDD0\\2"] = {0, 0, 3};
    {
        gt::DisplayScaleSession session(125, twins.ops());
        session.apply({0});
        check(twins.sets.size() == 1 && session.recorded() == 1, "a ghost twin does not block the write");
        // Now VDD 1 reports the same id as VDD 0.
        twins.identities[1].second = L"MONITOR\\PSCCDD0\\1";
        twins.sets.clear();
        session.apply({1});
        check(twins.sets.empty() && twins.logged("ambiguous"), "apply to an ambiguous id writes nothing");
        check(!session.restore_driver(0) && twins.sets.empty() && session.recorded() == 1,
              "restore with an ambiguous id is skipped, logged and kept");
        twins.identities[1].second = L"MONITOR\\PSCCDD0\\2";
        check(session.restore_driver(0) && twins.sets.size() == 1 && twins.sets[0].first == L"\\\\.\\DISPLAY5",
              "once unambiguous again the restore writes the right display");
    }
}

void test_display_scale_backup() {
    std::printf("vdd_selftest: display scale crash backup\n");
    // Serialization round trip, including the backslashes of monitor ids.
    gt::DisplayScaleBackup backup;
    backup[L"MONITOR\\PSCCDD0\\{4d36e96e}\\0001"] = {0, 100};
    backup[L"MONITOR\\PSCCDD0\\2"] = {-2, 0};
    const std::string text = gt::serialize_display_scale_backup(backup);
    gt::DisplayScaleBackup parsed;
    std::string error;
    check(gt::parse_display_scale_backup(text, parsed, error) && parsed == backup,
          "the backup round-trips through its JSON text");
    check(text.find("\"MONITOR\\\\PSCCDD0\\\\2\": {\"rel\": -2, \"percent\": 0}") != std::string::npos,
          "the JSON escapes backslashes and keeps rel/percent");
    check(gt::parse_display_scale_backup("{}", parsed, error) && parsed.empty(), "an empty object parses");
    for (const char* bad : {"", "{", "[]", "{\"a\": {\"rel\": 1.5}}", "{\"a\": {\"rel\": \"175%\"}}",
                            "{\"a\": {\"percent\": 100}}", "{\"a\": {\"rel\": 1}} x", "{\"\": {\"rel\": 1}}"}) {
        gt::DisplayScaleBackup untouched = backup;
        check(!gt::parse_display_scale_backup(bad, untouched, error) && untouched == backup,
              "a malformed backup is rejected and leaves the output alone");
    }

    // File store: atomic replace, deleted when empty, a missing file is empty.
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("vdd_selftest-scale-" + std::to_string(GetCurrentProcessId()) + ".json");
    const gt::DisplayScaleBackupStore store = gt::file_display_scale_backup(path);
    std::filesystem::remove(path);
    gt::DisplayScaleBackup loaded{{L"stale", {1, 1}}};
    check(store.load(loaded, error) && loaded.empty(), "a missing backup file loads as empty");
    check(store.save(backup, error) && std::filesystem::exists(path), "save writes the file");
    check(store.load(loaded, error) && loaded == backup, "the file loads back what was saved");
    check(store.save({}, error) && !std::filesystem::exists(path), "an empty backup deletes the file");

    // Crash: 100% -> 175%, then the engine is killed (no restore). Windows
    // keeps 175%; the next session must still return the display to 100%.
    FakeScaleWorld world;
    world.identities = {{L"\\\\.\\DISPLAY5", L"MONITOR\\PSCCDD0\\1"}};
    world.live = {{0, L"\\\\.\\DISPLAY5"}};
    world.by_id[L"MONITOR\\PSCCDD0\\1"] = {0, 0, 3};
    gt::DisplayScaleBackup at_crash;
    {
        gt::DisplayScaleSession session(175, world.ops(), world.store());
        session.apply({0});
        check(world.events.size() == 2 && world.events[0] == "save" && world.events[1] == "set",
              "the original is saved before the first write");
        at_crash = world.backup_file;
        check(world.backup_exists && at_crash.size() == 1 && at_crash.begin()->second.rel == 0 &&
                  at_crash.begin()->second.percent == 100,
              "the backup holds the original 100% by monitor id");
    }
    check(!world.backup_exists, "a clean restore removes the entry and deletes the file");
    // Replay the kill: the display stays at 175% and the backup file stays.
    world.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel = 3;
    world.backup_file = at_crash;
    world.backup_exists = true;
    world.sets.clear();
    {
        gt::DisplayScaleSession session(175, world.ops(), world.store());
        check(session.active() && world.logged("unclean exit"), "the next session reports the unclean exit");
        session.apply({0});
        check(world.sets.empty() && world.logged("taken from the backup"),
              "already at 175%: nothing written, the original comes from the backup");
    }
    check(world.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel == 0 && !world.backup_exists,
          "crash then next session: the display returns to the true original 100%");
    // Control: without the backup the next session would keep 175% forever.
    world.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel = 3;
    {
        gt::DisplayScaleSession session(175, world.ops());
        session.apply({0});
    }
    check(world.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel == 3,
          "control: with no backup the scaled value is taken as the original");

    // Scaling turned off after the crash still puts the display back.
    world.backup_file = at_crash;
    world.backup_exists = true;
    world.sets.clear();
    {
        gt::DisplayScaleSession session(0, world.ops(), world.store());
        check(!session.enabled() && session.active(), "scaling off with a pending backup is still active");
        session.apply({0});
        check(world.sets.size() == 1 && world.sets[0].second == 0 && world.by_id[L"MONITOR\\PSCCDD0\\1"].cur_rel == 0 &&
                  !world.backup_exists && session.recorded() == 0,
              "scaling off: the unclean exit's display is restored and the backup deleted");
    }

    // A backup that cannot be saved means no write at all.
    FakeScaleWorld readonly;
    readonly.identities = world.identities;
    readonly.live = world.live;
    readonly.by_id[L"MONITOR\\PSCCDD0\\1"] = {0, 0, 3};
    readonly.fail_saves = true;
    {
        gt::DisplayScaleSession session(150, readonly.ops(), readonly.store());
        session.apply({0});
        check(readonly.sets.empty() && readonly.logged("could not be saved"),
              "an unsaved original is never overwritten");
    }
}

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
    {
        const std::vector<std::wstring> notice{L"OK",
                                               L"A toolbar is already hidden on this side of your "
                                               L"screen.\nYou can have only one auto-hide toolbar "
                                               L"per side."};
        check(gt::is_taskbar_autohide_notice(L"Taskbar", L"#32770",
                                             LR"(C:\Windows\Explorer.EXE)", notice),
              "Explorer's auto-hide taskbar notice is recognised");
        check(!gt::is_taskbar_autohide_notice(L"Taskbar", L"#32770",
                                              LR"(C:\Tools\notepad.exe)", notice),
              "the same dialog from another process is left alone");
        check(gt::is_taskbar_autohide_notice(L"Taskbar", L"#32770",
                                             LR"(C:\Windows\Explorer.EXE)", {L"OK"}),
              "the live TaskDialog form (only the OK button is readable) is recognised");
        check(!gt::is_taskbar_autohide_notice(L"Taskbar", L"#32770",
                                              LR"(C:\Windows\explorer.exe)",
                                              {L"OK", L"Some other taskbar message"}),
              "another Explorer taskbar dialog is left alone");
        check(!gt::is_taskbar_autohide_notice(L"Taskbar", L"#32770",
                                              LR"(C:\Windows\explorer.exe)", {L"Yes", L"No"}),
              "a Taskbar dialog asking a question is left alone");
        check(!gt::is_taskbar_autohide_notice(L"Taskbar", L"#32770",
                                              LR"(C:\Windows\explorer.exe)", {}),
              "a Taskbar dialog with no OK button is left alone");
        check(!gt::is_taskbar_autohide_notice(L"Taskbar settings", L"#32770",
                                              LR"(C:\Windows\explorer.exe)", notice),
              "a differently titled window is left alone");
    }

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
    {
        // Vertical stacking mirrors the 3D layout (default screens: 46 x 26.9 deg).
        using P = gt::ScreenPlacement;
        const auto at = [](const std::vector<gt::PlannedDesktop>& planned, int driver) {
            for (const auto& desktop : planned) {
                if (desktop.driver_index == driver) return std::make_pair(desktop.x, desktop.y);
            }
            return std::make_pair(-99999, -99999);
        };
        const std::vector<P> stack{{0, -45.0f, 0.0f}, {1, 0.0f, 0.0f}, {2, 45.0f, 0.0f},
                                   {3, 0.0f, 28.0f}, {4, 0.0f, -28.0f}, {5, 1.5f, 56.0f},
                                   {6, 45.0f, 28.0f}};
        const int center = gt::select_center_driver(stack);
        const auto planned = gt::plan_workspace_desktops(stack, center, 1920, 1080);
        std::printf("  stacked columns: center=%d", center);
        for (const auto& desktop : planned) {
            std::printf(" %d@(%d,%d)", desktop.driver_index, desktop.x, desktop.y);
        }
        std::printf("\n");
        check(center == 1, "a level screen stays primary under screens stacked on it");
        check(at(planned, 1) == std::make_pair(0, 0) && at(planned, 0) == std::make_pair(-1920, 0) &&
                  at(planned, 2) == std::make_pair(1920, 0),
              "the level row keeps the left/centre/right desktops");
        check(at(planned, 3) == std::make_pair(0, -1080) && at(planned, 5) == std::make_pair(0, -2160),
              "screens above the centre stack upward in pitch order");
        check(at(planned, 4) == std::make_pair(0, 1080), "a screen below the centre goes below it");
        check(at(planned, 6) == std::make_pair(1920, -1080),
              "a screen above a side screen goes above that side's desktop");

        const std::vector<P> lower_driver_above{{0, 0.0f, 30.0f}, {1, 0.0f, 0.0f}};
        check(gt::select_center_driver(lower_driver_above) == 1,
              "a stacked screen never wins the primary on driver order");
        const std::vector<P> overlapping{{0, 0.0f, 0.0f}, {1, 5.0f, 3.0f}};
        const auto side = gt::plan_workspace_desktops(overlapping, 0, 1920, 1080);
        check(at(side, 0) == std::make_pair(0, 0) && at(side, 1) == std::make_pair(1920, 0),
              "screens that overlap vertically stay side by side");
        const std::vector<P> raised_side{{0, 0.0f, 0.0f}, {1, 45.0f, 30.0f}};
        const auto raised = gt::plan_workspace_desktops(raised_side, 0, 1920, 1080);
        check(at(raised, 1) == std::make_pair(1920, 0),
              "a lone raised side screen is its column's base row (no gap below it)");
        const std::vector<P> narrow{{0, 0.0f, 0.0f}, {1, 12.0f, 20.0f, 20.0f, 11.0f}};
        const auto narrow_plan = gt::plan_workspace_desktops(narrow, 0, 1920, 1080);
        check(at(narrow_plan, 1) == std::make_pair(1920, 0),
              "a column needs yaws within half the narrower screen's width");
        // A narrow screen between the centre and a screen above it must not
        // split the stack (it opens a newer column the upper screen also
        // could not join).
        const std::vector<P> split{{0, 0.0f, 0.0f}, {1, 10.0f, 0.0f, 4.0f, 3.0f}, {2, 14.0f, 30.0f}};
        const int split_center = gt::select_center_driver(split);
        const auto split_plan = gt::plan_workspace_desktops(split, split_center, 1920, 1080);
        check(split_center == 0 && at(split_plan, 0) == std::make_pair(0, 0) &&
                  at(split_plan, 2) == std::make_pair(0, -1080) &&
                  at(split_plan, 1) == std::make_pair(1920, 0),
              "a narrow screen in between never splits a stack");
        const std::vector<P> seam{{0, 179.0f, 0.0f}, {1, -179.0f, 30.0f}};
        const int seam_center = gt::select_center_driver(seam);
        const auto seam_plan = gt::plan_workspace_desktops(seam, seam_center, 1920, 1080);
        check(seam_center == 0 && at(seam_plan, 0) == std::make_pair(0, 0) &&
                  at(seam_plan, 1) == std::make_pair(0, -1080),
              "a stack across the 180 deg yaw seam stays one column");
        // +180 and -180 are the same direction: the stack keeps its side.
        for (const float upper_yaw : {180.0f, -180.0f}) {
            const std::vector<P> behind{{0, 0.0f, 0.0f}, {1, 179.0f, 0.0f}, {2, upper_yaw, 30.0f}};
            const auto behind_plan = gt::plan_workspace_desktops(behind, 0, 1920, 1080);
            check(at(behind_plan, 0) == std::make_pair(0, 0) &&
                      at(behind_plan, 1) == std::make_pair(1920, 0) &&
                      at(behind_plan, 2) == std::make_pair(1920, -1080),
                  upper_yaw > 0 ? "a stack behind keeps its side (+180 encoding)"
                                : "a stack behind keeps its side (-180 encoding)");
        }
    }
    test_portrait_planning();
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
    test_portrait_toggle_reload();
    test_workspace_reload("late arrival", 300, 0);
    test_workspace_reload("never arrives", 99999, 0);
    test_workspace_reload("one removal failure", 99999, 1);
    // Live 2026-11-18: shrinking 5 -> 3 needed a third attempt for the second
    // removal (Windows still re-arranging after the first); a fourth one
    // keeps that from being the last chance.
    test_workspace_reload("three removal failures", 99999, 3);
    test_workspace_reload("permanent removal failure", 99999, 4);

    std::printf("vdd_selftest: laptop re-detach guard\n");
    test_redetach("no internal panel active", false, true, true);
    test_redetach("clean re-detach", true, true, true);
    test_redetach("detach reports an error but lands", true, false, true);
    test_redetach("detach rejected, panel stays attached", true, false, false);
    test_redetach("detach call succeeds, panel stays attached", true, true, false);

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
        const auto moves = gt::plan_aside_moves(snapshots, slots, {});
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
        const auto moves = gt::plan_aside_moves(snapshots, slots, {});
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
        check(gt::plan_aside_moves(snapshots, slots, {L"V"}).empty(),
              "virtual displays are excluded from the aside plan");
    }
    {
        // Mid-session grow to a screen stacked above the centre: its slot
        // (0,-1080) covers the glasses' home, and the just-added VDD still sits
        // where Windows dropped it, far right. The glasses must land flush right
        // of the planned slots (3840), not past the stray VDD (5760).
        const std::vector<gt::DisplayModeSnapshot> snapshots = {
            make_snapshot(L"V0", -1920, 0, 1920, 1080, false),
            make_snapshot(L"V1", 0, 0, 1920, 1080, true),
            make_snapshot(L"V2", 1920, 0, 1920, 1080, false),
            make_snapshot(L"V3", 3840, 0, 1920, 1080, false),
            make_snapshot(L"GLASSES", 457, -1080, 1920, 1080, false),
        };
        const std::vector<gt::PlannedDesktop> slots = {
            {0, -1920, 0}, {1, 0, 0}, {2, 1920, 0}, {3, 0, -1080}};
        const auto moves =
            gt::plan_aside_moves(snapshots, slots, {L"V0", L"V1", L"V2", L"V3"});
        std::printf("  stacked grow aside: %zu move(s)%s%d\n", moves.size(),
                    moves.empty() ? "" : " glasses x=", moves.empty() ? 0 : moves[0].x);
        check(moves.size() == 1 && moves[0].device_name == L"GLASSES" && moves[0].x == 3840 &&
                  moves[0].y == 0,
              "a stacked slot moves the glasses flush right of the slots, ignoring stray VDDs");
    }
    {
        // A portrait right slot is 1080 wide: the aside lands flush at 3000,
        // not at 3840 (a gap Windows would pack, failing the exact readback).
        const std::vector<gt::DisplayModeSnapshot> snapshots = {
            make_snapshot(L"LAPTOP", 0, 0, 2880, 1800, true),
        };
        const std::vector<gt::PlannedDesktop> slots = {
            {0, -1920, 0}, {1, 0, 0}, {2, 1920, -420, 1080, 1920, true}};
        const auto moves = gt::plan_aside_moves(snapshots, slots, {});
        std::printf("  portrait right aside: x=%d\n", moves.empty() ? 0 : moves[0].x);
        check(moves.size() == 1 && moves[0].x == 3000 && moves[0].y == 0,
              "a portrait slot's own width places the aside flush right of it");
    }
    {
        // A display below the origin row that only a 1920-tall portrait centre
        // reaches must move aside (a 1080-tall slot would miss it).
        const std::vector<gt::DisplayModeSnapshot> snapshots = {
            make_snapshot(L"LAPTOP", 0, -1800, 2880, 1800, true),
            make_snapshot(L"GLASSES", 0, 1200, 1920, 1080, false),
        };
        const std::vector<gt::PlannedDesktop> slots = {
            {0, -1920, 420}, {1, 0, 0, 1080, 1920, true}, {2, 1080, 420}};
        const auto moves = gt::plan_aside_moves(snapshots, slots, {});
        check(moves.size() == 2 && moves[1].device_name == L"GLASSES" && moves[0].x == 3000 &&
                  moves[1].x == 5880,
              "a portrait centre's full height decides which displays move aside");
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

    test_display_scale();
    test_display_scale_backup();

    std::printf("vdd_selftest: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
