#pragma once

// Draws the live calibration guide (calibration_guide.h) onto a Canvas: the
// red line along the step's motion, the ball at the head's live position,
// target marks, the steadiness ring, repeat dots and the instruction text.
// One painter, two styles: the glasses window (black = see-through on the
// birdbath optics, large type) and the PC calibration panel (theme tokens).

#include "app_shell/calibration_guide.h"
#include "app_shell/ui_theme.h"
#include "app_shell/ui_widgets.h"

#include <windows.h>

namespace gt::ui {

struct CalGuideStyle {
    bool glasses = false;
    bool show_text = true;  // headline / subline / tip inside the guide
    COLORREF background = 0;
    COLORREF line = 0;         // the red guide line
    COLORREF track = 0;        // faint reference lines
    COLORREF good = 0;
    COLORREF warn = 0;
    COLORREF neutral = 0;
    COLORREF text = 0;
    COLORREF text_dim = 0;
    COLORREF tick = 0;         // the 12 deg gate mark
    COLORREF target = 0;       // the recommended target mark
    HFONT headline = nullptr;
    HFONT sub = nullptr;
    HFONT small = nullptr;
    HFONT countdown = nullptr;
};

// Large type for the glasses window, sized from its height in pixels.
struct GlassesGuideFonts {
    HFONT headline = nullptr;
    HFONT sub = nullptr;
    HFONT small = nullptr;
    HFONT countdown = nullptr;
    int height = 0;
    void create(int area_height);
    void destroy();
    ~GlassesGuideFonts() { destroy(); }
};

CalGuideStyle panel_guide_style(const Theme& theme, const FontSet& fonts);
// Glasses colours; the fonts are the caller's (sized to the glasses height).
CalGuideStyle glasses_guide_style(const GlassesGuideFonts& fonts);

// ball (optional) overrides the computed ball position (the glasses window
// eases it between live lines).
void paint_cal_guide(Canvas& canvas, const RECT& area, const CalGuideView& view, const CalGuideStyle& style,
                     float fov_h_deg, const CalGuidePoint* ball = nullptr);

// The guide region (below the text band) and its ball, so a caller can ease
// the ball toward the live target.
RECT cal_guide_region(const RECT& area, const CalGuideStyle& style);
CalGuidePoint cal_guide_target_ball(const RECT& area, const CalGuideView& view, const CalGuideStyle& style,
                                    float fov_h_deg);

}  // namespace gt::ui
