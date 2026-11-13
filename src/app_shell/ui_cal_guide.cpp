#include "app_shell/ui_cal_guide.h"

#include "app_shell/paths.h"
#include "app_shell/ui_paint_internal.h"

#include <algorithm>
#include <cmath>

namespace gt::ui {
namespace {

using detail::rect_of;
using detail::rh;
using detail::rw;

constexpr float kPi = 3.14159265358979323846f;

int band_height(const RECT& area, const CalGuideStyle& style) {
    if (!style.show_text) {
        return 0;
    }
    return static_cast<int>(static_cast<float>(rh(area)) * 0.30f);
}

int bottom_height(const RECT& area) {
    return static_cast<int>(static_cast<float>(rh(area)) * 0.11f);
}

COLORREF tone_colour(const CalGuideStyle& style, CalGuideTone tone) {
    switch (tone) {
        case CalGuideTone::Good:
            return style.good;
        case CalGuideTone::Warn:
            return style.warn;
        case CalGuideTone::Neutral:
            break;
    }
    return style.neutral;
}

CalGuidePoint region_centre(const RECT& region, CalGuideKind kind) {
    const float cx = 0.5f * static_cast<float>(region.left + region.right);
    float cy = 0.5f * static_cast<float>(region.top + region.bottom);
    if (kind == CalGuideKind::Tilt) {
        cy = static_cast<float>(region.top) + 0.95f * static_cast<float>(rh(region));
    } else if (kind == CalGuideKind::Nod) {
        // Nodding goes down: start the line higher so the target fits.
        cy = static_cast<float>(region.top) + 0.18f * static_cast<float>(rh(region));
    }
    return {cx, cy};
}

CalGuideScale region_scale(const RECT& region, const CalGuideView& view, float fov_h_deg) {
    const float w = static_cast<float>(rw(region));
    float h = static_cast<float>(rh(region));
    if (view.live.kind == CalGuideKind::Nod) {
        h *= 2.0f * 0.82f;  // the line starts near the top: 82% of the height lies below it
    }
    CalGuideScale scale = cal_guide_scale(view.live.kind, w, h, fov_h_deg, view.live.target_deg);
    if (view.live.kind == CalGuideKind::Tilt) {
        scale.arc_radius = std::min(0.8f * static_cast<float>(rh(region)), 0.45f * w);
    }
    return scale;
}

void dots(Canvas& c, int cx, int cy, int count, int done, float r, const CalGuideStyle& style) {
    if (count <= 0) {
        return;
    }
    const float gap = r * 3.2f;
    const float x0 = static_cast<float>(cx) - gap * static_cast<float>(count - 1) * 0.5f;
    for (int i = 0; i < count; ++i) {
        const int x = static_cast<int>(std::lround(x0 + gap * static_cast<float>(i)));
        if (i < done) {
            c.fill_circle(x, cy, r, style.good);
        } else {
            c.ring(x, cy, r - 1.0f, std::max(1.5f, r * 0.25f), style.tick);
        }
    }
}

}  // namespace

CalGuideStyle panel_guide_style(const Theme& t, const FontSet& f) {
    CalGuideStyle style;
    style.glasses = false;
    style.background = t.canvas;
    style.line = t.error;
    style.track = t.grid;
    style.good = t.ok;
    style.warn = t.warning;
    style.neutral = t.text_faint;
    style.text = t.text;
    style.text_dim = t.text_dim;
    style.tick = t.border_strong;
    style.target = t.ok;
    style.headline = f.title;
    style.sub = f.small;
    style.small = f.caption;
    style.countdown = f.metric;
    return style;
}

void GlassesGuideFonts::create(int area_height) {
    destroy();
    height = area_height;
    const auto font = [](int px, int weight) {
        return CreateFontW(-std::max(10, px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    headline = font(area_height * 62 / 1000, FW_SEMIBOLD);
    sub = font(area_height * 32 / 1000, FW_NORMAL);
    small = font(area_height * 25 / 1000, FW_NORMAL);
    countdown = font(area_height * 100 / 1000, FW_SEMIBOLD);
}

void GlassesGuideFonts::destroy() {
    for (HFONT* f : {&headline, &sub, &small, &countdown}) {
        if (*f != nullptr) {
            DeleteObject(*f);
            *f = nullptr;
        }
    }
    height = 0;
}

CalGuideStyle glasses_guide_style(const GlassesGuideFonts& fonts) {
    CalGuideStyle style;
    style.headline = fonts.headline;
    style.sub = fonts.sub;
    style.small = fonts.small;
    style.countdown = fonts.countdown;
    style.glasses = true;
    // Black is see-through on the birdbath optics; everything else is bright.
    style.background = rgb(0, 0, 0);
    style.line = rgb(255, 48, 48);
    style.track = rgb(70, 70, 70);
    style.good = rgb(60, 220, 120);
    style.warn = rgb(255, 190, 40);
    style.neutral = rgb(210, 210, 210);
    style.text = rgb(255, 255, 255);
    style.text_dim = rgb(200, 200, 200);
    style.tick = rgb(170, 170, 170);
    style.target = rgb(60, 220, 120);
    return style;
}

RECT cal_guide_region(const RECT& area, const CalGuideStyle& style) {
    RECT region = area;
    region.top += band_height(area, style);
    region.bottom -= bottom_height(area);
    const int inset = std::max(4, rw(area) / 40);
    region.left += inset;
    region.right -= inset;
    if (region.bottom <= region.top) {
        region.bottom = region.top + 1;
    }
    return region;
}

CalGuidePoint cal_guide_target_ball(const RECT& area, const CalGuideView& view, const CalGuideStyle& style,
                                    float fov_h_deg) {
    const RECT region = cal_guide_region(area, style);
    const CalGuidePoint centre = region_centre(region, view.live.kind);
    const CalGuideScale scale = region_scale(region, view, fov_h_deg);
    // The ball is clamped to the region below (its centre is not the region's).
    CalGuidePoint ball = cal_guide_ball(view.live, scale, centre.x, centre.y, static_cast<float>(rw(region)) * 2.0f,
                                        static_cast<float>(rh(region)) * 2.0f);
    const float margin = 0.5f * static_cast<float>(std::min(rw(region), rh(region))) * 0.06f;
    ball.x = std::clamp(ball.x, static_cast<float>(region.left) + margin, static_cast<float>(region.right) - margin);
    ball.y = std::clamp(ball.y, static_cast<float>(region.top) + margin, static_cast<float>(region.bottom) - margin);
    return ball;
}

void paint_cal_guide(Canvas& c, const RECT& area, const CalGuideView& view, const CalGuideStyle& style,
                     float fov_h_deg, const CalGuidePoint* ball_override) {
    c.fill(area, style.background);
    const int W = rw(area);
    const int H = rh(area);
    if (W < 40 || H < 40) {
        return;
    }
    const float unit = static_cast<float>(std::min(W, H * 2));
    const float lw = std::max(2.0f, static_cast<float>(H) * (style.glasses ? 0.007f : 0.012f));
    const float ball_r = std::max(5.0f, static_cast<float>(H) * (style.glasses ? 0.03f : 0.04f));
    const COLORREF tone = tone_colour(style, view.tone);
    const CalGuideKind kind = view.live.kind;

    // ---- Text band ------------------------------------------------------
    const int band = band_height(area, style);
    if (style.show_text && band > 0) {
        const int pad = std::max(4, H / 60);
        RECT step_rect{area.left + pad * 2, area.top + pad, area.left + W / 3, area.top + pad + H / 18};
        if (view.steps > 0 && style.glasses) {  // the PC panel already shows the step
            c.text(style.small, style.text_dim, step_rect,
                   format_wide(L"Step %d of %d", std::max(1, view.step), view.steps), DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
        const int text_left = area.left + W / 8;
        const int text_w = W - W / 4;
        const COLORREF headline_colour = view.tone == CalGuideTone::Warn ? style.warn : style.text;
        const std::wstring headline = wide_from_utf8(view.headline);
        const int hh = wrapped_text_height(c.dc(), style.headline, text_w, headline);
        int y = area.top + pad;
        c.text(style.headline, headline_colour, rect_of(text_left, y, text_w, hh), headline,
               DT_CENTER | DT_TOP | DT_WORDBREAK);
        y += hh + pad / 2;
        if (!view.subline.empty()) {
            const std::wstring sub = wide_from_utf8(view.subline);
            const int sh = std::min(wrapped_text_height(c.dc(), style.sub, text_w, sub), static_cast<int>(area.top) + band - y);
            if (sh > 0) {
                c.text(style.sub, style.text_dim, rect_of(text_left, y, text_w, sh), sub,
                       DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
                y += sh + pad / 2;
            }
        }
        if (!view.tip.empty() && y < area.top + band) {
            const std::wstring tip = wide_from_utf8(view.tip);
            const int th = std::min(wrapped_text_height(c.dc(), style.sub, text_w, tip), static_cast<int>(area.top) + band - y);
            if (th > 0) {
                c.text(style.sub, style.warn, rect_of(text_left, y, text_w, th), tip,
                       DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
            }
        }
        if (view.countdown >= 0) {
            const RECT count_rect{area.right - W / 8 - pad, area.top + pad, area.right - pad, area.top + band - pad};
            c.text(style.countdown, style.text, count_rect, format_wide(L"%d", view.countdown),
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            const RECT label{count_rect.left, area.top + pad, count_rect.right, area.top + pad + H / 18};
            c.text(style.small, style.text_dim, label, L"starts in", DT_CENTER | DT_TOP | DT_SINGLELINE);
        }
    }

    // ---- Guide ----------------------------------------------------------
    const RECT region = cal_guide_region(area, style);
    const CalGuidePoint centre = region_centre(region, kind);
    const CalGuideScale scale = region_scale(region, view, fov_h_deg);
    const float cx = centre.x;
    const float cy = centre.y;
    const float margin = static_cast<float>(std::max(6, W / 50));
    const float gate = view.live.gate_deg;
    const float target = view.live.target_deg;
    const bool moving_step = kind != CalGuideKind::Still;

    const auto tick = [&](float deg, COLORREF colour, float length, float thickness) {
        const CalGuidePoint p = cal_guide_axis_point(kind, deg, scale, cx, cy);
        if (kind == CalGuideKind::Turn) {
            c.line(p.x, p.y - length, p.x, p.y + length, thickness, colour);
        } else if (kind == CalGuideKind::Nod) {
            c.line(p.x - length, p.y, p.x + length, p.y, thickness, colour);
        } else if (kind == CalGuideKind::Tilt) {
            const float dx = (p.x - cx) / scale.arc_radius;
            const float dy = (p.y - cy) / scale.arc_radius;
            c.line(p.x - dx * length, p.y - dy * length, p.x + dx * length, p.y + dy * length, thickness, colour);
        }
    };
    const auto label_at = [&](float deg, COLORREF colour, float offset) {
        const CalGuidePoint p = cal_guide_axis_point(kind, deg, scale, cx, cy);
        RECT r{};
        const int bw = static_cast<int>(unit * 0.08f);
        const int bh = std::max(12, H / 22);
        if (kind == CalGuideKind::Turn) {
            r = rect_of(static_cast<int>(p.x) - bw / 2, static_cast<int>(p.y + offset), bw, bh);
        } else if (kind == CalGuideKind::Nod) {
            r = rect_of(static_cast<int>(p.x + offset), static_cast<int>(p.y) - bh / 2, bw, bh);
        } else {
            const float dx = (p.x - cx) / scale.arc_radius;
            const float dy = (p.y - cy) / scale.arc_radius;
            r = rect_of(static_cast<int>(p.x + dx * offset) - bw / 2, static_cast<int>(p.y + dy * offset) - bh / 2, bw,
                        bh);
        }
        const UINT align = kind == CalGuideKind::Nod ? (DT_LEFT | DT_VCENTER | DT_SINGLELINE)
                                                     : (DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        c.text(style.small, colour, r, format_wide(L"%d°", static_cast<int>(std::lround(deg))), align);
    };

    const float tick_len = static_cast<float>(rh(region)) * 0.05f;
    switch (kind) {
        case CalGuideKind::Turn:
            c.line(static_cast<float>(region.left) + margin, cy, static_cast<float>(region.right) - margin, cy, lw,
                   style.line);
            c.line(cx, cy - tick_len * 1.6f, cx, cy + tick_len * 1.6f, lw * 0.6f, style.text_dim);
            break;
        case CalGuideKind::Nod:
            c.line(cx, static_cast<float>(region.top) + margin * 0.5f, cx, static_cast<float>(region.bottom) - margin * 0.5f,
                   lw, style.line);
            c.line(cx - tick_len * 1.6f, cy, cx + tick_len * 1.6f, cy, lw * 0.6f, style.text_dim);
            break;
        case CalGuideKind::Tilt: {
            const int r = static_cast<int>(std::lround(scale.arc_radius));
            c.line(cx, cy, cx, cy - scale.arc_radius, std::max(1.0f, lw * 0.4f), style.track);
            // Arc spans +-30 deg of roll (+-60 deg drawn); GDI+ angles start at +x, clockwise.
            c.arc(static_cast<int>(cx), static_cast<int>(cy), static_cast<float>(r), -90.0f - 30.0f * scale.arc_gain,
                  60.0f * scale.arc_gain, lw, style.line);
            const CalGuidePoint top = cal_guide_axis_point(kind, 0.0f, scale, cx, cy);
            c.line(top.x, top.y - tick_len * 1.6f, top.x, top.y + tick_len * 1.6f, lw * 0.6f, style.text_dim);
            break;
        }
        case CalGuideKind::Still: {
            const float r_min = static_cast<float>(rh(region)) * 0.07f;
            const float r_max = static_cast<float>(rh(region)) * 0.42f;
            c.line(cx - r_max, cy, cx + r_max, cy, 1.0f, style.track);
            c.line(cx, cy - r_max, cx, cy + r_max, 1.0f, style.track);
            c.ring(static_cast<int>(cx), static_cast<int>(cy), r_min, std::max(1.0f, lw * 0.5f), style.target);
            const float rms = view.has_live && !view.live.waiting ? view.live.still_rms_degs : view.live.still_gate_degs;
            const float ring_r = cal_guide_ring_radius(rms, view.live.still_gate_degs, r_min, r_max);
            c.ring(static_cast<int>(cx), static_cast<int>(cy), ring_r, lw, view.has_live ? tone : style.neutral);
            break;
        }
    }
    if (moving_step) {
        tick(gate, style.tick, tick_len, lw * 0.6f);
        tick(target, style.target, tick_len * 1.7f, lw);
        label_at(target, style.target,
                 kind == CalGuideKind::Turn ? tick_len * 1.9f + 2.0f
                                            : tick_len * 1.7f + static_cast<float>(std::max(12, H / 22)));
        if (view.has_live && !view.live.waiting && view.live.peak_deg > 1.0f) {
            const CalGuidePoint peak = cal_guide_axis_point(kind, view.live.peak_deg, scale, cx, cy);
            c.ring(static_cast<int>(peak.x), static_cast<int>(peak.y), ball_r * 0.55f, std::max(1.0f, lw * 0.4f),
                   style.text_dim);
        }
    }

    if (view.has_live) {
        CalGuidePoint ball = ball_override != nullptr ? *ball_override
                                                      : cal_guide_target_ball(area, view, style, fov_h_deg);
        if (kind == CalGuideKind::Tilt) {
            c.line(cx, cy, ball.x, ball.y, std::max(1.5f, lw * 0.6f), tone);
        }
        const float r = kind == CalGuideKind::Still ? ball_r * 0.6f : ball_r;
        c.fill_circle(static_cast<int>(std::lround(ball.x)), static_cast<int>(std::lround(ball.y)), r, tone);
    }

    // ---- Repeat dots ----------------------------------------------------
    if (moving_step && view.live.repeats_required > 0) {
        const int dot_y = area.bottom - bottom_height(area) / 2;
        dots(c, (area.left + area.right) / 2, dot_y, view.live.repeats_required,
             std::min(view.live.repeats, view.live.repeats_required), std::max(4.0f, static_cast<float>(H) * 0.016f),
             style);
    }
}

}  // namespace gt::ui
