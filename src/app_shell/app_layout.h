#pragma once

// Layout editing rules shared by the dashboard, the visual editor and the
// selftest. Nothing here knows about windows: it mutates a gt::Layout in memory
// and always leaves it inside the gt::validate_layout contract, so the bytes
// written by the controller are always loadable by spatial_desk.exe.

#include "layout/layout.h"

#include <string>
#include <vector>

namespace gt {

enum class LayoutPreset {
    Single,
    TripleArc,
    QuadArc,
    FiveArc,
    WideArc,
};

const char* layout_preset_name(LayoutPreset preset);
const char* layout_preset_label(LayoutPreset preset);
std::vector<LayoutPreset> layout_presets();
Layout preset_layout(LayoutPreset preset);

// Screen add/remove keep ids and vdd indices unique: ids are generated from the
// lowest free "screen-N", vdd indices from the lowest free slot in 0..15, and a
// removed index becomes available again.
bool add_screen(Layout& layout, std::string& error);
bool remove_screen(Layout& layout, size_t index, std::string& error);
int next_free_vdd_index(const Layout& layout);
std::string next_screen_id(const Layout& layout);

enum class LayoutField {
    Yaw = 0,
    Pitch,
    Roll,
    Distance,
    Width,
    Height,
    Fov,
    ActiveFps,
    MidFps,
    IdleFps,
    EnterDeg,
    LeaveDeg,
    Count,
};

const wchar_t* layout_field_label(LayoutField field);
const char* layout_field_key(LayoutField field);
float layout_field_value(const Layout& layout, const ScreenLayout& screen, LayoutField field);
bool layout_field_range(LayoutField field, float& minimum, float& maximum, float& step);
// Applies the value and normalises every coupled field (capture FPS ordering,
// enter > leave, yaw wrapping) so the result still validates.
bool set_layout_field(Layout& layout, ScreenLayout& screen, LayoutField field, float value,
                      std::string& error);
std::string format_layout_field_value(LayoutField field, float value);
std::string layout_summary(const Layout& layout);

// Named user presets: validated Layout files stored as <name>.json inside a
// presets directory (normally <config>/layouts/presets). The engine keeps
// loading the single live layout file; applying a preset copies its content
// into the editor, and the existing save path pushes it live.
bool layout_preset_name_valid(const std::string& name, std::string& error);
std::vector<std::string> list_layout_presets(const std::string& presets_dir);
bool save_layout_preset(const std::string& presets_dir, const std::string& name,
                        const Layout& layout, std::string& error);
bool load_layout_preset(const std::string& presets_dir, const std::string& name, Layout& layout,
                        std::string& error);
bool delete_layout_preset(const std::string& presets_dir, const std::string& name,
                          std::string& error);

}  // namespace gt
