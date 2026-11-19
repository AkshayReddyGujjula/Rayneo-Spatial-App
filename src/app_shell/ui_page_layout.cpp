// The Layout page: presets, the arrangement editor (2D map and orbitable 3D
// view), the screen list and the selected-screen inspector. Also hosts the
// editor geometry shared with the input handling, and the small arc preview
// reused by Home and the setup guide.

#include "app_shell/ui_orbit.h"
#include "app_shell/ui_paint_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace gt::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;

COLORREF screen_tint(const ScreenLayout& screen) {
    return RGB(static_cast<int>(std::clamp(screen.color[0], 0.0f, 1.0f) * 255.0f),
               static_cast<int>(std::clamp(screen.color[1], 0.0f, 1.0f) * 255.0f),
               static_cast<int>(std::clamp(screen.color[2], 0.0f, 1.0f) * 255.0f));
}

}  // namespace

EditorGeometry editor_geometry(const Layout& layout, const RECT& rect, UINT dpi) {
    EditorGeometry geometry;
    geometry.head.x = (rect.left + rect.right) / 2;
    geometry.head.y = rect.bottom - scale_px(30, dpi);
    float max_depth = 0.5f;
    float max_lateral = 0.5f;
    for (const ScreenLayout& screen : layout.screens) {
        const float yaw = screen.yaw_deg * kPi / 180.0f;
        const float pitch = screen.pitch_deg * kPi / 180.0f;
        const float x = screen.distance_m * std::sin(yaw) * std::cos(pitch);
        const float z = screen.distance_m * std::cos(yaw) * std::cos(pitch);
        max_depth = std::max(max_depth, z + screen.height_m * 0.5f);
        max_lateral = std::max(max_lateral, std::fabs(x) + screen.width_m * 0.5f);
    }
    const float usable_width =
        static_cast<float>(rect.right - rect.left) * 0.5f - static_cast<float>(scale_px(14, dpi));
    const float usable_height =
        static_cast<float>(geometry.head.y - rect.top) - static_cast<float>(scale_px(8, dpi));
    const float scale_x = usable_width / max_lateral;
    const float scale_y = usable_height / max_depth;
    geometry.scale = std::max(6.0f, std::min(scale_x, scale_y));
    return geometry;
}

POINT editor_screen_center(const ScreenLayout& screen, const EditorGeometry& geometry) {
    const float yaw = screen.yaw_deg * kPi / 180.0f;
    const float pitch = screen.pitch_deg * kPi / 180.0f;
    const float x = screen.distance_m * std::sin(yaw) * std::cos(pitch);
    const float z = screen.distance_m * std::cos(yaw) * std::cos(pitch);
    POINT point{};
    point.x = geometry.head.x + static_cast<int>(x * geometry.scale);
    point.y = geometry.head.y - static_cast<int>(z * geometry.scale);
    return point;
}

int editor_screen_hit_test(const Layout& layout, const EditorGeometry& geometry, POINT point,
                           int threshold_px) {
    int best = -1;
    double best_distance = static_cast<double>(threshold_px) * threshold_px;
    for (size_t index = 0; index < layout.screens.size(); ++index) {
        const ScreenLayout& screen = layout.screens[index];
        const POINT center = editor_screen_center(screen, geometry);
        const float yaw = screen.yaw_deg * kPi / 180.0f;
        const float half_width =
            std::max(static_cast<float>(threshold_px), screen.width_m * 0.5f * geometry.scale);
        const double dx = std::cos(yaw) * half_width;
        const double dy = std::sin(yaw) * half_width;
        const double px = static_cast<double>(point.x - center.x);
        const double py = static_cast<double>(point.y - center.y);
        const double length_squared = dx * dx + dy * dy;
        double t = length_squared > 0.0 ? (px * dx + py * dy) / length_squared : 0.0;
        t = std::clamp(t, -1.0, 1.0);
        const double cx = px - dx * t;
        const double cy = py - dy * t;
        const double distance = cx * cx + cy * cy;
        if (distance <= best_distance) {
            best_distance = distance;
            best = static_cast<int>(index);
        }
    }
    return best;
}

float editor_yaw_from_point(const EditorGeometry& geometry, POINT point) {
    const double dx = static_cast<double>(point.x - geometry.head.x);
    const double dz = static_cast<double>(geometry.head.y - point.y);
    if (dx == 0.0 && dz == 0.0) {
        return 0.0f;
    }
    const double yaw = std::atan2(dx, dz) * 180.0 / static_cast<double>(kPi);
    return static_cast<float>(yaw);
}

namespace detail {

std::wstring editor_screen_label(const ScreenLayout& screen) {
    // Ids may be up to 64 characters: shorten the id, never the angle (a
    // fixed 64-wide swprintf buffer used to drop the angle, and the label
    // overran the canvas).
    return ellipsize(wide_from_utf8(screen.id), 24) +
           format_wide(L"  %.0f°", static_cast<double>(screen.yaw_deg));
}

// Geometry of the 2D editor's fixed captions (the -90/0/90 yaw ticks, then
// "you"), which screen labels must not cover.
std::vector<RECT> editor_2d_captions(const EditorGeometry& geometry, UINT dpi) {
    std::vector<RECT> captions;
    for (const int angle_deg : {-90, 0, 90}) {
        const float angle = static_cast<float>(angle_deg) * kPi / 180.0f;
        const int label_radius = scale_px(52, dpi);
        captions.push_back(
            rect_of(geometry.head.x + static_cast<int>(std::sin(angle) * label_radius) - scale_px(20, dpi),
                    geometry.head.y - static_cast<int>(std::cos(angle) * label_radius) - scale_px(18, dpi),
                    scale_px(40, dpi), scale_px(16, dpi)));
    }
    captions.push_back(rect_of(geometry.head.x - scale_px(40, dpi), geometry.head.y + scale_px(8, dpi),
                               scale_px(80, dpi), scale_px(16, dpi)));
    return captions;
}

std::vector<EditorLabel> editor_2d_labels(HDC dc, const FontSet& fonts, const Layout& layout,
                                          size_t selected_screen, const RECT& canvas_rect, UINT dpi) {
    const EditorGeometry geometry = editor_geometry(layout, canvas_rect, dpi);
    std::vector<RECT> blockers = editor_2d_captions(geometry, dpi);
    std::vector<EditorLabel> placed;
    // The selected screen's label is placed first, so it gets the best spot
    // and the others avoid it.
    std::vector<size_t> order;
    if (selected_screen < layout.screens.size()) {
        order.push_back(selected_screen);
    }
    for (size_t index = 0; index < layout.screens.size(); ++index) {
        if (index != selected_screen) {
            order.push_back(index);
        }
    }
    const auto inside_canvas = [&](const RECT& r) {
        return r.left >= canvas_rect.left && r.right <= canvas_rect.right && r.top >= canvas_rect.top &&
               r.bottom <= canvas_rect.bottom;
    };
    const auto clamp_into_canvas = [&](RECT r) {
        OffsetRect(&r, std::max(0L, canvas_rect.left - r.left), std::max(0L, canvas_rect.top - r.top));
        OffsetRect(&r, std::min(0L, canvas_rect.right - r.right), std::min(0L, canvas_rect.bottom - r.bottom));
        return r;
    };
    const auto collides = [&](const RECT& r) {
        for (const RECT& other : blockers) {
            RECT intersection{};
            RECT inflated = r;
            InflateRect(&inflated, scale_px(2, dpi), scale_px(1, dpi));
            if (IntersectRect(&intersection, &inflated, &other) != 0) {
                return true;
            }
        }
        return false;
    };
    for (const size_t index : order) {
        const ScreenLayout& screen = layout.screens[index];
        const bool selected = index == selected_screen;
        const float yaw = screen.yaw_deg * kPi / 180.0f;
        const POINT center = editor_screen_center(screen, geometry);
        const float thickness = static_cast<float>(selected ? scale_px(7, dpi) : scale_px(5, dpi));
        HFONT label_font = selected ? fonts.small_strong : fonts.small;
        const int label_h = scale_px(18, dpi);
        const float nx = std::sin(yaw);
        const float ny = -std::cos(yaw);
        // The label sits beside the screen along its outward normal, far
        // enough that the text box clears the (possibly angled) line and the
        // selection halo. Preference: outward, inward (towards the wearer),
        // then outward or inward pushed back inside the canvas; the first
        // one inside the canvas that collides with no other label wins.
        // Returns whether the spot found for this text is clear.
        const auto place = [&](const std::wstring& text, RECT& label_rect) {
            const int label_w = text_width(dc, label_font, text) + scale_px(8, dpi);
            const float clearance = std::fabs(nx) * static_cast<float>(label_w) * 0.5f +
                                    std::fabs(ny) * static_cast<float>(label_h) * 0.5f +
                                    (thickness + (selected ? static_cast<float>(scale_px(12, dpi)) : 0.0f)) * 0.5f +
                                    static_cast<float>(scale_px(6, dpi));
            const auto label_at = [&](float direction) {
                const int lx = center.x + static_cast<int>(std::lround(nx * clearance * direction));
                const int ly = center.y + static_cast<int>(std::lround(ny * clearance * direction));
                return rect_of(lx - label_w / 2, ly - label_h / 2, label_w, label_h);
            };
            const RECT candidates[] = {label_at(1.0f), label_at(-1.0f), clamp_into_canvas(label_at(1.0f)),
                                       clamp_into_canvas(label_at(-1.0f))};
            label_rect = candidates[2];
            for (const RECT& candidate : candidates) {
                if (inside_canvas(candidate) && !collides(candidate)) {
                    label_rect = candidate;
                    return true;
                }
            }
            // Crowded layouts: step the label down until it is clear, but
            // never out of the canvas (a clipped label is worse than a close one).
            const int step = scale_px(17, dpi);
            for (int attempt = 0; attempt < 8; ++attempt) {
                RECT next = label_rect;
                OffsetRect(&next, 0, step * (attempt + 1));
                if (!inside_canvas(next)) {
                    break;
                }
                if (!collides(next)) {
                    label_rect = next;
                    return true;
                }
            }
            return false;
        };
        // A crowded canvas (narrow window) first drops the angle, then an
        // unselected screen's label (its chip in the inspector still names
        // it). The selected screen's label always shows.
        EditorLabel label;
        label.index = index;
        label.selected = selected;
        label.text = editor_screen_label(screen);
        bool clear = place(label.text, label.rect);
        if (!clear) {
            const std::wstring short_text = ellipsize(wide_from_utf8(screen.id), 24);
            RECT short_rect{};
            if (place(short_text, short_rect)) {
                label.text = short_text;
                label.rect = short_rect;
                clear = true;
            }
        }
        if (!clear && !selected) {
            continue;
        }
        blockers.push_back(label.rect);
        placed.push_back(label);
    }
    return placed;
}

namespace {

void paint_editor_2d(Ui& ui, const RECT& canvas_rect) {
    const AppState& s = ui.s;
    const Theme& t = ui.t;
    const EditorGeometry geometry = editor_geometry(s.layout, canvas_rect, ui.dpi);
    ui.c.push_clip(canvas_rect);
    // Distance rings every metre in front of the head.
    for (int metres = 1; metres <= 4; ++metres) {
        ui.c.arc(geometry.head.x, geometry.head.y, static_cast<float>(metres) * geometry.scale, 180.0f, 180.0f,
                 1.0f, t.grid);
    }
    for (int angle_deg = -90; angle_deg <= 90; angle_deg += 15) {
        const float angle = static_cast<float>(angle_deg) * kPi / 180.0f;
        const float inner = static_cast<float>(ui.px(10));
        ui.c.line(static_cast<float>(geometry.head.x), static_cast<float>(geometry.head.y),
                  static_cast<float>(geometry.head.x) + std::sin(angle) * inner,
                  static_cast<float>(geometry.head.y) - std::cos(angle) * inner, 1.0f, t.border_strong);
    }
    const std::vector<RECT> captions = editor_2d_captions(geometry, ui.dpi);
    const wchar_t* const tick_texts[] = {L"-90°", L"0°", L"90°"};
    for (size_t i = 0; i < std::size(tick_texts); ++i) {
        ui.c.text(ui.f.caption, t.text_faint, captions[i], tick_texts[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // The wearer.
    ui.c.fill_circle(geometry.head.x, geometry.head.y, static_cast<float>(ui.px(10)), t.accent, 50);
    ui.c.fill_circle(geometry.head.x, geometry.head.y, static_cast<float>(ui.px(5)), t.accent);
    ui.c.text(ui.f.caption, t.text_faint, captions[3], L"you", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    for (size_t index = 0; index < s.layout.screens.size(); ++index) {
        const ScreenLayout& screen = s.layout.screens[index];
        const float yaw = screen.yaw_deg * kPi / 180.0f;
        const POINT center = editor_screen_center(screen, geometry);
        const float half_width =
            std::max(static_cast<float>(ui.px(9)), screen.width_m * 0.5f * geometry.scale);
        const float dx = std::cos(yaw) * half_width;
        const float dy = std::sin(yaw) * half_width;
        const COLORREF tint = screen_tint(screen);
        const bool selected = index == s.selected_screen;
        const float thickness = static_cast<float>(selected ? ui.px(7) : ui.px(5));
        const float x0 = static_cast<float>(center.x) - dx;
        const float y0 = static_cast<float>(center.y) - dy;
        const float x1 = static_cast<float>(center.x) + dx;
        const float y1 = static_cast<float>(center.y) + dy;
        if (selected) {
            // Selection keeps the screen's own colour (several screens are
            // blue) and adds a halo plus drag handles at both ends.
            ui.c.line(x0, y0, x1, y1, thickness + static_cast<float>(ui.px(12)), t.text, t.dark ? 34 : 26);
        }
        ui.c.line(x0, y0, x1, y1, thickness, tint);
        if (selected) {
            const float r = static_cast<float>(ui.px(6));
            for (const POINT end : {POINT{static_cast<LONG>(x0), static_cast<LONG>(y0)},
                                    POINT{static_cast<LONG>(x1), static_cast<LONG>(y1)}}) {
                ui.c.fill_circle(end.x, end.y, r, t.surface);
                ui.c.ring(end.x, end.y, r, static_cast<float>(ui.px(2)), t.text);
            }
        }
    }
    for (const EditorLabel& label :
         editor_2d_labels(ui.c.dc(), ui.f, s.layout, s.selected_screen, canvas_rect, ui.dpi)) {
        ui.c.text(label.selected ? ui.f.small_strong : ui.f.small, label.selected ? t.text : t.text_dim, label.rect,
                  label.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    ui.c.pop_clip();
}

void paint_editor_3d(Ui& ui, const RECT& canvas_rect) {
    const AppState& s = ui.s;
    const Theme& t = ui.t;
    OrbitView view;
    view.yaw_deg = s.orbit_yaw_deg;
    view.pitch_deg = s.orbit_pitch_deg;
    view.distance_m = s.orbit_distance_m;
    const OrbitCamera camera = orbit_camera(view);
    ui.c.push_clip(canvas_rect);
    for (int step = -4; step <= 6; ++step) {
        const OrbitVec3 xa{static_cast<float>(step), -2.0f, -1.0f};
        const OrbitVec3 xb{static_cast<float>(step), 6.0f, -1.0f};
        const OrbitVec3 ya{-4.0f, static_cast<float>(step) - 1.0f, -1.0f};
        const OrbitVec3 yb{6.0f, static_cast<float>(step) - 1.0f, -1.0f};
        const OrbitPoint pa = orbit_project(view, camera, xa, canvas_rect);
        const OrbitPoint pb = orbit_project(view, camera, xb, canvas_rect);
        const OrbitPoint pc = orbit_project(view, camera, ya, canvas_rect);
        const OrbitPoint pd = orbit_project(view, camera, yb, canvas_rect);
        if (!pa.behind && !pb.behind) {
            ui.c.line(static_cast<float>(pa.pixel.x), static_cast<float>(pa.pixel.y), static_cast<float>(pb.pixel.x),
                      static_cast<float>(pb.pixel.y), 1.0f, t.grid);
        }
        if (!pc.behind && !pd.behind) {
            ui.c.line(static_cast<float>(pc.pixel.x), static_cast<float>(pc.pixel.y), static_cast<float>(pd.pixel.x),
                      static_cast<float>(pd.pixel.y), 1.0f, t.grid);
        }
    }
    const OrbitPoint head = orbit_project(view, camera, OrbitVec3{0.0f, 0.0f, 0.0f}, canvas_rect);
    if (!head.behind) {
        ui.c.fill_circle(head.pixel.x, head.pixel.y, static_cast<float>(ui.px(10)), t.accent, 50);
        ui.c.fill_circle(head.pixel.x, head.pixel.y, static_cast<float>(ui.px(5)), t.accent);
        ui.c.text(ui.f.caption, t.text_faint, rect_of(head.pixel.x - ui.px(40), head.pixel.y + ui.px(8), ui.px(80), ui.px(16)),
                  L"you", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    struct DepthItem {
        size_t index;
        float depth;
    };
    std::vector<DepthItem> order;
    for (size_t i = 0; i < s.layout.screens.size(); ++i) {
        const OrbitQuad quad = orbit_screen_quad(s.layout.screens[i]);
        const OrbitPoint centre = orbit_project(view, camera, quad.center, canvas_rect);
        if (!centre.behind) {
            order.push_back(DepthItem{i, centre.depth_m});
        }
    }
    std::sort(order.begin(), order.end(), [](const DepthItem& a, const DepthItem& b) { return a.depth > b.depth; });
    for (const DepthItem& item : order) {
        const ScreenLayout& screen = s.layout.screens[item.index];
        const OrbitQuad quad = orbit_screen_quad(screen);
        POINT points[5];
        bool visible = true;
        for (int corner = 0; corner < 4; ++corner) {
            const OrbitPoint projected = orbit_project(view, camera, quad.corners[corner], canvas_rect);
            if (projected.behind) {
                visible = false;
                break;
            }
            points[corner] = projected.pixel;
        }
        if (!visible) {
            continue;
        }
        points[4] = points[0];
        const COLORREF tint = screen_tint(screen);
        const bool selected = item.index == s.selected_screen;
        ui.c.fill_polygon(points, 4, mix(t.canvas, tint, selected ? (t.dark ? 0.45f : 0.36f) : (t.dark ? 0.28f : 0.2f)),
                          235);
        ui.c.polyline(points, 5, static_cast<float>(selected ? ui.px(3) : ui.px(2)), selected ? t.text : tint);
        const OrbitPoint centre = orbit_project(view, camera, quad.center, canvas_rect);
        const std::wstring label = editor_screen_label(screen);
        // A foreshortened quad can be narrower than its label: a soft plate
        // keeps the text readable where it crosses the screen's outline.
        HFONT label_font = selected ? ui.f.small_strong : ui.f.small;
        const int label_w = text_width(ui.c.dc(), label_font, label) + ui.px(12);
        const RECT plate = rect_of(centre.pixel.x - label_w / 2, centre.pixel.y - ui.px(10), label_w, ui.px(20));
        ui.c.fill_round(plate, ui.px(radius::sm), t.canvas, 200);
        ui.c.text(label_font, selected ? t.text : t.text_dim, plate, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    ui.c.pop_clip();
}

// Factory and user presets plus the preset actions, flowed into wrapping
// rows. With draw == false it only measures; returns the content height.
int presets_flow(Ui& ui, int left, int top, int width, bool draw) {
    const AppState& s = ui.s;
    const int chip_h = ui.px(size::control_sm);
    const int gap = ui.px(space::sm);
    const int caption_w = ui.px(116);
    const int help_w = help_mark_width(ui.dpi);
    int y = top;
    int x = left + caption_w;
    int row_h = chip_h;
    const auto newline = [&](int block_gap) {
        y += row_h + block_gap;
        x = left + caption_w;
        row_h = chip_h;
    };
    const auto place = [&](int w, int h) {
        if (x > left + caption_w && x + w > left + width) {
            newline(gap);
        }
        RECT r = rect_of(x, y, w, h);
        x += w + gap;
        row_h = std::max(row_h, h);
        return r;
    };
    if (draw) {
        ui.c.text(ui.f.small_strong, ui.t.text_faint, rect_of(left, y, caption_w, chip_h), L"Starter arcs");
    }
    for (const LayoutPreset preset : layout_presets()) {
        const std::wstring label = wide_from_utf8(layout_preset_label(preset));
        const int w = text_width(ui.c.dc(), ui.f.small_strong, label) + ui.px(32);
        RECT r = place(w, chip_h);
        if (draw) {
            const int id = ui_preset_id(preset);
            draw_chip(ui.c, r, label, false, ui.ws(id), ui.f.small_strong);
            ui.hot(id, 0, r);
        }
    }
    {
        RECT r = place(help_w, chip_h);
        if (draw) {
            ui.help(kHelpLayout, 0, HelpTopic::LayoutFactory, rect_of(r.left, r.top + (chip_h - help_w) / 2, help_w, help_w));
        }
    }
    newline(ui.px(space::md));
    if (draw) {
        ui.c.text(ui.f.small_strong, ui.t.text_faint, rect_of(left, y, caption_w, chip_h), L"Your presets");
    }
    if (s.preset_names.empty()) {
        const std::wstring hint = L"None yet. Arrange screens, then Save as preset.";
        RECT r = place(text_width(ui.c.dc(), ui.f.small, hint) + ui.px(8), chip_h);
        if (draw) {
            ui.c.text(ui.f.small, ui.t.text_faint, r, hint);
        }
    }
    for (size_t index = 0; index < s.preset_names.size() && index < 32; ++index) {
        const std::wstring label = wide_from_utf8(s.preset_names[index]);
        const bool loaded = s.preset_names[index] == s.loaded_preset;
        const int w = text_width(ui.c.dc(), ui.f.small_strong, label) + ui.px(32);
        RECT r = place(w, chip_h);
        if (draw) {
            const int id = ui_user_preset_id(index);
            draw_chip(ui.c, r, label, loaded, ui.ws(id), ui.f.small_strong);
            ui.hot(id, static_cast<int>(index), r);
        }
    }
    {
        RECT r = place(help_w, chip_h);
        if (draw) {
            ui.help(kHelpLayout, 1, HelpTopic::LayoutUserPresets, rect_of(r.left, r.top + (chip_h - help_w) / 2, help_w, help_w));
        }
    }
    // Preset actions sit on their own row, right after the user presets.
    x += ui.px(space::md);
    const struct {
        int id;
        const wchar_t* label;
        ButtonStyle style;
        bool enabled;
        int help_row;
        HelpTopic topic;
    } actions[] = {
        {kUiPresetSave, L"Save as preset", ButtonStyle::Secondary, true, 6, HelpTopic::LayoutPresetSave},
        {kUiPresetDelete, L"Delete preset", ButtonStyle::Danger, !s.loaded_preset.empty(), 7,
         HelpTopic::LayoutPresetDelete},
    };
    for (const auto& action : actions) {
        const int w = text_width(ui.c.dc(), ui.f.small_strong, action.label) + ui.px(36);
        RECT r = place(w + gap + help_w, chip_h);
        if (draw) {
            RECT b = rect_of(r.left, r.top, w, chip_h);
            draw_button(ui.c, b, action.label, action.style, ui.ws(action.id, action.enabled), ui.f.small_strong);
            ui.hot(action.id, 0, b, action.enabled);
            ui.help(kHelpLayout, action.help_row, action.topic,
                    rect_of(b.right + gap, r.top + (chip_h - help_w) / 2, help_w, help_w));
        }
    }
    return y + row_h - top;
}

// Screen chips plus Add/Remove; returns the height.
int screens_flow(Ui& ui, int left, int top, int width, bool draw) {
    const AppState& s = ui.s;
    const int chip_h = ui.px(size::control_sm);
    const int gap = ui.px(space::sm);
    const int help_w = help_mark_width(ui.dpi);
    int x = left;
    int y = top;
    const auto place = [&](int w) {
        if (x > left && x + w > left + width) {
            x = left;
            y += chip_h + gap;
        }
        RECT r = rect_of(x, y, w, chip_h);
        x += w + gap;
        return r;
    };
    for (size_t index = 0; index < s.layout.screens.size(); ++index) {
        const ScreenLayout& screen = s.layout.screens[index];
        const std::wstring label = ellipsize(wide_from_utf8(screen.id), 14);
        const int w = text_width(ui.c.dc(), ui.f.small_strong, label) + ui.px(44);
        RECT r = place(w);
        if (draw) {
            const int id = ui_screen_id(index);
            draw_chip(ui.c, r, label, index == s.selected_screen, ui.ws(id), ui.f.small_strong, screen_tint(screen));
            ui.hot(id, static_cast<int>(index), r);
        }
    }
    const bool can_add = s.layout.screens.size() < 8;
    const bool can_remove = s.layout.screens.size() > 1;
    const struct {
        int id;
        const wchar_t* label;
        const wchar_t* icon;
        ButtonStyle style;
        bool enabled;
        int help_row;
        HelpTopic topic;
    } actions[] = {
        {kUiAddScreen, L"Add", glyph::add, ButtonStyle::Secondary, can_add, 2, HelpTopic::LayoutAddScreen},
        {kUiRemoveScreen, L"Remove", glyph::remove, ButtonStyle::Danger, can_remove, 3, HelpTopic::LayoutRemoveScreen},
    };
    for (const auto& action : actions) {
        const int w = text_width(ui.c.dc(), ui.f.small_strong, action.label) + ui.px(52);
        RECT r = place(w + gap + help_w);
        if (draw) {
            RECT b = rect_of(r.left, r.top, w, chip_h);
            draw_button(ui.c, b, action.label, action.style, ui.ws(action.id, action.enabled), ui.f.small_strong,
                        action.icon, ui.f.icon_small);
            ui.hot(action.id, 0, b, action.enabled);
            ui.help(kHelpLayout, action.help_row, action.topic,
                    rect_of(b.right + gap, r.top + (chip_h - help_w) / 2, help_w, help_w));
        }
    }
    return y + chip_h - top;
}

void paint_editor_card(Ui& ui, const RECT& frame) {
    AppState& s = ui.s;
    const Theme& t = ui.t;
    card(ui, frame);
    const int pad = ui.px(space::xl);
    const int help_w = help_mark_width(ui.dpi);
    const int bar_h = ui.px(size::control_sm);
    const int top = frame.top + pad;

    // Toolbar, right to left: help (canvas), fullscreen + help, 2D/3D + help.
    int x = frame.right - pad;
    RECT help_canvas = rect_of(x - help_w, top + (bar_h - help_w) / 2, help_w, help_w);
    ui.help(kHelpEditor, 0, HelpTopic::EditorCanvas, help_canvas);
    x = help_canvas.left - ui.px(space::md);
    RECT fs = rect_of(x - bar_h, top, bar_h, bar_h);
    draw_icon_button(ui.c, fs, s.editor_fullscreen ? glyph::exit_fullscreen : glyph::fullscreen, ui.f.icon,
                     ui.ws(kUiEditorFullscreen), s.editor_fullscreen);
    ui.hot(kUiEditorFullscreen, 0, fs);
    RECT help_fs = rect_of(fs.left - ui.px(space::xs) - help_w, top + (bar_h - help_w) / 2, help_w, help_w);
    ui.help(kHelpEditor, 2, HelpTopic::EditorFullscreen, help_fs);
    x = help_fs.left - ui.px(space::lg);
    RECT seg = rect_of(x - ui.px(120), top, ui.px(120), bar_h);
    {
        static const wchar_t* const kNames[] = {L"2D", L"3D"};
        const float pos = ui.track(anim::kSegment, kUiEditor2D, s.editor_3d ? 1.0f : 0.0f, anim::kTauSlide);
        int hover = s.hover_id == kUiEditor2D ? 0 : s.hover_id == kUiEditor3D ? 1 : -1;
        int focus = !s.focus_visible ? -1 : s.focus_id == kUiEditor2D ? 0 : s.focus_id == kUiEditor3D ? 1 : -1;
        draw_segmented(ui.c, seg, kNames, 2, pos, hover, focus, true, ui.f.small_strong);
        ui.hot(kUiEditor2D, 0, segment_rect(seg, 2, 0));
        ui.hot(kUiEditor3D, 1, segment_rect(seg, 2, 1));
    }
    ui.help(kHelpEditor, 1, HelpTopic::EditorDimToggle,
            rect_of(seg.left - ui.px(space::xs) - help_w, top + (bar_h - help_w) / 2, help_w, help_w));
    // The title yields to the toolbar when the card is narrow.
    const int title_room = seg.left - ui.px(space::xs) - help_w - ui.px(space::md) - (frame.left + pad);
    if (title_room >= text_width(ui.c.dc(), ui.f.title, L"Arrangement")) {
        ui.c.text(ui.f.title, t.text, rect_of(frame.left + pad, top, title_room, bar_h), L"Arrangement");
    }

    const bool quick = s.editor_fullscreen && !s.layout.screens.empty() && s.selected_screen < s.layout.screens.size();
    std::wstring hint;
    if (s.editor_3d) {
        hint = L"Click a screen to select it and drag to move it. Drag empty space to orbit; scroll to zoom.";
    } else {
        hint = L"Drag a screen sideways for yaw and up or down for pitch. Tab to the map for arrow keys.";
    }
    // Hint and layout summary share one row when both fit; otherwise the
    // hint wraps across the full width and the summary goes on its own line
    // below, so neither is cut off.
    const std::wstring summary = wide_from_utf8(layout_display_summary(s.layout));
    const int row_w = rw(frame) - pad * 2;
    const int line_h = ui.px(24);
    const int summary_full_w = text_width(ui.c.dc(), ui.f.caption, summary);
    const bool one_row = text_width(ui.c.dc(), ui.f.caption, hint) + ui.px(space::lg) + summary_full_w <= row_w;
    const int hint_text_h = one_row ? 0 : wrapped_text_height(ui.c.dc(), ui.f.caption, row_w, hint);
    // On its own line the summary wraps rather than being cut short at narrow widths.
    const bool summary_wraps = !one_row && summary_full_w > row_w;
    const int summary_h = summary_wraps ? wrapped_text_height(ui.c.dc(), ui.f.caption, row_w, summary) + ui.px(space::xs)
                                        : line_h;
    const int hint_h = one_row ? line_h : ui.px(space::xs) + hint_text_h + summary_h;
    const int quick_h = quick ? ui.px(size::slider) * 2 + ui.px(space::md) + ui.px(size::control) + ui.px(space::md) : 0;
    RECT canvas_rect{frame.left + pad, top + bar_h + ui.px(space::lg), frame.right - pad,
                     frame.bottom - pad - hint_h - ui.px(space::sm) - quick_h};
    if (rh(canvas_rect) > ui.px(60)) {
        ui.c.fill_round(canvas_rect, ui.px(radius::md), t.canvas);
        ui.c.outline_round(canvas_rect, ui.px(radius::md), 1.0f, t.border);
        if (s.editor_3d) {
            paint_editor_3d(ui, canvas_rect);
        } else {
            paint_editor_2d(ui, canvas_rect);
        }
        if (s.focus_visible && s.focus_id == kUiEditorCanvas) {
            draw_focus_ring(ui.c, canvas_rect, ui.px(radius::md));
        }
        Hotspot editor;
        editor.id = kUiEditorCanvas;
        editor.rect = canvas_rect;
        editor.full = canvas_rect;
        ui.hs.push_back(editor);
    } else {
        canvas_rect.bottom = canvas_rect.top;
    }
    const int hint_y = canvas_rect.bottom + ui.px(space::sm);
    if (one_row) {
        ui.c.text(ui.f.caption, t.text_dim, rect_of(frame.right - pad - summary_full_w, hint_y, summary_full_w, line_h),
                  summary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        ui.c.text(ui.f.caption, t.text_faint,
                  rect_of(frame.left + pad, hint_y, std::max(0, row_w - summary_full_w - ui.px(space::lg)), line_h),
                  hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        ui.c.text(ui.f.caption, t.text_faint, rect_of(frame.left + pad, hint_y + ui.px(space::xs), row_w, hint_text_h),
                  hint, DT_LEFT | DT_TOP | DT_WORDBREAK);
        const RECT summary_rect = rect_of(frame.left + pad, hint_y + ui.px(space::xs) + hint_text_h, row_w, summary_h);
        if (summary_wraps) {
            ui.c.text(ui.f.caption, t.text_dim, rect_of(summary_rect.left, summary_rect.top + ui.px(space::xs), row_w,
                                                        summary_h - ui.px(space::xs)),
                      summary, DT_LEFT | DT_TOP | DT_WORDBREAK);
        } else {
            ui.c.text(ui.f.caption, t.text_dim, summary_rect, summary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    if (!quick) {
        return;
    }
    // Fullscreen quick controls for the selected screen.
    const ScreenLayout& screen = s.layout.screens[s.selected_screen];
    const LayoutField quick_fields[] = {LayoutField::Yaw,  LayoutField::Pitch, LayoutField::Distance,
                                        LayoutField::Roll, LayoutField::Width, LayoutField::Height};
    const int qleft = frame.left + pad;
    const int qwidth = rw(frame) - pad * 2;
    const int column_w = (qwidth - ui.px(space::xl) * 2) / 3;
    const int qtop = canvas_rect.bottom + ui.px(space::sm) + hint_h + ui.px(space::md);
    for (int index = 0; index < 6; ++index) {
        const LayoutField field = quick_fields[index];
        const float value = layout_field_value(s.layout, screen, field);
        RECT r = rect_of(qleft + (index % 3) * (column_w + ui.px(space::xl)), qtop + (index / 3) * ui.px(size::slider),
                         column_w, ui.px(size::slider));
        const std::wstring label = field == LayoutField::Distance ? L"Depth from eyes" : layout_field_label(field);
        slider(ui, ui_field_id(field), static_cast<int>(field), r, label,
               wide_from_utf8(format_layout_field_value(field, value)), layout_slider_fraction(field, value), true,
               s.drag_field == static_cast<int>(field));
    }
    const int ay = qtop + ui.px(size::slider) * 2 + ui.px(space::md);
    RECT save = rect_of(qleft, ay, ui.px(170), ui.px(size::control));
    button(ui, kUiSaveLayout, save, L"Save and apply", ButtonStyle::Primary, s.layout_dirty, glyph::save);
    RECT revert = rect_of(save.right + ui.px(space::md), ay, ui.px(140), ui.px(size::control));
    button(ui, kUiRevertLayout, revert, L"Revert", ButtonStyle::Secondary, s.layout_dirty, glyph::undo);
    ui.c.text(ui.f.small, s.layout_dirty ? t.warning : t.text_faint,
              rect_of(revert.right + ui.px(space::lg), ay,
                      std::max(0, qleft + qwidth - static_cast<int>(revert.right) - ui.px(space::lg)),
                      ui.px(size::control)),
              std::wstring(s.layout_dirty ? L"Unsaved changes" : L"Layout saved") + L"  ·  editing '" +
                  wide_from_utf8(screen.id) + L"'");
}

void paint_inspector(Ui& ui, const RECT& frame) {
    AppState& s = ui.s;
    const Theme& t = ui.t;
    card(ui, frame);
    const int pad = ui.px(space::xl);
    const int inner_w = rw(frame) - pad * 2;
    int y = frame.top + pad;
    ui.c.text(ui.f.title, t.text, rect_of(frame.left + pad, y, inner_w, ui.px(28)), L"Screens");
    y += ui.px(28) + ui.px(space::md);
    y += screens_flow(ui, frame.left + pad, y, inner_w, true) + ui.px(space::lg);
    ui.c.fill(rect_of(frame.left + pad, y, inner_w, 1), t.border);
    y += ui.px(space::lg);
    const bool has_screen = !s.layout.screens.empty() && s.selected_screen < s.layout.screens.size();
    if (!has_screen) {
        ui.c.text(ui.f.body, t.warning, rect_of(frame.left + pad, y, inner_w, ui.px(24)),
                  L"No screens yet. Add one to edit it.");
        hide_scroll(ui, kScrollInspector);
        return;
    }
    const ScreenLayout& screen = s.layout.screens[s.selected_screen];
    ui.c.text(ui.f.heading, t.text, rect_of(frame.left + pad, y, inner_w, ui.px(24)),
              L"Selected: " + wide_from_utf8(screen.id), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += ui.px(24) + ui.px(space::sm);

    struct Group {
        const wchar_t* title;
        std::vector<LayoutField> fields;
    };
    const Group groups[] = {
        {L"Placement", {LayoutField::Yaw, LayoutField::Pitch, LayoutField::Roll, LayoutField::Distance}},
        {L"Size", {LayoutField::Width, LayoutField::Height}},
        {L"View", {LayoutField::Fov}},
        {L"Capture rate", {LayoutField::ActiveFps, LayoutField::MidFps, LayoutField::IdleFps}},
        {L"Gaze thresholds", {LayoutField::EnterDeg, LayoutField::LeaveDeg}},
    };
    RECT view{frame.left + pad - ui.px(space::sm), y, frame.right - pad + ui.px(space::sm) - ui.px(space::xs),
              frame.bottom - ui.px(space::lg)};
    ScrollArea area = begin_scroll(ui, kScrollInspector, frame, view);
    const int content_top = y - area.offset;
    int cy = content_top;
    const int help_w = help_mark_width(ui.dpi);
    const int slider_w = inner_w - help_w - ui.px(space::md) - ui.px(space::sm);
    for (const Group& group : groups) {
        cy += ui.px(space::sm);
        ui.c.text(ui.f.caption, t.text_faint, rect_of(frame.left + pad, cy, inner_w, ui.px(18)), group.title);
        cy += ui.px(20);
        if (group.fields.front() == LayoutField::Width) {
            // Portrait leads the Size group: it turns the quad (width and
            // height swap) and the engine gives the screen a 1080 x 1920
            // desktop. Its help mark lines up with the sliders' column.
            cy += switch_row(ui, kUiScreenPortrait, frame.left + pad, cy, inner_w - ui.px(space::sm), L"Portrait",
                             L"Stand the screen on its end (1080 x 1920).", screen.portrait, true, kHelpFields,
                             static_cast<int>(LayoutField::Count), HelpTopic::FieldPortrait);
            cy += ui.px(space::xs);
        }
        for (const LayoutField field : group.fields) {
            const float value = layout_field_value(s.layout, screen, field);
            RECT r = rect_of(frame.left + pad, cy, slider_w, ui.px(size::slider));
            slider(ui, ui_field_id(field), static_cast<int>(field), r, layout_field_label(field),
                   wide_from_utf8(format_layout_field_value(field, value)), layout_slider_fraction(field, value), true,
                   s.drag_field == static_cast<int>(field));
            ui.help(kHelpFields, static_cast<int>(field), help_for_field(field),
                    rect_of(r.right + ui.px(space::md), cy + ui.px(28), help_w, help_w));
            cy += ui.px(size::slider);
        }
    }
    end_scroll(ui, area, cy - content_top + ui.px(space::sm));
}

}  // namespace

void paint_arc_preview(Ui& ui, const RECT& rect, const Layout& layout, int selected) {
    const EditorGeometry geometry = editor_geometry(layout, rect, ui.dpi);
    ui.c.push_clip(rect);
    for (int metres = 1; metres <= 3; ++metres) {
        ui.c.arc(geometry.head.x, geometry.head.y, static_cast<float>(metres) * geometry.scale, 180.0f, 180.0f, 1.0f,
                 ui.t.grid);
    }
    for (size_t index = 0; index < layout.screens.size(); ++index) {
        const ScreenLayout& screen = layout.screens[index];
        const float yaw = screen.yaw_deg * kPi / 180.0f;
        const POINT center = editor_screen_center(screen, geometry);
        const float half_width = std::max(static_cast<float>(ui.px(6)), screen.width_m * 0.5f * geometry.scale);
        const float dx = std::cos(yaw) * half_width;
        const float dy = std::sin(yaw) * half_width;
        const bool highlight = static_cast<int>(index) == selected;
        ui.c.line(static_cast<float>(center.x) - dx, static_cast<float>(center.y) - dy,
                  static_cast<float>(center.x) + dx, static_cast<float>(center.y) + dy,
                  static_cast<float>(ui.px(highlight ? 6 : 5)), highlight ? ui.t.accent : screen_tint(screen));
    }
    ui.c.fill_circle(geometry.head.x, geometry.head.y, static_cast<float>(ui.px(8)), ui.t.accent, 50);
    ui.c.fill_circle(geometry.head.x, geometry.head.y, static_cast<float>(ui.px(4)), ui.t.accent);
    ui.c.pop_clip();
}

void paint_layout_header_actions(Ui& ui, int right, int top, int height, int title_right) {
    const AppState& s = ui.s;
    const int help_w = help_mark_width(ui.dpi);
    int x = right;
    RECT help_save = rect_of(x - help_w, top + (height - help_w) / 2, help_w, help_w);
    ui.help(kHelpLayout, 4, HelpTopic::LayoutSave, help_save);
    RECT save = rect_of(help_save.left - ui.px(space::sm) - ui.px(176), top, ui.px(176), height);
    button(ui, kUiSaveLayout, save, L"Save and apply", ButtonStyle::Primary, true, glyph::save);
    RECT help_revert = rect_of(save.left - ui.px(space::lg) - help_w, top + (height - help_w) / 2, help_w, help_w);
    ui.help(kHelpLayout, 5, HelpTopic::LayoutRevert, help_revert);
    RECT revert = rect_of(help_revert.left - ui.px(space::sm) - ui.px(120), top, ui.px(120), height);
    button(ui, kUiRevertLayout, revert, L"Revert", ButtonStyle::Secondary, true, glyph::undo);
    if (s.layout_dirty) {
        const std::wstring text = L"Unsaved changes";
        const int w = text_width(ui.c.dc(), ui.f.small_strong, text) + ui.px(28);
        if (revert.left - ui.px(space::lg) - w < title_right + ui.px(space::lg)) {
            return;
        }
        draw_badge(ui.c, rect_of(revert.left - ui.px(space::lg) - w, top + (height - ui.px(26)) / 2, w, ui.px(26)), text,
                   ui.t.warning, ui.f.small_strong);
    }
}

void paint_layout_page(Ui& ui, const RECT& body) {
    AppState& s = ui.s;
    for (int i = 0; i < kUiSplitCount; ++i) {
        s.split_track[i].valid = false;
    }
    if (s.editor_fullscreen) {
        hide_scroll(ui, kScrollInspector);
        paint_editor_card(ui, body);
        return;
    }
    const int gap = ui.px(space::xl);
    const int pad = ui.px(space::xl);
    // Presets card. In a short window (many presets near the minimum size)
    // it is capped so the editor and inspector keep a usable height, and its
    // chips scroll inside it. The page scroll portal is otherwise unused on
    // this page, so it carries that scroll.
    const int inner_w = rw(body) - pad * 2;
    const int presets_h = presets_flow(ui, body.left + pad, 0, inner_w, false);
    const int min_main = ui.px(280);
    const int presets_cap =
        std::max(pad * 2 + ui.px(size::control_sm), rh(body) - gap - min_main);
    const bool presets_scroll = presets_h + pad * 2 > presets_cap;
    RECT presets = rect_of(body.left, body.top, rw(body), presets_scroll ? presets_cap : presets_h + pad * 2);
    card(ui, presets);
    if (presets_scroll) {
        // The bar sits in the card's right padding, as in the inspector.
        RECT view{presets.left + pad - ui.px(space::sm), presets.top + pad - ui.px(space::sm),
                  presets.right - pad + ui.px(space::sm) - ui.px(space::xs), presets.bottom - pad + ui.px(space::sm)};
        ScrollArea area = begin_scroll(ui, kScrollPage, presets, view);
        presets_flow(ui, body.left + pad, presets.top + pad - area.offset, inner_w, true);
        end_scroll(ui, area, presets_h + ui.px(space::sm) * 2);
    } else {
        hide_scroll(ui, kScrollPage);
        presets_flow(ui, body.left + pad, body.top + pad, inner_w, true);
    }

    // Main row: editor | splitter | inspector.
    RECT main{body.left, presets.bottom + gap, body.right, body.bottom};
    const int main_w = rw(main);
    const int min_editor = ui.px(340);
    const int min_inspector = ui.px(320);
    int inspector_w = std::clamp(main_w * 36 / 100, ui.px(340), ui.px(460));
    if (s.config.splits.column >= 0.0f) {
        inspector_w = static_cast<int>(std::lround(s.config.splits.column * static_cast<float>(main_w)));
    }
    inspector_w = std::clamp(inspector_w, min_inspector, std::max(min_inspector, main_w - gap - min_editor));
    RECT editor{main.left, main.top, main.right - gap - inspector_w, main.bottom};
    RECT inspector{editor.right + gap, main.top, main.right, main.bottom};
    if (rh(main) < ui.px(160)) {
        hide_scroll(ui, kScrollInspector);
        return;
    }
    paint_editor_card(ui, editor);
    paint_inspector(ui, inspector);

    // Column splitter (persisted as the inspector's share of the row).
    const int cx = editor.right + gap / 2;
    const int grip = ui.px(10);
    RECT zone{cx - grip / 2, main.top + ui.px(space::lg), cx + grip / 2, main.bottom - ui.px(space::lg)};
    SplitterTrack& track = s.split_track[4];
    track.zone = zone;
    track.origin_px = cx;
    track.min_px = main.left + min_editor + gap / 2;
    track.max_px = main.right - min_inspector - gap / 2;
    if (track.max_px < track.min_px) {
        track.min_px = track.max_px = cx;
    }
    track.span_px = std::max(1, main_w);
    track.span_start_px = main.right - gap / 2;
    track.column = 2;
    track.index = 0;
    track.from_right = true;
    track.valid = true;
    const int split_id = kUiSplitBase + 4;
    const float active = ui.track(anim::kHover, split_id, s.hover_id == split_id || s.drag_splitter == 4 ? 1.0f : 0.0f);
    if (active > 0.0f) {
        RECT line_rect{cx - ui.px(2), zone.top + ui.px(40), cx + ui.px(2), zone.bottom - ui.px(40)};
        ui.c.fill_round(line_rect, ui.px(2), ui.t.accent, static_cast<int>(active * 255.0f));
    } else {
        RECT handle{cx - ui.px(2), (zone.top + zone.bottom) / 2 - ui.px(18), cx + ui.px(2),
                    (zone.top + zone.bottom) / 2 + ui.px(18)};
        ui.c.fill_round(handle, ui.px(2), ui.t.border_strong);
    }
    ui.hot(split_id, 0, zone);
}

}  // namespace detail
}  // namespace gt::ui
