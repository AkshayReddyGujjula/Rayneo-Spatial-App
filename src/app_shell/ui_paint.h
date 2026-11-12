#pragma once

// Custom-drawn controller UI: a sidebar with pages (Home, Layout, Comfort,
// Tracking, Diagnostics, Settings), the first-run setup guide overlay, and
// the arrangement editor.
//
// paint_window() fills the client area and records one hotspot per
// interactive element; the geometry helpers are shared with the input
// handling so a drag in the editor uses exactly the same projection that was
// drawn. The same function renders the offscreen snapshots (ui_snapshot).

#include "app_shell/app_state.h"
#include "app_shell/ui_theme.h"
#include "app_shell/ui_widgets.h"

#include <windows.h>

#include <vector>

namespace gt::ui {

struct PaintContext {
    // Mutable: the paint pass refreshes the scroll-portal geometry cache and
    // registers animation targets.
    AppState* state = nullptr;
    const FontSet* fonts = nullptr;
    UINT dpi = 96;
    RECT client{};
};

const Theme& current_theme(const AppState& state);
void paint_window(Canvas& canvas, const PaintContext& context, std::vector<Hotspot>& hotspots);
// Width of the navigation rail for a client width (compact below ~1000 px).
int sidebar_width(int client_width, UINT dpi);

struct EditorGeometry {
    POINT head{};
    float scale = 1.0f;  // pixels per metre
};

EditorGeometry editor_geometry(const Layout& layout, const RECT& rect, UINT dpi);
POINT editor_screen_center(const ScreenLayout& screen, const EditorGeometry& geometry);
int editor_screen_hit_test(const Layout& layout, const EditorGeometry& geometry, POINT point,
                           int threshold_px);
float editor_yaw_from_point(const EditorGeometry& geometry, POINT point);

}  // namespace gt::ui
