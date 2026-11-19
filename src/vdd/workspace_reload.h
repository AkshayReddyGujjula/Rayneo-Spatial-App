#pragma once

#include "layout/layout.h"
#include "vdd/display_config.h"

#include <functional>
#include <optional>

namespace gt {

struct WorkspacePlan {
    int center_driver = -1;
    std::vector<PlannedDesktop> desktops;  // VDD binding rank
};

bool plan_workspace_for_layout(const Layout& layout, const std::vector<int>& driver_indices,
                               WorkspacePlan& plan, std::string& error);
// Pure preflight (no display calls): true when the layout can be made into
// Windows desktops by the same planner and tiling check the engine uses.
// Otherwise `reason` says why. The controller refuses to save such a layout
// and the engine refuses it before any VDD is added, removed or moved.
bool workspace_layout_is_arrangeable(const Layout& layout, std::string& reason);
std::vector<ConfiguredDisplay> bind_displays_to_screens(
    const Layout& layout, const std::vector<ConfiguredDisplay>& displays);

// Old screen slot for each exact output/mode match. An empty device name
// represents a capture that the caller has already released.
std::vector<std::optional<size_t>> reusable_capture_sources(
    const std::vector<ConfiguredDisplay>& previous, const std::vector<ConfiguredDisplay>& next);

struct WorkspaceReloadOps {
    std::function<const std::vector<int>&()> indices;
    std::function<bool(size_t, std::string&)> resize;
    std::function<bool(const std::vector<int>&, int, const char*,
                       std::vector<VirtualDisplayInfo>&, std::string&)> wait;
    std::function<bool(const WorkspacePlan&, const std::map<int, std::wstring>&,
                       std::vector<ConfiguredDisplay>&, std::string&)> reposition;
    std::function<bool(const std::vector<ConfiguredDisplay>&, std::string&)> apply_layout;
    std::function<bool(const std::vector<ConfiguredDisplay>&, std::string&)> restore_captures;
};

// Apply the candidate or recover the previous workspace. On rejection,
// rollback_error is nonempty only if recovery itself failed.
bool reload_workspace(const Layout& previous, const Layout& candidate,
                      std::vector<ConfiguredDisplay>& configured, const WorkspaceReloadOps& ops,
                      std::string& error, std::string& rollback_error);

}  // namespace gt
