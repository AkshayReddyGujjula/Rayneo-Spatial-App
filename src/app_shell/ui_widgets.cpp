#include "app_shell/ui_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

// GDI+ expects unqualified min/max, which NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

namespace gt::ui {
namespace {

Gdiplus::Color argb(COLORREF color, int alpha) {
    return Gdiplus::Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)), GetRValue(color),
                          GetGValue(color), GetBValue(color));
}

class Gfx {
public:
    explicit Gfx(HDC dc) : graphics_(dc) {
        graphics_.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics_.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    }
    Gdiplus::Graphics* operator->() { return &graphics_; }
    Gdiplus::Graphics& get() { return graphics_; }

private:
    Gdiplus::Graphics graphics_;
};

void round_path(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    const float d = std::min({r * 2.0f, w, h});
    if (d <= 0.5f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

int width_of(const RECT& r) {
    return static_cast<int>(r.right - r.left);
}
int height_of(const RECT& r) {
    return static_cast<int>(r.bottom - r.top);
}

void draw_text_in(HDC dc, HFONT font, COLORREF color, const RECT& rect, const std::wstring& value,
                  UINT format) {
    const HGDIOBJ previous = SelectObject(dc, font);
    const int previous_mode = SetBkMode(dc, TRANSPARENT);
    const COLORREF previous_color = SetTextColor(dc, color);
    RECT target = rect;
    UINT effective = format | DT_NOPREFIX;
    if ((format & DT_WORDBREAK) == 0) {
        effective |= DT_SINGLELINE;
    }
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &target, effective);
    SetTextColor(dc, previous_color);
    SetBkMode(dc, previous_mode);
    SelectObject(dc, previous);
}

}  // namespace

GdiplusSession::GdiplusSession() {
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token_, &input, nullptr) != Gdiplus::Ok) {
        token_ = 0;
    }
}

GdiplusSession::~GdiplusSession() {
    if (token_ != 0) {
        Gdiplus::GdiplusShutdown(token_);
    }
}

void Canvas::fill(const RECT& rect, COLORREF color) const {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc_, &rect, brush);
    DeleteObject(brush);
}

void Canvas::fill_round(const RECT& rect, int radius, COLORREF color, int alpha) const {
    if (rect.right <= rect.left || rect.bottom <= rect.top || alpha <= 0) {
        return;
    }
    Gfx gfx(dc_);
    Gdiplus::GraphicsPath path;
    round_path(path, static_cast<float>(rect.left), static_cast<float>(rect.top),
               static_cast<float>(width_of(rect)), static_cast<float>(height_of(rect)),
               static_cast<float>(radius));
    Gdiplus::SolidBrush brush(argb(color, alpha));
    gfx->FillPath(&brush, &path);
}

void Canvas::outline_round(const RECT& rect, int radius, float thickness, COLORREF color,
                           int alpha) const {
    if (rect.right <= rect.left || rect.bottom <= rect.top || alpha <= 0) {
        return;
    }
    Gfx gfx(dc_);
    const float inset = thickness / 2.0f;
    Gdiplus::GraphicsPath path;
    round_path(path, static_cast<float>(rect.left) + inset, static_cast<float>(rect.top) + inset,
               static_cast<float>(width_of(rect)) - thickness,
               static_cast<float>(height_of(rect)) - thickness,
               std::max(0.0f, static_cast<float>(radius) - inset));
    Gdiplus::Pen pen(argb(color, alpha), thickness);
    gfx->DrawPath(&pen, &path);
}

void Canvas::fill_circle(int center_x, int center_y, float r, COLORREF color, int alpha) const {
    if (r <= 0.0f || alpha <= 0) {
        return;
    }
    Gfx gfx(dc_);
    Gdiplus::SolidBrush brush(argb(color, alpha));
    gfx->FillEllipse(&brush, static_cast<float>(center_x) - r, static_cast<float>(center_y) - r,
                     r * 2.0f, r * 2.0f);
}

void Canvas::ring(int center_x, int center_y, float r, float thickness, COLORREF color,
                  int alpha) const {
    if (r <= 0.0f || alpha <= 0) {
        return;
    }
    Gfx gfx(dc_);
    Gdiplus::Pen pen(argb(color, alpha), thickness);
    gfx->DrawEllipse(&pen, static_cast<float>(center_x) - r, static_cast<float>(center_y) - r,
                     r * 2.0f, r * 2.0f);
}

void Canvas::line(float x0, float y0, float x1, float y1, float thickness, COLORREF color,
                  int alpha) const {
    Gfx gfx(dc_);
    Gdiplus::Pen pen(argb(color, alpha), thickness);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    gfx->DrawLine(&pen, x0, y0, x1, y1);
}

void Canvas::polyline(const POINT* points, int count, float thickness, COLORREF color) const {
    if (count < 2) {
        return;
    }
    std::vector<Gdiplus::PointF> converted;
    converted.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        converted.emplace_back(static_cast<float>(points[i].x), static_cast<float>(points[i].y));
    }
    Gfx gfx(dc_);
    Gdiplus::Pen pen(argb(color, 255), thickness);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    gfx->DrawLines(&pen, converted.data(), count);
}

void Canvas::fill_polygon(const POINT* points, int count, COLORREF color, int alpha) const {
    if (count < 3) {
        return;
    }
    std::vector<Gdiplus::PointF> converted;
    converted.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        converted.emplace_back(static_cast<float>(points[i].x), static_cast<float>(points[i].y));
    }
    Gfx gfx(dc_);
    Gdiplus::SolidBrush brush(argb(color, alpha));
    gfx->FillPolygon(&brush, converted.data(), count);
}

void Canvas::arc(int center_x, int center_y, float r, float start_deg, float sweep_deg,
                 float thickness, COLORREF color) const {
    if (r <= 0.0f) {
        return;
    }
    Gfx gfx(dc_);
    Gdiplus::Pen pen(argb(color, 255), thickness);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    gfx->DrawArc(&pen, static_cast<float>(center_x) - r, static_cast<float>(center_y) - r,
                 r * 2.0f, r * 2.0f, start_deg, sweep_deg);
}

void Canvas::shadow(const RECT& rect, int radius, int depth) const {
    const int base = theme_->dark ? 34 : 14;
    for (int i = depth; i >= 1; --i) {
        RECT r = rect;
        InflateRect(&r, i, i);
        OffsetRect(&r, 0, (i + 1) / 2);
        fill_round(r, radius + i, theme_->shadow, base / (i + 1));
    }
}

void Canvas::separator(const RECT& rect, COLORREF color) const {
    RECT line_rect = rect;
    if (line_rect.bottom - line_rect.top > 1) {
        line_rect.bottom = line_rect.top + 1;
    }
    fill(line_rect, color);
}

void Canvas::text(HFONT font, COLORREF color, const RECT& rect, const std::wstring& value,
                  UINT format) const {
    draw_text_in(dc_, font, color, rect, value, format);
}

void Canvas::push_clip(const RECT& rect) {
    const int saved = SaveDC(dc_);
    IntersectClipRect(dc_, rect.left, rect.top, rect.right, rect.bottom);
    clip_stack_.push_back(saved);
}

void Canvas::pop_clip() {
    if (!clip_stack_.empty()) {
        RestoreDC(dc_, clip_stack_.back());
        clip_stack_.pop_back();
    }
}

void draw_focus_ring(Canvas& canvas, const RECT& rect, int radius) {
    RECT ring = rect;
    const int out = canvas.px(3);
    InflateRect(&ring, out, out);
    canvas.outline_round(ring, radius + out, static_cast<float>(canvas.px(2)), canvas.theme().focus);
}

void draw_button(Canvas& canvas, const RECT& rect, const std::wstring& label, ButtonStyle style,
                 const WidgetState& state, HFONT font, const wchar_t* icon, HFONT icon_font) {
    const Theme& t = canvas.theme();
    const int radius = canvas.px(radius::md);
    const float hover = state.enabled ? state.hover : 0.0f;
    const float press = state.enabled ? state.press : 0.0f;
    COLORREF background = t.surface_alt;
    int background_alpha = 255;
    COLORREF border = CLR_INVALID;
    COLORREF label_color = t.text;
    switch (style) {
        case ButtonStyle::Primary:
            background = state.enabled ? mix(t.accent_fill, t.accent_hover, hover) : t.surface_alt;
            label_color = state.enabled ? t.accent_text : t.disabled_text;
            break;
        case ButtonStyle::Secondary:
            background = mix(t.surface_alt, t.surface_hover, hover);
            border = t.border;
            label_color = state.enabled ? t.text : t.disabled_text;
            break;
        case ButtonStyle::Danger:
            background = mix(t.surface_alt, mix(t.surface_alt, t.error, 0.22f), hover);
            border = mix(t.border, t.error, 0.35f);
            label_color = state.enabled ? t.error : t.disabled_text;
            break;
        case ButtonStyle::Ghost:
            background = t.surface_hover;
            background_alpha = static_cast<int>(hover * 255.0f);
            label_color = state.enabled ? mix(t.text_dim, t.text, hover) : t.disabled_text;
            break;
    }
    if (press > 0.0f) {
        background = mix(background, t.dark ? RGB(0, 0, 0) : RGB(40, 50, 70), press * 0.18f);
        background_alpha = std::max(background_alpha, static_cast<int>(press * 255.0f));
    }
    if (!state.enabled && style == ButtonStyle::Danger) {
        border = t.border;
    }
    canvas.fill_round(rect, radius, background, background_alpha);
    if (border != CLR_INVALID) {
        canvas.outline_round(rect, radius, 1.0f, border);
    }
    if (state.focused) {
        draw_focus_ring(canvas, rect, radius);
    }
    if (icon != nullptr && icon_font != nullptr) {
        const int icon_w = canvas.px(18);
        const int gap = canvas.px(8);
        const int label_w = label.empty() ? 0 : text_width(canvas.dc(), font, label);
        const int total = icon_w + (label.empty() ? 0 : gap + label_w);
        const int start = (rect.left + rect.right - total) / 2;
        RECT icon_rect{start, rect.top, start + icon_w, rect.bottom};
        canvas.text(icon_font, label_color, icon_rect, icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (!label.empty()) {
            RECT label_rect{icon_rect.right + gap, rect.top, rect.right, rect.bottom};
            canvas.text(font, label_color, label_rect, label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        return;
    }
    canvas.text(font, label_color, rect, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void draw_icon_button(Canvas& canvas, const RECT& rect, const wchar_t* icon, HFONT icon_font,
                      const WidgetState& state, bool active) {
    const Theme& t = canvas.theme();
    const int radius = canvas.px(radius::md);
    if (active) {
        canvas.fill_round(rect, radius, t.accent_soft);
    }
    canvas.fill_round(rect, radius, t.surface_hover, static_cast<int>(state.hover * 255.0f));
    const COLORREF color = !state.enabled ? t.disabled_text
                           : active       ? t.accent
                                          : mix(t.text_dim, t.text, state.hover);
    canvas.text(icon_font, color, rect, icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (state.focused) {
        draw_focus_ring(canvas, rect, radius);
    }
}

void draw_chip(Canvas& canvas, const RECT& rect, const std::wstring& label, bool selected,
               const WidgetState& state, HFONT font, COLORREF dot) {
    const Theme& t = canvas.theme();
    const int radius = height_of(rect) / 2;
    const COLORREF background =
        selected ? t.accent_soft : mix(t.surface_alt, t.surface_hover, state.hover);
    canvas.fill_round(rect, radius, background);
    canvas.outline_round(rect, radius, 1.0f, selected ? t.accent : t.border);
    RECT text_rect = rect;
    if (dot != CLR_INVALID) {
        const int dot_x = rect.left + canvas.px(14);
        canvas.fill_circle(dot_x, (rect.top + rect.bottom) / 2, static_cast<float>(canvas.px(4)), dot);
        text_rect.left = dot_x + canvas.px(10);
        text_rect.right -= canvas.px(10);
        canvas.text(font, selected ? t.text : mix(t.text_dim, t.text, state.hover), text_rect, label,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        canvas.text(font, selected ? t.text : mix(t.text_dim, t.text, state.hover), text_rect, label,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (state.focused) {
        draw_focus_ring(canvas, rect, radius);
    }
}

void draw_help_mark(Canvas& canvas, const RECT& rect, const WidgetState& state, HFONT font) {
    const Theme& t = canvas.theme();
    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2;
    const float r = static_cast<float>(std::min(width_of(rect), canvas.px(size::help))) / 2.0f;
    if (state.hover > 0.0f) {
        canvas.fill_circle(cx, cy, r, t.accent_soft, static_cast<int>(state.hover * 255.0f));
    }
    const COLORREF color = mix(t.text_faint, t.accent, state.hover);
    RECT glyph_rect{cx - static_cast<int>(r), cy - static_cast<int>(r), cx + static_cast<int>(r) + 1,
                    cy + static_cast<int>(r) + 1};
    canvas.text(font, color, glyph_rect, L"?", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (state.focused) {
        canvas.ring(cx, cy, r + static_cast<float>(canvas.px(2)), static_cast<float>(canvas.px(2)),
                    t.focus);
    }
}

void draw_slider(Canvas& canvas, const RECT& rect, const std::wstring& label,
                 const std::wstring& value_text, float fraction, const WidgetState& state,
                 HFONT label_font, HFONT value_font) {
    const Theme& t = canvas.theme();
    const int label_h = canvas.px(24);
    RECT label_rect{rect.left, rect.top, rect.left + width_of(rect) * 64 / 100, rect.top + label_h};
    RECT value_rect{label_rect.right, rect.top, rect.right, rect.top + label_h};
    canvas.text(label_font, state.enabled ? t.text_dim : t.disabled_text, label_rect, label,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    canvas.text(value_font, state.enabled ? t.text : t.disabled_text, value_rect, value_text,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    const int knob_r = canvas.px(8);
    const int track_h = canvas.px(4);
    const int cy = rect.top + label_h + (height_of(rect) - label_h) / 2;
    RECT track{rect.left + knob_r, cy - track_h / 2, rect.right - knob_r, cy - track_h / 2 + track_h};
    canvas.fill_round(track, track_h / 2, state.enabled ? t.surface_hover : t.surface_alt);
    canvas.outline_round(track, track_h / 2, 1.0f, t.border, 120);
    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    const int knob_x = track.left + static_cast<int>(std::lround(static_cast<float>(width_of(track)) * clamped));
    RECT filled = track;
    filled.right = knob_x;
    if (filled.right > filled.left) {
        canvas.fill_round(filled, track_h / 2, state.enabled ? t.accent : t.border_strong);
    }
    if (state.enabled && (state.hover > 0.0f || state.press > 0.0f)) {
        const float halo = static_cast<float>(knob_r) * (1.6f + 0.25f * state.press);
        canvas.fill_circle(knob_x, cy, halo, t.accent, static_cast<int>(48.0f * std::max(state.hover, state.press)));
    }
    canvas.fill_circle(knob_x, cy, static_cast<float>(knob_r), state.enabled ? t.accent : t.border_strong);
    canvas.fill_circle(knob_x, cy, static_cast<float>(knob_r) * 0.42f,
                       state.enabled ? t.accent_text : t.surface_alt);
    if (state.focused) {
        canvas.ring(knob_x, cy, static_cast<float>(knob_r + canvas.px(3)),
                    static_cast<float>(canvas.px(2)), t.focus);
    }
}

void draw_switch(Canvas& canvas, const RECT& track, float on, const WidgetState& state) {
    const Theme& t = canvas.theme();
    const float k = std::clamp(on, 0.0f, 1.0f);
    const int h = height_of(track);
    COLORREF fill = mix(t.surface_hover, t.accent, k);
    if (!state.enabled) {
        fill = mix(fill, t.surface, 0.55f);
    }
    canvas.fill_round(track, h / 2, fill);
    if (k < 1.0f) {
        canvas.outline_round(track, h / 2, 1.0f, t.border_strong, static_cast<int>((1.0f - k) * 255.0f));
    }
    const float r = static_cast<float>(h) / 2.0f - static_cast<float>(canvas.px(3));
    const float left = static_cast<float>(track.left) + static_cast<float>(h) / 2.0f;
    const float right = static_cast<float>(track.right) - static_cast<float>(h) / 2.0f;
    const int cx = static_cast<int>(std::lround(left + (right - left) * k));
    const int cy = (track.top + track.bottom) / 2;
    if (state.enabled && state.hover > 0.0f) {
        canvas.fill_circle(cx, cy, r + static_cast<float>(canvas.px(5)), t.accent,
                           static_cast<int>(36.0f * state.hover));
    }
    const COLORREF thumb = state.enabled ? (k > 0.5f ? t.accent_text : (t.dark ? t.text_dim : RGB(255, 255, 255)))
                                         : t.disabled_text;
    canvas.fill_circle(cx, cy, r, thumb);
    if (!t.dark && k <= 0.5f) {
        canvas.ring(cx, cy, r, 1.0f, t.border_strong);
    }
    if (state.focused) {
        draw_focus_ring(canvas, track, h / 2);
    }
}

RECT segment_rect(const RECT& rect, int count, int index) {
    const int n = std::max(1, count);
    const int inset = 3;
    const int inner_w = width_of(rect) - inset * 2;
    const int left = rect.left + inset + inner_w * index / n;
    const int right = rect.left + inset + inner_w * (index + 1) / n;
    return RECT{left, rect.top + inset, right, rect.bottom - inset};
}

void draw_segmented(Canvas& canvas, const RECT& rect, const wchar_t* const* labels, int count,
                    float selected_pos, int hover_index, int focus_index, bool enabled,
                    HFONT font) {
    const Theme& t = canvas.theme();
    const int radius = canvas.px(radius::md);
    canvas.fill_round(rect, radius, t.surface_alt);
    canvas.outline_round(rect, radius, 1.0f, t.border);
    if (count <= 0) {
        return;
    }
    if (selected_pos >= 0.0f) {
        const float pos = std::clamp(selected_pos, 0.0f, static_cast<float>(count - 1));
        const int lower = static_cast<int>(std::floor(pos));
        const int upper = std::min(count - 1, lower + 1);
        const float frac = pos - static_cast<float>(lower);
        const RECT a = segment_rect(rect, count, lower);
        const RECT b = segment_rect(rect, count, upper);
        RECT pill{static_cast<LONG>(std::lround(a.left + (b.left - a.left) * frac)), a.top,
                  static_cast<LONG>(std::lround(a.right + (b.right - a.right) * frac)), a.bottom};
        const COLORREF pill_color = t.dark ? mix(t.surface_hover, t.text, 0.08f) : t.surface;
        if (!t.dark) {
            canvas.shadow(pill, radius - canvas.px(2), 2);
        }
        canvas.fill_round(pill, radius - canvas.px(2), enabled ? pill_color : t.surface_hover);
        if (!t.dark) {
            canvas.outline_round(pill, radius - canvas.px(2), 1.0f, t.border);
        }
    }
    const int selected = selected_pos < 0.0f ? -1 : static_cast<int>(std::lround(selected_pos));
    for (int i = 0; i < count; ++i) {
        const RECT seg = segment_rect(rect, count, i);
        COLORREF color = t.text_dim;
        if (!enabled) {
            color = t.disabled_text;
        } else if (i == selected) {
            color = t.text;
        } else if (i == hover_index) {
            color = mix(t.text_dim, t.text, 0.7f);
        }
        canvas.text(font, color, seg, labels[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (i == focus_index) {
            RECT ring = seg;
            InflateRect(&ring, -canvas.px(1), -canvas.px(1));
            canvas.outline_round(ring, radius - canvas.px(2), static_cast<float>(canvas.px(2)), t.focus);
        }
    }
}

void draw_card(Canvas& canvas, const RECT& rect) {
    const Theme& t = canvas.theme();
    const int radius = canvas.px(radius::lg);
    canvas.shadow(rect, radius, canvas.px(3));
    canvas.fill_round(rect, radius, t.surface);
    canvas.outline_round(rect, radius, 1.0f, t.border);
}

void draw_dot(Canvas& canvas, int center_x, int center_y, int radius, COLORREF color) {
    canvas.fill_circle(center_x, center_y, static_cast<float>(radius), color);
}

void draw_status_dot(Canvas& canvas, int center_x, int center_y, int radius, COLORREF color,
                     float pulse_phase) {
    if (pulse_phase >= 0.0f) {
        const float p = std::clamp(pulse_phase, 0.0f, 1.0f);
        const float r = static_cast<float>(radius) * (1.0f + 1.6f * p);
        canvas.fill_circle(center_x, center_y, r, color, static_cast<int>((1.0f - p) * 110.0f));
    }
    canvas.fill_circle(center_x, center_y, static_cast<float>(radius), color);
}

void draw_badge(Canvas& canvas, const RECT& rect, const std::wstring& label, COLORREF color,
                HFONT font) {
    const Theme& t = canvas.theme();
    canvas.fill_round(rect, height_of(rect) / 2, mix(t.surface, color, t.dark ? 0.18f : 0.12f));
    canvas.text(font, color, rect, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_progress_bar(Canvas& canvas, const RECT& rect, float fraction, COLORREF color) {
    const Theme& t = canvas.theme();
    const int r = height_of(rect) / 2;
    canvas.fill_round(rect, r, t.surface_hover);
    const float k = std::clamp(fraction, 0.0f, 1.0f);
    if (k > 0.0f) {
        RECT filled = rect;
        filled.right = rect.left + std::max(height_of(rect),
                                            static_cast<int>(std::lround(width_of(rect) * k)));
        canvas.fill_round(filled, r, color);
    }
}

void draw_icon(Canvas& canvas, const RECT& rect, const wchar_t* glyph, HFONT font, COLORREF color) {
    canvas.text(font, color, rect, glyph, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_scrollbar(Canvas& canvas, const RECT& track_rect, const RECT& thumb_rect, float hover) {
    const Theme& t = canvas.theme();
    if (hover > 0.0f) {
        canvas.fill_round(track_rect, width_of(track_rect) / 2, t.surface_alt,
                          static_cast<int>(hover * 255.0f));
    }
    RECT thumb = thumb_rect;
    const int shrink = static_cast<int>(std::lround((1.0f - hover) * canvas.px(2)));
    thumb.left += shrink;
    thumb.right -= shrink;
    if (thumb.bottom - thumb.top < canvas.px(16)) {
        thumb.bottom = thumb.top + canvas.px(16);
    }
    canvas.fill_round(thumb, width_of(thumb) / 2, mix(t.border_strong, t.text_faint, hover));
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

int text_width(HDC dc, HFONT font, const std::wstring& text) {
    if (text.empty()) {
        return 0;
    }
    const HGDIOBJ previous = SelectObject(dc, font);
    SIZE extent{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
    SelectObject(dc, previous);
    return extent.cx;
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
