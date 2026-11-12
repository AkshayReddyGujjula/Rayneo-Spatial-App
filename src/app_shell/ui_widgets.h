#pragma once

// Immediate-mode drawing helpers plus the hotspot list that makes the
// custom-drawn UI interactive.
//
// The window paints itself into an off-screen bitmap and records one Hotspot
// per interactive element. Mouse and keyboard handling then work off that
// list, which keeps the drawing code declarative and avoids a retained widget
// tree. Hotspot ids are stable enums (see app_state.h), so keyboard focus
// survives repaints.
//
// Shapes are drawn anti-aliased with GDI+ (rounded rectangles, circles,
// lines); text stays on GDI with ClearType. A GdiplusSession must be alive
// for the lifetime of any Canvas.

#include "app_shell/ui_theme.h"

#include <windows.h>

#include <string>
#include <vector>

namespace gt::ui {

// RAII GdiplusStartup/GdiplusShutdown. One per process (main.cpp and the
// offscreen render tool).
class GdiplusSession {
public:
    GdiplusSession();
    ~GdiplusSession();
    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;
    bool ok() const { return token_ != 0; }

private:
    ULONG_PTR token_ = 0;
};

struct Hotspot {
    int id = 0;
    int arg = 0;
    RECT rect{};
    bool enabled = true;
    // Scroll portal that owns this hotspot (a ScrollPanel value from
    // app_state.h, or -1 outside any scrollable panel). full is the
    // unclipped rect: drag math must use it, hit tests use rect.
    int panel = -1;
    RECT full{};
};

enum class ButtonStyle {
    Primary,
    Secondary,
    Danger,
    Ghost,
};

// Interaction state of one widget for this frame: eased hover and press
// amounts (0..1, from the Animator), keyboard focus (only drawn when focus
// is visible) and whether it accepts input.
struct WidgetState {
    float hover = 0.0f;
    float press = 0.0f;
    bool focused = false;
    bool enabled = true;
};

class Canvas {
public:
    Canvas(HDC dc, const RECT& bounds, const Theme& theme, UINT dpi)
        : dc_(dc), bounds_(bounds), theme_(&theme), dpi_(dpi) {}

    HDC dc() const { return dc_; }
    RECT bounds() const { return bounds_; }
    int width() const { return bounds_.right - bounds_.left; }
    int height() const { return bounds_.bottom - bounds_.top; }
    const Theme& theme() const { return *theme_; }
    UINT dpi() const { return dpi_; }
    int px(int value) const { return scale_px(value, dpi_); }

    void fill(const RECT& rect, COLORREF color) const;
    // Anti-aliased shapes. alpha is 0..255 over whatever is already drawn.
    void fill_round(const RECT& rect, int radius, COLORREF color, int alpha = 255) const;
    void outline_round(const RECT& rect, int radius, float thickness, COLORREF color,
                       int alpha = 255) const;
    void fill_circle(int center_x, int center_y, float r, COLORREF color, int alpha = 255) const;
    void ring(int center_x, int center_y, float r, float thickness, COLORREF color,
              int alpha = 255) const;
    void line(float x0, float y0, float x1, float y1, float thickness, COLORREF color,
              int alpha = 255) const;
    void polyline(const POINT* points, int count, float thickness, COLORREF color) const;
    void fill_polygon(const POINT* points, int count, COLORREF color, int alpha = 255) const;
    void arc(int center_x, int center_y, float r, float start_deg, float sweep_deg, float thickness,
             COLORREF color) const;
    // Soft drop shadow under a card (a few stacked translucent rounds).
    void shadow(const RECT& rect, int radius, int depth) const;
    void separator(const RECT& rect, COLORREF color) const;
    void text(HFONT font, COLORREF color, const RECT& rect, const std::wstring& value,
              UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE) const;
    void push_clip(const RECT& rect);
    void pop_clip();

private:
    HDC dc_;
    RECT bounds_;
    const Theme* theme_;
    UINT dpi_;
    std::vector<int> clip_stack_;
};

struct FontSet;

void draw_focus_ring(Canvas& canvas, const RECT& rect, int radius);
void draw_button(Canvas& canvas, const RECT& rect, const std::wstring& label, ButtonStyle style,
                 const WidgetState& state, HFONT font, const wchar_t* icon = nullptr,
                 HFONT icon_font = nullptr);
// Square icon-only button (toolbar style).
void draw_icon_button(Canvas& canvas, const RECT& rect, const wchar_t* icon, HFONT icon_font,
                      const WidgetState& state, bool active = false);
void draw_chip(Canvas& canvas, const RECT& rect, const std::wstring& label, bool selected,
               const WidgetState& state, HFONT font, COLORREF dot = CLR_INVALID);
// Minimalist hover-help mark: a small faint "?", no chip chrome. Brightens
// on hover; keyboard focus keeps a subtle ring.
void draw_help_mark(Canvas& canvas, const RECT& rect, const WidgetState& state, HFONT font);
inline int help_mark_width(UINT dpi) { return scale_px(size::help, dpi); }
// Label row (label left, value right) above a track with an eased knob.
void draw_slider(Canvas& canvas, const RECT& rect, const std::wstring& label,
                 const std::wstring& value_text, float fraction, const WidgetState& state,
                 HFONT label_font, HFONT value_font);
// iOS-style switch; on is the eased 0..1 thumb position.
void draw_switch(Canvas& canvas, const RECT& track, float on, const WidgetState& state);
// Segmented control: equal segments with a sliding selection pill at the
// eased (fractional) index. hover_index/focus_index are -1 for none.
RECT segment_rect(const RECT& rect, int count, int index);
void draw_segmented(Canvas& canvas, const RECT& rect, const wchar_t* const* labels, int count,
                    float selected_pos, int hover_index, int focus_index, bool enabled,
                    HFONT font);
void draw_card(Canvas& canvas, const RECT& rect);
void draw_dot(Canvas& canvas, int center_x, int center_y, int radius, COLORREF color);
// Status dot with an optional expanding pulse ring (phase 0..1, or < 0).
void draw_status_dot(Canvas& canvas, int center_x, int center_y, int radius, COLORREF color,
                     float pulse_phase);
void draw_badge(Canvas& canvas, const RECT& rect, const std::wstring& label, COLORREF color,
                HFONT font);
void draw_progress_bar(Canvas& canvas, const RECT& rect, float fraction, COLORREF color);
void draw_icon(Canvas& canvas, const RECT& rect, const wchar_t* glyph, HFONT font, COLORREF color);
void draw_scrollbar(Canvas& canvas, const RECT& track_rect, const RECT& thumb_rect, float hover);

std::wstring ellipsize(const std::wstring& text, size_t max_characters);
// Wrapped-text height for layout measurement (DrawText DT_CALCRECT; nothing
// is painted).
int wrapped_text_height(HDC dc, HFONT font, int width, const std::wstring& text);
int text_width(HDC dc, HFONT font, const std::wstring& text);
std::wstring format_wide(const wchar_t* format, ...);

}  // namespace gt::ui
