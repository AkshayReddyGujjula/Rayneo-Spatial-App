#include "app_shell/ui_widgets.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace gt::ui {
namespace {

COLORREF lighten(COLORREF color, int amount) {
    const int red = std::min(255, static_cast<int>(GetRValue(color)) + amount);
    const int green = std::min(255, static_cast<int>(GetGValue(color)) + amount);
    const int blue = std::min(255, static_cast<int>(GetBValue(color)) + amount);
    return RGB(red, green, blue);
}

COLORREF mix(COLORREF first, COLORREF second, int second_percent) {
    const int percent = std::clamp(second_percent, 0, 100);
    const int red = (GetRValue(first) * (100 - percent) + GetRValue(second) * percent) / 100;
    const int green = (GetGValue(first) * (100 - percent) + GetGValue(second) * percent) / 100;
    const int blue = (GetBValue(first) * (100 - percent) + GetBValue(second) * percent) / 100;
    return RGB(red, green, blue);
}

void draw_text_in(HDC dc, HFONT font, COLORREF color, const RECT& rect, const std::wstring& value,
                  UINT format) {
    const HGDIOBJ previous = SelectObject(dc, font);
    const int previous_mode = SetBkMode(dc, TRANSPARENT);
    const COLORREF previous_color = SetTextColor(dc, color);
    RECT target = rect;
    UINT effective = format | DT_NOPREFIX;
    if ((format & DT_WORDBREAK) == 0) {
        effective |= DT_SINGLELINE | DT_VCENTER;
    }
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &target, effective);
    SetTextColor(dc, previous_color);
    SetBkMode(dc, previous_mode);
    SelectObject(dc, previous);
}

}  // namespace

void Canvas::fill(const RECT& rect, COLORREF color) const {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc_, &rect, brush);
    DeleteObject(brush);
}

void Canvas::fill_round(const RECT& rect, int radius, COLORREF color) const {
    HBRUSH brush = CreateSolidBrush(color);
    const HGDIOBJ previous_brush = SelectObject(dc_, brush);
    const HGDIOBJ previous_pen = SelectObject(dc_, GetStockObject(NULL_PEN));
    RoundRect(dc_, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc_, previous_pen);
    SelectObject(dc_, previous_brush);
    DeleteObject(brush);
}

void Canvas::outline_round(const RECT& rect, int radius, int thickness, COLORREF color) const {
    HPEN pen = CreatePen(PS_SOLID, thickness, color);
    const HGDIOBJ previous_pen = SelectObject(dc_, pen);
    const HGDIOBJ previous_brush = SelectObject(dc_, GetStockObject(NULL_BRUSH));
    RoundRect(dc_, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc_, previous_brush);
    SelectObject(dc_, previous_pen);
    DeleteObject(pen);
}

void Canvas::separator(const RECT& rect, COLORREF color) const {
    RECT line = rect;
    if (line.bottom - line.top > 1) {
        line.bottom = line.top + 1;
    }
    fill(line, color);
}

void Canvas::text(HFONT font, COLORREF color, const RECT& rect, const std::wstring& value,
                  UINT format) const {
    draw_text_in(dc_, font, color, rect, value, format);
}

void Canvas::push_clip(const RECT& rect) {
    const int saved = SaveDC(dc_);
    IntersectClipRect(dc_, rect.left, rect.top, rect.right, rect.bottom);
    clip_stack_.push_back(saved);
    ++clip_depth_;
}

void Canvas::pop_clip() {
    if (!clip_stack_.empty()) {
        RestoreDC(dc_, clip_stack_.back());
        clip_stack_.pop_back();
        if (clip_depth_ > 0) {
            --clip_depth_;
        }
    }
}

void draw_button(Canvas& canvas, const RECT& rect, const std::wstring& label, ButtonStyle style,
                 bool focused, bool hovered, bool enabled, HFONT font) {
    COLORREF background = palette::panel_alt;
    COLORREF border = palette::border;
    COLORREF label_color = palette::text;
    switch (style) {
        case ButtonStyle::Primary:
            background = enabled ? palette::accent : mix(palette::panel_alt, palette::border, 60);
            border = enabled ? lighten(palette::accent, 24) : palette::border;
            label_color = enabled ? RGB(9, 14, 24) : palette::disabled_text;
            break;
        case ButtonStyle::Secondary:
            background = hovered && enabled ? lighten(palette::panel_alt, 12) : palette::panel_alt;
            border = palette::border_strong;
            label_color = enabled ? palette::text : palette::disabled_text;
            break;
        case ButtonStyle::Danger:
            background = hovered && enabled ? mix(palette::panel_alt, palette::error, 35)
                                            : palette::panel_alt;
            border = mix(palette::border, palette::error, 55);
            label_color = enabled ? palette::error : palette::disabled_text;
            break;
        case ButtonStyle::Ghost:
            background = hovered && enabled ? palette::panel_alt : palette::panel;
            border = palette::border;
            label_color = enabled ? palette::text_dim : palette::disabled_text;
            break;
    }
    if (style == ButtonStyle::Primary && hovered && enabled) {
        background = lighten(background, 18);
    }
    if (!enabled) {
        background = mix(background, palette::panel, 45);
    }
    const int radius = 6;
    canvas.fill_round(rect, radius, background);
    canvas.outline_round(rect, radius, 1, focused ? palette::focus : border);
    if (focused) {
        RECT ring = rect;
        InflateRect(&ring, -2, -2);
        canvas.outline_round(ring, radius - 1, 1, mix(palette::focus, background, 40));
    }
    canvas.text(font, label_color, rect, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_chip(Canvas& canvas, const RECT& rect, const std::wstring& label, COLORREF accent,
               bool focused, bool hovered, HFONT font) {
    const COLORREF background = hovered ? mix(palette::panel_alt, accent, 18) : palette::panel_alt;
    canvas.fill_round(rect, (rect.bottom - rect.top) / 2, background);
    canvas.outline_round(rect, (rect.bottom - rect.top) / 2, 1,
                         focused ? palette::focus : mix(palette::border, accent, 45));
    canvas.text(font, palette::text, rect, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_help_mark(Canvas& canvas, const RECT& rect, bool focused, bool hovered, HFONT font) {
    if (hovered) {
        canvas.fill_round(rect, (rect.bottom - rect.top) / 2, palette::accent_soft);
    }
    const COLORREF color = hovered ? palette::accent : palette::text_faint;
    canvas.text(font, color, rect, L"?", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (focused) {
        canvas.outline_round(rect, (rect.bottom - rect.top) / 2, 1, palette::focus);
    }
}

void draw_slider(Canvas& canvas, const RECT& rect, const std::wstring& label,
                 const std::wstring& value_text, float fraction, bool focused, bool hovered,
                 bool enabled, HFONT label_font, HFONT value_font) {
    const int label_height = (rect.bottom - rect.top) * 45 / 100;
    RECT label_band = rect;
    label_band.bottom = rect.top + label_height;
    // The value owns the right part of the label band, so it can never sit
    // under the track or the knob.
    RECT label_rect = label_band;
    label_rect.right = label_band.left + (label_band.right - label_band.left) * 62 / 100;
    RECT value_rect = label_band;
    value_rect.left = label_rect.right;

    const COLORREF label_color = enabled ? palette::text_dim : palette::disabled_text;
    canvas.text(label_font, label_color, label_rect, label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    canvas.text(value_font, enabled ? palette::text : palette::disabled_text, value_rect,
                value_text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT track = rect;
    track.top = rect.top + label_height;
    track.bottom = rect.bottom;
    const int track_height = std::max(3, static_cast<int>((track.bottom - track.top) / 4));
    RECT line = track;
    line.top = track.top + (track.bottom - track.top - track_height) / 2;
    line.bottom = line.top + track_height;
    canvas.fill_round(line, track_height / 2,
                      enabled ? palette::border_strong : palette::border);
    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    RECT filled = line;
    filled.right = line.left + static_cast<int>((line.right - line.left) * clamped);
    if (filled.right > filled.left + track_height) {
        canvas.fill_round(filled, track_height / 2,
                          enabled ? palette::accent_dim : palette::border);
    }
    const int knob_radius = std::max(4, static_cast<int>((track.bottom - track.top) / 5));
    const int knob_x = line.left + static_cast<int>((line.right - line.left) * clamped);
    const int knob_y = (line.top + line.bottom) / 2;
    RECT knob{knob_x - knob_radius, knob_y - knob_radius, knob_x + knob_radius,
              knob_y + knob_radius};
    canvas.fill_round(knob, knob_radius, enabled ? palette::accent : palette::border_strong);
    if (focused) {
        RECT ring = knob;
        InflateRect(&ring, 3, 3);
        canvas.outline_round(ring, knob_radius + 2, 1, palette::focus);
    } else if (hovered && enabled) {
        RECT ring = knob;
        InflateRect(&ring, 2, 2);
        canvas.outline_round(ring, knob_radius + 1, 1, mix(palette::focus, palette::panel, 50));
    }
}

void draw_scrollbar(Canvas& canvas, const RECT& track_rect, const RECT& thumb_rect, bool hovered) {
    canvas.fill_round(track_rect, (track_rect.right - track_rect.left) / 2, palette::panel_alt);
    RECT thumb = thumb_rect;
    if (thumb.bottom - thumb.top < 8) {
        thumb.bottom = thumb.top + 8;
    }
    canvas.fill_round(thumb, (thumb.right - thumb.left) / 2,
                      hovered ? palette::accent_dim : palette::border_strong);
}

int wrapped_text_height(HDC dc, HFONT font, int width, const std::wstring& text) {
    if (width <= 0 || text.empty()) {
        return 0;
    }
    const HGDIOBJ previous = SelectObject(dc, font);
    RECT measure{0, 0, width, 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measure,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, previous);
    return measure.bottom - measure.top;
}

void draw_dot(Canvas& canvas, int center_x, int center_y, int radius, COLORREF color) {
    RECT rect{center_x - radius, center_y - radius, center_x + radius, center_y + radius};
    canvas.fill_round(rect, radius, color);
}

void draw_toggle(Canvas& canvas, const RECT& rect, const std::wstring& label, bool value,
                 bool focused, bool hovered, HFONT font) {
    const int box = std::min(18, static_cast<int>(rect.bottom - rect.top));
    RECT box_rect{rect.left, rect.top + ((rect.bottom - rect.top) - box) / 2,
                  rect.left + box, rect.top + ((rect.bottom - rect.top) - box) / 2 + box};
    canvas.fill_round(box_rect, 4,
                      value ? palette::accent : (hovered ? palette::panel_alt : palette::panel));
    canvas.outline_round(box_rect, 4, 1, focused ? palette::focus : palette::border_strong);
    if (value) {
        // A small check mark drawn as two strokes.
        const int x0 = box_rect.left + box / 5;
        const int y0 = box_rect.top + box / 2;
        const int x1 = box_rect.left + box / 2 - 1;
        const int y1 = box_rect.bottom - box / 4;
        const int x2 = box_rect.right - box / 5;
        const int y2 = box_rect.top + box / 4;
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(9, 14, 24));
        const HGDIOBJ previous = SelectObject(canvas.dc(), pen);
        MoveToEx(canvas.dc(), x0, y0, nullptr);
        LineTo(canvas.dc(), x1, y1);
        LineTo(canvas.dc(), x2, y2);
        SelectObject(canvas.dc(), previous);
        DeleteObject(pen);
    }
    RECT label_rect = rect;
    label_rect.left = box_rect.right + 8;
    canvas.text(font, hovered ? palette::text : palette::text_dim, label_rect, label,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

std::wstring ellipsize(const std::wstring& text, size_t max_characters) {
    if (text.size() <= max_characters || max_characters < 4) {
        return text;
    }
    std::wstring out = text.substr(0, max_characters - 3);
    out += L"...";
    return out;
}

std::wstring format_wide(const wchar_t* format, ...) {
    wchar_t buffer[512];
    va_list arguments;
    va_start(arguments, format);
    const int written = _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, arguments);
    va_end(arguments);
    if (written <= 0) {
        return std::wstring();
    }
    return std::wstring(buffer, static_cast<size_t>(written));
}

}  // namespace gt::ui
