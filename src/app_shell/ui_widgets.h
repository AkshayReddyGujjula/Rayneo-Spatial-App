#pragma once

// Minimal immediate-mode drawing helpers over GDI plus the hotspot list that
// makes the custom-drawn UI interactive.
//
// The window paints itself into an off-screen bitmap and records one Hotspot
// per interactive element. Mouse and keyboard handling then work off that list,
// which keeps the drawing code declarative and avoids a retained widget tree.
// Hotspot ids are stable enums (see app_state.h), so keyboard focus survives
// repaints.

#include "app_shell/ui_theme.h"

#include <windows.h>

#include <string>
#include <vector>

namespace gt::ui {

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

class Canvas {
public:
    Canvas(HDC dc, const RECT& bounds) : dc_(dc), bounds_(bounds) {}

    HDC dc() const { return dc_; }
    RECT bounds() const { return bounds_; }
    int width() const { return bounds_.right - bounds_.left; }
    int height() const { return bounds_.bottom - bounds_.top; }

    void fill(const RECT& rect, COLORREF color) const;
    void fill_round(const RECT& rect, int radius, COLORREF color) const;
    void outline_round(const RECT& rect, int radius, int thickness, COLORREF color) const;
    void separator(const RECT& rect, COLORREF color) const;
    void text(HFONT font, COLORREF color, const RECT& rect, const std::wstring& value,
              UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE) const;
    void push_clip(const RECT& rect);
    void pop_clip();

private:
    HDC dc_;
    RECT bounds_;
    int clip_depth_ = 0;
    std::vector<int> clip_stack_;
};

void draw_button(Canvas& canvas, const RECT& rect, const std::wstring& label, ButtonStyle style,
                 bool focused, bool hovered, bool enabled, HFONT font);
void draw_chip(Canvas& canvas, const RECT& rect, const std::wstring& label, COLORREF accent,
               bool focused, bool hovered, HFONT font);
void draw_slider(Canvas& canvas, const RECT& rect, const std::wstring& label,
                 const std::wstring& value_text, float fraction, bool focused, bool hovered,
                 bool enabled, HFONT label_font, HFONT value_font);
void draw_dot(Canvas& canvas, int center_x, int center_y, int radius, COLORREF color);
void draw_toggle(Canvas& canvas, const RECT& rect, const std::wstring& label, bool value,
                 bool focused, bool hovered, HFONT font);
void draw_scrollbar(Canvas& canvas, const RECT& track_rect, const RECT& thumb_rect, bool hovered);

std::wstring ellipsize(const std::wstring& text, size_t max_characters);
// Wrapped-text height for scroll content measurement (DrawText DT_CALCRECT;
// nothing is painted).
int wrapped_text_height(HDC dc, HFONT font, int width, const std::wstring& text);
std::wstring format_wide(const wchar_t* format, ...);

}  // namespace gt::ui
