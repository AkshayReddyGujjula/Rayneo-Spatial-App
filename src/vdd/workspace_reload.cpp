#include "vdd/workspace_reload.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace gt {
namespace {

std::vector<size_t> layout_screen_order(const Layout& layout) {
    std::vector<size_t> order(layout.screens.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
        return layout.screens[left].vdd_index < layout.screens[right].vdd_index;
    });
    return order;
}

}  // namespace

bool plan_workspace_for_layout(const Layout& layout, const std::vector<int>& driver_indices,
                               WorkspacePlan& plan, std::string& error) {
    if (layout.screens.size() != driver_indices.size()) {
        error = "capture binding requires one virtual display per layout screen";
        return false;
    }
    constexpr float kRadToDeg = 57.29577951f;
    std::vector<ScreenPlacement> placements;
    const auto order = layout_screen_order(layout);
    for (size_t rank = 0; rank < order.size(); ++rank) {
        const ScreenLayout& screen = layout.screens[order[rank]];
        // Layout validation keeps distance >= 0.25 m and sizes >= 0.1 m.
        placements.push_back(
            {driver_indices[rank], screen.yaw_deg, screen.pitch_deg,
             2.0f * std::atan(0.5f * screen.width_m / screen.distance_m) * kRadToDeg,
             2.0f * std::atan(0.5f * screen.height_m / screen.distance_m) * kRadToDeg});
    }
    plan.center_driver = select_center_driver(placements);
    if (plan.center_driver < 0) {
        error = "no virtual displays bound";
        return false;
    }
    plan.desktops = plan_workspace_desktops(placements, plan.center_driver, 1920, 1080);
    return true;
}

std::vector<ConfiguredDisplay> bind_displays_to_screens(
    const Layout& layout, const std::vector<ConfiguredDisplay>& displays) {
    if (layout.screens.size() != displays.size()) return {};
    const auto order = layout_screen_order(layout);
    std::vector<ConfiguredDisplay> by_screen(displays.size());
    for (size_t rank = 0; rank < order.size(); ++rank) {
        by_screen[order[rank]] = displays[rank];
    }
    return by_screen;
}

std::vector<std::optional<size_t>> reusable_capture_sources(
    const std::vector<ConfiguredDisplay>& previous, const std::vector<ConfiguredDisplay>& next) {
    std::vector<std::optional<size_t>> sources(next.size());
    for (size_t i = 0; i < next.size(); ++i) {
        if (next[i].device_name.empty()) continue;
        const auto old = std::find(previous.begin(), previous.end(), next[i]);
        if (old != previous.end()) sources[i] = static_cast<size_t>(old - previous.begin());
    }
    return sources;
}

bool reload_workspace(const Layout& previous, const Layout& candidate,
                      std::vector<ConfiguredDisplay>& configured, const WorkspaceReloadOps& ops,
                      std::string& error, std::string& rollback_error) {
    const auto original_indices = ops.indices();
    WorkspacePlan previous_plan;
    if (!plan_workspace_for_layout(previous, original_indices, previous_plan, error)) {
        rollback_error = "virtual display rollback failed: " + error;
        return false;
    }

    const auto position = [&](const WorkspacePlan& plan, const char* phase,
                              std::vector<ConfiguredDisplay>& displays, std::string& detail) {
        std::vector<VirtualDisplayInfo> active;
        return ops.wait(ops.indices(), kVirtualDisplayWaitMs, phase, active, detail) &&
               ops.reposition(plan, active_virtual_display_names(active), displays, detail);
    };

    const bool resize_needed = candidate.screens.size() != original_indices.size();
    WorkspacePlan next_plan;
    auto next = configured;
    bool ready = (!resize_needed || ops.resize(candidate.screens.size(), error)) &&
                 plan_workspace_for_layout(candidate, ops.indices(), next_plan, error);
    const bool reposition_needed = ready &&
        (resize_needed || next_plan.desktops != previous_plan.desktops ||
         next_plan.center_driver != previous_plan.center_driver);
    if (reposition_needed) ready = position(next_plan, "reload", next, error);
    if (ready && ops.apply_layout(next, error)) {
        configured = std::move(next);
        return true;
    }

    std::string detail;
    if (resize_needed || reposition_needed) {
        std::vector<int> added;
        for (int index : ops.indices()) {
            if (std::find(original_indices.begin(), original_indices.end(), index) ==
                original_indices.end()) {
                added.push_back(index);
            }
        }
        if (!added.empty()) {
            // The add IOCTL can finish before Windows attaches the monitor.
            // Give late outputs another chance to settle before removing them.
            // A missing output still has a driver index that must be removed.
            std::vector<VirtualDisplayInfo> settled;
            ops.wait(added, 1500, "reload rollback settle", settled, detail);
            detail.clear();
        }
        // A failed shrink may have already removed some displays. Replan
        // against the restored indices because replacement IDs can differ.
        if ((ops.indices().size() != original_indices.size() &&
             !ops.resize(original_indices.size(), detail)) ||
            !plan_workspace_for_layout(previous, ops.indices(), previous_plan, detail) ||
            !position(previous_plan, "reload rollback", configured, detail)) {
            rollback_error = "virtual display rollback failed: " + detail;
            return false;
        }
    }
    if (!ops.restore_captures(configured, detail)) {
        rollback_error = "desktop capture rollback failed: " + detail;
    } else if (resize_needed || reposition_needed) {
        std::printf("  layout reload rollback restored %zu virtual displays\n", configured.size());
    }
    return false;
}

}  // namespace gt
