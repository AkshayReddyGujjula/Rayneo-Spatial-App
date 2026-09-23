#pragma once

// Custom-drawn dashboard + layout editor.
//
// paint_window() fills the client area and records one hotspot per interactive
// element; the geometry helpers are shared with the input handling so a drag in
// the editor uses exactly the same projection that was drawn.

#include "app_shell/app_state.h"
#include "app_shell/ui_theme.h"
#include "app_shell/ui_widgets.h"

#include <windows.h>

#include <vector>

namespace gt::ui {

struct PaintContext {
    // Mutable: the paint pass refreshes the scroll-portal geometry cache.
    AppState* state = nullptr;
    const FontSet* fonts = nullptr;
    UINT dpi = 96;
    RECT client{};
};

void paint_window(Canvas& canvas, const PaintContext& context, std::vector<Hotspot>& hotspots);

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
