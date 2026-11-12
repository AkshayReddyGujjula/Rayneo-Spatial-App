#include "app_shell/ui_paint.h"

#include "app/engine_protocol.h"
#include "app_shell/ui_paint_internal.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace gt::ui {

const Theme& current_theme(const AppState& state) {
    return state.dark ? dark_theme() : light_theme();
}

int sidebar_width(int client_width, UINT dpi) {
    return client_width < scale_px(1000, dpi) ? scale_px(size::sidebar_compact, dpi)
                                              : scale_px(size::sidebar, dpi);
}

namespace detail {

void Ui::hot(int id, int arg, const RECT& rect, bool enabled) {
    Hotspot spot;
    spot.id = id;
    spot.arg = arg;
    spot.full = rect;
    spot.enabled = enabled;
    spot.panel = panel;
    if (panel >= 0) {
        RECT clipped{};
        if (IntersectRect(&clipped, &rect, &view) == 0) {
            return;
        }
        spot.rect = clipped;
    } else {
        spot.rect = rect;
    }
    hs.push_back(spot);
}

WidgetState Ui::ws(int id, bool enabled) {
    WidgetState state;
    state.enabled = enabled;
    state.hover = track(anim::kHover, id, enabled && s.hover_id == id ? 1.0f : 0.0f);
    state.press = track(anim::kPress, id, enabled && s.press_id == id ? 1.0f : 0.0f, 0.035f);
    state.focused = s.focus_visible && s.focus_id == id;
    return state;
}

float Ui::track(uint32_t channel, int id, float target, float tau) {
    return s.anim.track(anim::key(channel, id), target, tau);
}

void Ui::help(int help_panel, int row, HelpTopic topic, const RECT& rect) {
    const int id = ui_help_id(help_panel, row);
    draw_help_mark(c, rect, ws(id), f.small_strong);
    hot(id, static_cast<int>(topic), rect);
}

ScrollArea begin_scroll(Ui& ui, ScrollPanel panel, const RECT& frame, const RECT& view) {
    ScrollArea area;
    area.panel = panel;
    area.frame = frame;
    area.view = view;
    const ScrollPortal& portal = ui.s.scroll[panel];
    area.offset = std::clamp(portal.offset, 0, std::max(0, portal.max_offset));
    area.saved_panel = ui.panel;
    area.saved_view = ui.view;
    ui.panel = panel;
    ui.view = view;
    ui.c.push_clip(view);
    return area;
}

void end_scroll(Ui& ui, ScrollArea& area, int content_h) {
    ui.c.pop_clip();
    ui.panel = area.saved_panel;
    ui.view = area.saved_view;
    ScrollPortal& portal = ui.s.scroll[area.panel];
    const int view_h = std::max(0, rh(area.view));
    const int max_offset = std::max(0, content_h - view_h);
    portal.panel = area.frame;
    portal.view = area.view;
    portal.content_h = content_h;
    portal.max_offset = max_offset;
    portal.last_max = max_offset;
    portal.visible = max_offset > 0 && view_h > 0;
    if (area.offset > max_offset) {
        portal.offset = max_offset;
        ui.s.repaint_requested = true;
    } else {
        portal.offset = area.offset;
    }
    portal.scrollbar = RECT{};
    portal.thumb = RECT{};
    if (!portal.visible) {
        return;
    }
    const int bar_w = ui.px(10);
    RECT track_rect{area.view.right + ui.px(4), area.view.top + ui.px(2),
                    area.view.right + ui.px(4) + bar_w, area.view.bottom - ui.px(2)};
    const int track_h = std::max(1, rh(track_rect));
    const int thumb_h = std::clamp(track_h * view_h / std::max(1, content_h),
                                   std::min(track_h, ui.px(28)), track_h);
    const int travel = std::max(1, track_h - thumb_h);
    const int thumb_top = track_rect.top + portal.offset * travel / std::max(1, max_offset);
    RECT thumb{track_rect.left, thumb_top, track_rect.right, thumb_top + thumb_h};
    const int scroll_id = static_cast<int>(kUiScrollBase) + static_cast<int>(area.panel);
    const float hover = ui.track(anim::kHover, scroll_id,
                                 ui.s.hover_id == scroll_id || ui.s.drag_scroll_panel == area.panel
                                     ? 1.0f
                                     : 0.0f);
    draw_scrollbar(ui.c, track_rect, thumb, hover);
    portal.scrollbar = track_rect;
    portal.thumb = thumb;
    Hotspot spot;
    spot.id = scroll_id;
    spot.arg = static_cast<int>(area.panel);
    spot.rect = track_rect;
    spot.full = track_rect;
    spot.panel = static_cast<int>(area.panel);
    ui.hs.push_back(spot);
}

void hide_scroll(Ui& ui, ScrollPanel panel) {
    ScrollPortal& portal = ui.s.scroll[panel];
    const int offset = portal.offset;
    portal = ScrollPortal{};
    portal.offset = offset;
}

void card(Ui& ui, const RECT& rect) {
    draw_card(ui.c, rect);
}

int card_header(Ui& ui, const RECT& card_rect, const wchar_t* title, const wchar_t* description) {
    const int pad = ui.px(space::xl);
    int y = card_rect.top + pad;
    const int width = rw(card_rect) - pad * 2;
    ui.c.text(ui.f.title, ui.t.text, rect_of(card_rect.left + pad, y, width, ui.px(28)), title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += ui.px(30);
    if (description != nullptr && *description != L'\0') {
        y += paragraph(ui, rect_of(card_rect.left + pad, y, width, 0), ui.f.small, ui.t.text_dim,
                       description);
    }
    return y + ui.px(space::lg);
}

int paragraph(Ui& ui, const RECT& area, HFONT font, COLORREF color, const std::wstring& text) {
    const int width = rw(area);
    const int height = wrapped_text_height(ui.c.dc(), font, width, text);
    RECT r = rect_of(area.left, area.top, width, height);
    ui.c.text(font, color, r, text, DT_LEFT | DT_TOP | DT_WORDBREAK);
    return height;
}

void button(Ui& ui, int id, const RECT& rect, const std::wstring& label, ButtonStyle style,
            bool enabled, const wchar_t* icon) {
    draw_button(ui.c, rect, label, style, ui.ws(id, enabled), ui.f.body_strong, icon, ui.f.icon);
    ui.hot(id, 0, rect, enabled);
}

int switch_row(Ui& ui, int id, int left, int top, int width, const wchar_t* title,
               const std::wstring& description, bool value, bool enabled, int help_panel,
               int help_row, HelpTopic topic) {
    const int help_w = help_mark_width(ui.dpi);
    const int switch_w = ui.px(size::switch_w);
    const int switch_h = ui.px(size::switch_h);
    const int text_w = width - switch_w - help_w - ui.px(space::lg) * 2;
    const int title_h = ui.px(22);
    const int desc_h = description.empty()
                           ? 0
                           : wrapped_text_height(ui.c.dc(), ui.f.small, text_w, description);
    const int row_h = std::max(ui.px(44), title_h + (desc_h > 0 ? desc_h + ui.px(2) : 0));
    ui.c.text(ui.f.body_strong, enabled ? ui.t.text : ui.t.disabled_text,
              rect_of(left, top, text_w, title_h), title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (desc_h > 0) {
        ui.c.text(ui.f.small, enabled ? ui.t.text_dim : ui.t.disabled_text,
                  rect_of(left, top + title_h + ui.px(2), text_w, desc_h), description,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    const int switch_left = left + width - help_w - ui.px(space::lg) - switch_w;
    const int switch_top = top + (std::min(row_h, ui.px(44)) - switch_h) / 2;
    RECT track_rect = rect_of(switch_left, switch_top, switch_w, switch_h);
    const float on = ui.track(anim::kToggle, id, value ? 1.0f : 0.0f, anim::kTauToggle);
    const WidgetState state = ui.ws(id, enabled);
    draw_switch(ui.c, track_rect, on, state);
    // The whole row is the hit target; the switch is just the indicator.
    ui.hot(id, 0, rect_of(left, top, switch_left + switch_w - left, row_h), enabled);
    ui.help(help_panel, help_row, topic,
            rect_of(left + width - help_w, switch_top + (switch_h - help_w) / 2, help_w, help_w));
    return row_h;
}

void segmented(Ui& ui, int base_id, const RECT& rect, const wchar_t* const* labels, int count,
               int selected, bool enabled) {
    const float pos = ui.track(anim::kSegment, base_id, static_cast<float>(selected), anim::kTauSlide);
    int hover = -1;
    int focus = -1;
    for (int i = 0; i < count; ++i) {
        if (ui.s.hover_id == base_id + i) {
            hover = i;
        }
        if (ui.s.focus_visible && ui.s.focus_id == base_id + i) {
            focus = i;
        }
    }
    draw_segmented(ui.c, rect, labels, count, selected >= 0 ? pos : -1.0f, hover, focus, enabled,
                   ui.f.body_strong);
    for (int i = 0; i < count; ++i) {
        ui.hot(base_id + i, i, segment_rect(rect, count, i), enabled);
    }
}

void slider(Ui& ui, int id, int arg, const RECT& rect, const std::wstring& label,
            const std::wstring& value_text, float fraction, bool enabled, bool dragging) {
    float shown = fraction;
    if (dragging) {
        ui.s.anim.snap(anim::key(anim::kSlider, id), fraction);
    } else {
        shown = ui.track(anim::kSlider, id, fraction, anim::kTauSlide);
    }
    WidgetState state = ui.ws(id, enabled);
    if (dragging) {
        state.press = 1.0f;
    }
    draw_slider(ui.c, rect, label, value_text, shown, state, ui.f.small, ui.f.body_strong);
    ui.hot(id, arg, rect, enabled);
}

COLORREF severity_colour(const Theme& theme, Severity severity) {
    switch (severity) {
        case Severity::Ok:
            return theme.ok;
        case Severity::Warning:
            return theme.warning;
        case Severity::Error:
            return theme.error;
        case Severity::Neutral:
        default:
            return theme.text_faint;
    }
}

const wchar_t* severity_glyph(Severity severity) {
    switch (severity) {
        case Severity::Ok:
            return glyph::check;
        case Severity::Warning:
            return glyph::warning;
        case Severity::Error:
            return glyph::error;
        case Severity::Neutral:
        default:
            return glyph::info;
    }
}

float pulse_phase(Ui& ui, bool active) {
    if (!active || ui.s.anim.reduce_motion()) {
        return -1.0f;
    }
    ui.s.anim.request_frames();
    const double period = 1.6;
    return static_cast<float>(std::fmod(ui.s.now_s, period) / period);
}

EngineView engine_view(const AppState& state, const Theme& theme) {
    EngineView view;
    const EngineSnapshot& engine = state.engine.snapshot();
    view.busy = state.engine.busy();
    switch (engine.state) {
        case EngineProcessState::Running:
            view.color = theme.ok;
            view.label = engine.mode == LaunchMode::Preview ? L"Preview running"
                                                            : L"Workspace running";
            view.headline = engine.mode == LaunchMode::Preview ? L"Preview is running"
                            : engine.mode == LaunchMode::Workspace
                                ? L"Your workspace is running"
                                : L"The engine is running";
            if (state.engine_status.parsed) {
                view.detail = wide_from_utf8(engine_status_summary(state.engine_status));
            } else {
                view.detail = L"Put the glasses on and look around. Recenter whenever the centre "
                              L"screen drifts.";
            }
            break;
        case EngineProcessState::Starting:
            view.color = theme.warning;
            view.transitional = true;
            view.label = L"Starting";
            view.headline = L"Starting the engine";
            view.detail = L"Preparing the screens and head tracking. This takes a few seconds.";
            break;
        case EngineProcessState::Stopping:
            view.color = theme.warning;
            view.transitional = true;
            view.label = L"Stopping";
            view.headline = L"Restoring your displays";
            view.detail = L"The engine removes its virtual screens and restores your display "
                          L"layout. This can take up to a minute.";
            break;
        case EngineProcessState::Failed:
            view.color = theme.error;
            view.label = L"Failed";
            view.headline = L"The engine stopped with an error";
            view.detail = engine.last_error.empty()
                              ? std::wstring(L"Open the engine log on the Diagnostics page.")
                              : wide_from_utf8(engine.last_error);
            break;
        case EngineProcessState::Exited:
        case EngineProcessState::Stopped:
        default: {
            view.color = theme.text_faint;
            view.label = L"Stopped";
            const std::wstring workspace_block = start_block_reason(state, LaunchMode::Workspace);
            const std::wstring preview_block = start_block_reason(state, LaunchMode::Preview);
            if (workspace_block.empty()) {
                view.headline = L"Ready to start";
                view.detail = L"Your glasses are connected and calibrated. Start the workspace "
                              L"to see your screens around you.";
                view.color = theme.accent;
            } else if (preview_block.empty()) {
                view.headline = L"Preview is ready";
                view.detail = L"The full workspace is not available yet: " + workspace_block;
                view.color = theme.warning;
            } else {
                view.headline = L"Setup needed";
                view.detail = workspace_block;
                view.color = theme.warning;
            }
            break;
        }
    }
    if (view.busy && engine.state == EngineProcessState::Stopped) {
        view.label = L"Running";
    }
    return view;
}

std::vector<HealthItem> health_items(const AppState& state) {
    const DiagnosticsReport& report = state.diagnostics;
    std::vector<HealthItem> items;
    {
        HealthItem item;
        item.title = L"Engine";
        const EngineSnapshot& engine = state.engine.snapshot();
        item.value = engine_process_state_label(engine.state);
        if (engine.process_alive) {
            item.value += L"  (pid " + std::to_wstring(engine.process_id) + L")";
        }
        if (!engine.last_error.empty()) {
            item.detail = wide_from_utf8(engine.last_error);
            item.severity = Severity::Error;
        } else if (state.engine_status.parsed) {
            item.detail = wide_from_utf8(engine_status_summary(state.engine_status));
            item.severity = Severity::Ok;
        } else if (state.engine.busy()) {
            item.detail = L"Engine window found; waiting for its status file.";
            item.severity = Severity::Warning;
        } else {
            item.detail = L"Not running. Start the workspace or a preview from Home.";
        }
        item.topic = HelpTopic::StatusEngine;
        item.status_row = 0;
        items.push_back(item);
    }
    {
        HealthItem item;
        item.title = L"Glasses display";
        item.value = report.glasses_found ? L"Connected" : L"Not detected";
        item.detail = report.glasses_found
                          ? report.glasses_detail
                          : L"Connect the glasses, press Win+P and choose Extend.";
        item.severity = report.glasses_found ? Severity::Ok : Severity::Error;
        item.topic = HelpTopic::StatusDisplay;
        item.status_row = 1;
        items.push_back(item);
    }
    {
        HealthItem item;
        item.title = L"Head calibration";
        item.value = report.calibration_valid ? L"Valid" : L"Needed";
        item.detail = report.calibration_valid
                          ? report.calibration_detail
                          : std::wstring(L"Run the head calibration once on the Tracking page.");
        item.raw = report.calibration_detail;
        item.severity = report.calibration_valid ? Severity::Ok : Severity::Warning;
        item.topic = HelpTopic::StatusCalibration;
        item.status_row = 2;
        items.push_back(item);
    }
    {
        HealthItem item;
        item.title = L"Head-tracking sensor";
        item.value = report.hid_device_count > 0 ? L"Found" : L"Not found";
        item.detail = report.hid_device_count > 0
                          ? report.hid_detail
                          : std::wstring(L"Check the USB-C cable and that the glasses are switched on.");
        item.raw = report.hid_detail;
        item.severity = report.hid_device_count > 0 ? Severity::Ok : Severity::Error;
        item.topic = HelpTopic::StatusHid;
        item.status_row = 3;
        items.push_back(item);
    }
    {
        HealthItem item;
        item.title = L"Virtual display driver";
        item.value = report.vdd_ready ? L"Ready" : L"Not ready";
        item.detail = report.vdd_ready
                          ? report.vdd_status_text
                          : std::wstring(L"Needed only for the full workspace; preview works without it.");
        item.raw = L"Parsec VDD: " + report.vdd_status_text + (report.vdd_ready ? L"" : L". " + item.detail);
        item.severity = report.vdd_ready ? Severity::Ok : Severity::Warning;
        item.topic = HelpTopic::StatusVdd;
        item.status_row = 4;
        items.push_back(item);
    }
    {
        HealthItem item;
        item.title = L"Layout file";
        item.value = report.layout_ok ? L"Valid" : L"Problem";
        item.detail = report.layout_detail;
        item.severity = report.layout_ok ? Severity::Ok : Severity::Error;
        item.topic = HelpTopic::StatusLayoutFile;
        item.status_row = 5;
        items.push_back(item);
    }
    {
        HealthItem item;
        item.title = L"Telemetry";
        item.value = report.telemetry_present ? L"Recording" : L"No data yet";
        item.detail = report.telemetry_detail.empty()
                          ? std::wstring(L"Written while the engine runs.")
                          : report.telemetry_detail;
        item.severity = report.telemetry_present ? Severity::Ok : Severity::Neutral;
        item.topic = HelpTopic::StatusTelemetry;
        item.status_row = 6;
        items.push_back(item);
    }
    {
        HealthItem item;
        item.title = L"Engine log";
        std::error_code size_error;
        const uint64_t bytes = std::filesystem::file_size(state.paths.engine_log, size_error);
        item.value = size_error ? L"Not written yet" : format_bytes_wide(bytes);
        item.detail = state.paths.engine_log.wstring();
        item.severity = Severity::Neutral;
        item.topic = HelpTopic::StatusEngineLog;
        item.status_row = 7;
        items.push_back(item);
    }
    return items;
}

}  // namespace detail

namespace {

using namespace detail;

// Logo mark: rounded accent plate with an arc and three screen ticks.
void paint_logo(Ui& ui, int left, int top, int size_px) {
    // Three screens on an arc around the wearer, seen from above: the same
    // picture as the arrangement editor.
    RECT plate = rect_of(left, top, size_px, size_px);
    ui.c.fill_round(plate, size_px * 28 / 100, ui.t.accent);
    const float s = static_cast<float>(size_px);
    const float cx = static_cast<float>(left) + s * 0.5f;
    const float cy = static_cast<float>(top) + s * 0.72f;
    const float r = s * 0.36f;
    const float thickness = std::max(2.0f, s * 0.1f);
    const float half = s * 0.13f;
    const float angles[3] = {-0.95f, 0.0f, 0.95f};
    for (const float angle : angles) {
        const float x = cx + std::sin(angle) * r;
        const float y = cy - std::cos(angle) * r;
        ui.c.line(x - half * std::cos(angle), y - half * std::sin(angle), x + half * std::cos(angle),
                  y + half * std::sin(angle), thickness, ui.t.accent_text);
    }
    ui.c.fill_circle(static_cast<int>(cx), static_cast<int>(cy), thickness * 0.75f, ui.t.accent_text);
}

const wchar_t* page_glyph(AppPage page) {
    switch (page) {
        case AppPage::Home:
            return glyph::home;
        case AppPage::Layout:
            return glyph::layout;
        case AppPage::Comfort:
            return glyph::comfort;
        case AppPage::Tracking:
            return glyph::tracking;
        case AppPage::Diagnostics:
            return glyph::diagnostics;
        case AppPage::Settings:
        case AppPage::Count:
        default:
            return glyph::settings;
    }
}

void paint_sidebar(Ui& ui, const RECT& side) {
    const Theme& t = ui.t;
    ui.c.fill(side, t.sidebar);
    ui.c.fill(RECT{side.right - 1, side.top, side.right, side.bottom}, t.border);
    const bool compact = rw(side) < ui.px(150);
    const int inset = ui.px(space::md);

    // Brand.
    const int logo = ui.px(34);
    const int logo_left = compact ? side.left + (rw(side) - logo) / 2 : side.left + ui.px(22);
    paint_logo(ui, logo_left, side.top + ui.px(24), logo);
    if (!compact) {
        const int text_left = logo_left + logo + ui.px(12);
        ui.c.text(ui.f.heading, t.text, rect_of(text_left, side.top + ui.px(22), rw(side) - text_left, ui.px(20)),
                  L"RayNeo Spatial");
        ui.c.text(ui.f.caption, t.text_faint,
                  rect_of(text_left, side.top + ui.px(42), rw(side) - text_left, ui.px(18)),
                  L"Workspace controller");
    }

    // Navigation with a sliding selection highlight.
    const int item_h = ui.px(size::nav_item);
    const int item_gap = ui.px(space::xs);
    const int items_top = side.top + ui.px(92);
    const int selected = static_cast<int>(ui.s.page);
    const float pos = ui.track(anim::kNav, 0, static_cast<float>(selected), anim::kTauSlide);
    const int highlight_top =
        items_top + static_cast<int>(std::lround(pos * static_cast<float>(item_h + item_gap)));
    RECT highlight = rect_of(side.left + inset, highlight_top, rw(side) - inset * 2, item_h);
    ui.c.fill_round(highlight, ui.px(radius::md), t.accent_soft);
    ui.c.fill_round(rect_of(highlight.left, highlight.top + ui.px(12), ui.px(3), item_h - ui.px(24)),
                    ui.px(2), t.accent);
    for (int i = 0; i < kAppPageCount; ++i) {
        const auto page = static_cast<AppPage>(i);
        const int id = kUiNavBase + i;
        RECT item = rect_of(side.left + inset, items_top + i * (item_h + item_gap), rw(side) - inset * 2,
                            item_h);
        const WidgetState state = ui.ws(id);
        const bool active = i == selected;
        if (!active) {
            ui.c.fill_round(item, ui.px(radius::md), t.surface_hover,
                            static_cast<int>(state.hover * 200.0f));
        }
        const COLORREF fg = active ? t.text : mix(t.text_dim, t.text, state.hover);
        if (compact) {
            draw_icon(ui.c, item, page_glyph(page), ui.f.icon, active ? t.accent : fg);
        } else {
            draw_icon(ui.c, rect_of(item.left + ui.px(12), item.top, ui.px(24), item_h), page_glyph(page),
                      ui.f.icon, active ? t.accent : fg);
            ui.c.text(active ? ui.f.body_strong : ui.f.body, fg,
                      rect_of(item.left + ui.px(48), item.top, rw(item) - ui.px(56), item_h),
                      app_page_label(page));
        }
        if (state.focused) {
            draw_focus_ring(ui.c, item, ui.px(radius::md));
        }
        ui.hot(id, i, item);
    }

    // Engine status, always visible: answers "what state am I in" anywhere.
    const EngineView view = engine_view(ui.s, t);
    const int card_h = compact ? ui.px(52) : ui.px(68);
    RECT status = rect_of(side.left + inset, side.bottom - card_h - ui.px(space::lg), rw(side) - inset * 2,
                          card_h);
    const WidgetState state = ui.ws(kUiStatusCard);
    ui.c.fill_round(status, ui.px(radius::md), mix(t.surface_alt, t.surface_hover, state.hover));
    ui.c.outline_round(status, ui.px(radius::md), 1.0f, t.border);
    const COLORREF dot = view.busy ? (view.transitional ? t.warning : t.ok) : t.text_faint;
    const float phase = pulse_phase(ui, view.transitional);
    if (compact) {
        draw_status_dot(ui.c, (status.left + status.right) / 2, (status.top + status.bottom) / 2,
                        ui.px(6), dot, phase);
    } else {
        draw_status_dot(ui.c, status.left + ui.px(20), status.top + card_h / 2, ui.px(5), dot, phase);
        ui.c.text(ui.f.caption, t.text_faint,
                  rect_of(status.left + ui.px(36), status.top + ui.px(12), rw(status) - ui.px(44), ui.px(18)),
                  L"Engine");
        ui.c.text(ui.f.body_strong, t.text,
                  rect_of(status.left + ui.px(36), status.top + ui.px(30), rw(status) - ui.px(44), ui.px(22)),
                  view.busy ? view.label : std::wstring(L"Stopped"),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (state.focused) {
        draw_focus_ring(ui.c, status, ui.px(radius::md));
    }
    ui.hot(kUiStatusCard, 0, status);
}

int paint_banner(Ui& ui, int left, int top, int width) {
    if (ui.s.banner.empty()) {
        return 0;
    }
    const Theme& t = ui.t;
    const COLORREF color = severity_colour(t, ui.s.banner_severity);
    const int pad = ui.px(space::lg);
    const int icon_w = ui.px(24);
    const int close_w = ui.px(32);
    const int text_w = width - pad * 2 - icon_w - close_w - ui.px(space::md) * 2;
    const int text_h = wrapped_text_height(ui.c.dc(), ui.f.body, text_w, ui.s.banner);
    const int height = std::max(ui.px(52), text_h + pad * 2);
    RECT box = rect_of(left, top, width, height);
    ui.c.fill_round(box, ui.px(radius::lg), mix(t.surface, color, t.dark ? 0.13f : 0.09f));
    ui.c.outline_round(box, ui.px(radius::lg), 1.0f, mix(t.border, color, 0.45f));
    draw_icon(ui.c, rect_of(left + pad, top, icon_w, height), severity_glyph(ui.s.banner_severity),
              ui.f.icon, color);
    ui.c.text(ui.f.body, t.text,
              rect_of(left + pad + icon_w + ui.px(space::md), top + (height - text_h) / 2, text_w, text_h),
              ui.s.banner, DT_LEFT | DT_TOP | DT_WORDBREAK);
    RECT close = rect_of(box.right - pad / 2 - close_w, top + (height - close_w) / 2, close_w, close_w);
    draw_icon_button(ui.c, close, glyph::close, ui.f.icon_small, ui.ws(kUiBannerDismiss));
    ui.hot(kUiBannerDismiss, 0, close);
    return height;
}

void paint_page_actions(Ui& ui, int right, int top, int height) {
    if (ui.s.page == AppPage::Layout) {
        paint_layout_header_actions(ui, right, top, height);
        return;
    }
    if (ui.s.page == AppPage::Diagnostics || ui.s.page == AppPage::Home) {
        const int help_w = help_mark_width(ui.dpi);
        RECT help_rect = rect_of(right - help_w, top + (height - help_w) / 2, help_w, help_w);
        ui.help(kHelpHeader, 0, HelpTopic::HeaderRefresh, help_rect);
        const int button_w = ui.px(124);
        RECT refresh = rect_of(help_rect.left - ui.px(space::sm) - button_w, top, button_w, height);
        button(ui, kUiRefresh, refresh, L"Refresh", ButtonStyle::Secondary, true, glyph::refresh);
    }
}

void paint_content(Ui& ui, const RECT& content) {
    AppState& s = ui.s;
    if (s.page == AppPage::Layout && s.editor_fullscreen) {
        RECT area = content;
        InflateRect(&area, -ui.px(space::xl), -ui.px(space::xl));
        hide_scroll(ui, kScrollPage);
        paint_layout_page(ui, area);
        return;
    }
    const bool narrow = rw(content) < ui.px(760);
    const int pad = narrow ? ui.px(space::xl) : ui.px(40);
    int left = content.left + pad;
    int right = content.right - pad;
    if (s.page != AppPage::Layout) {
        const int max_w = ui.px(1180);
        if (right - left > max_w) {
            left += (right - left - max_w) / 2;
            right = left + max_w;
        }
    }
    // Page entrance: the new page eases up a few pixels while the old frame
    // fades out over it (see AppWindow::on_paint).
    const float progress = s.page_transition.progress(s.now_s);
    if (progress < 1.0f) {
        s.anim.request_frames();
    }
    const int dy = s.anim.reduce_motion()
                       ? 0
                       : static_cast<int>(std::lround((1.0f - anim::ease_out_cubic(progress)) *
                                                      static_cast<float>(ui.px(14))));
    int y = content.top + ui.px(30) + dy;
    const int actions_h = ui.px(size::control);
    const int actions_w = std::min(ui.px(420), (right - left) / 2);
    ui.c.text(ui.f.display, ui.t.text, rect_of(left, y, right - left - actions_w, ui.px(38)),
              app_page_title(s.page), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    paint_page_actions(ui, right, y, actions_h);
    y += ui.px(40);
    ui.c.text(ui.f.body, ui.t.text_dim, rect_of(left, y, right - left, ui.px(22)),
              app_page_subtitle(s.page), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += ui.px(22) + ui.px(space::xl);
    const int banner_h = paint_banner(ui, left, y, right - left);
    if (banner_h > 0) {
        y += banner_h + ui.px(space::lg);
    }
    RECT body{left, y, right, content.bottom};
    switch (s.page) {
        case AppPage::Home:
            paint_home(ui, body);
            break;
        case AppPage::Layout:
            hide_scroll(ui, kScrollPage);
            body.bottom -= ui.px(space::xl);
            paint_layout_page(ui, body);
            break;
        case AppPage::Comfort:
            paint_comfort(ui, body);
            break;
        case AppPage::Tracking:
            paint_tracking(ui, body);
            break;
        case AppPage::Diagnostics:
            paint_diagnostics(ui, body);
            break;
        case AppPage::Settings:
        case AppPage::Count:
        default:
            paint_settings(ui, body);
            break;
    }
}

void paint_toast(Ui& ui, const RECT& client) {
    const AppState& s = ui.s;
    const bool visible = !s.toast.empty() && s.now_s < s.toast_expiry_s;
    const float v = ui.track(anim::kMisc, 1, visible ? 1.0f : 0.0f, 0.09f);
    if (v <= 0.01f || s.toast.empty()) {
        return;
    }
    const Theme& t = ui.t;
    const int text_w = std::min(text_width(ui.c.dc(), ui.f.body, s.toast), rw(client) - ui.px(160));
    const int width = text_w + ui.px(72);
    const int height = ui.px(46);
    const int side = sidebar_width(rw(client), ui.dpi);
    const int center = side + (rw(client) - side) / 2;
    const int bottom = client.bottom - ui.px(space::xl) +
                       static_cast<int>(std::lround((1.0f - v) * static_cast<float>(ui.px(16))));
    RECT pill = rect_of(center - width / 2, bottom - height, width, height);
    const COLORREF bg = t.dark ? rgb(48, 54, 67) : rgb(30, 35, 46);
    const COLORREF fg = rgb(240, 243, 248);
    ui.c.shadow(pill, height / 2, ui.px(4));
    ui.c.fill_round(pill, height / 2, bg, static_cast<int>(v * 255.0f));
    draw_icon(ui.c, rect_of(pill.left + ui.px(16), pill.top, ui.px(22), height), glyph::check,
              ui.f.icon_small, mix(bg, t.ok, v));
    ui.c.text(ui.f.body, mix(bg, fg, v), rect_of(pill.left + ui.px(46), pill.top, text_w + ui.px(8), height),
              s.toast, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void paint_help_bubble(Ui& ui, const RECT& client) {
    const AppState& s = ui.s;
    int id = s.hover_id;
    const int help_end = kUiHelpBase + 7 * 64;
    if (id < kUiHelpBase || id >= help_end) {
        id = s.focus_visible ? s.focus_id : kUiNone;
    }
    if (id < kUiHelpBase || id >= help_end) {
        return;
    }
    const Hotspot* anchor = nullptr;
    for (const Hotspot& spot : ui.hs) {
        if (spot.id == id) {
            anchor = &spot;
            break;
        }
    }
    if (anchor == nullptr || anchor->arg < 0 || anchor->arg >= static_cast<int>(HelpTopic::Count)) {
        return;
    }
    const wchar_t* text = help_text(static_cast<HelpTopic>(anchor->arg));
    if (text == nullptr || *text == L'\0') {
        return;
    }
    const int margin = ui.px(space::lg);
    const int max_w = std::min(ui.px(360), rw(client) - margin * 2);
    if (max_w < ui.px(160)) {
        return;
    }
    const int pad = ui.px(space::md);
    const int height = wrapped_text_height(ui.c.dc(), ui.f.small, max_w - pad * 2, text) + pad * 2;
    int x = anchor->rect.left - max_w / 2 + rw(anchor->rect) / 2;
    int y = anchor->rect.bottom + ui.px(space::sm);
    x = std::clamp(x, static_cast<int>(client.left) + margin, static_cast<int>(client.right) - margin - max_w);
    if (y + height > client.bottom - margin) {
        y = anchor->rect.top - height - ui.px(space::sm);
    }
    const RECT box{x, y, x + max_w, y + height};
    ui.c.shadow(box, ui.px(radius::md), ui.px(5));
    ui.c.fill_round(box, ui.px(radius::md), ui.t.dark ? rgb(44, 50, 62) : ui.t.surface);
    ui.c.outline_round(box, ui.px(radius::md), 1.0f, ui.t.border_strong);
    ui.c.text(ui.f.small, ui.t.text, RECT{box.left + pad, box.top + pad, box.right - pad, box.bottom - pad},
              text, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

}  // namespace

void paint_window(Canvas& canvas, const PaintContext& context, std::vector<Hotspot>& hotspots) {
    hotspots.clear();
    if (context.state == nullptr || context.fonts == nullptr) {
        return;
    }
    AppState& state = *context.state;
    state.anim.begin_frame();
    state.repaint_requested = false;
    const Theme& theme = canvas.theme();
    canvas.fill(context.client, theme.window);

    // The setup guide is modal: the shell still paints underneath, but its
    // hotspots go to a sink so only the guide is interactive.
    std::vector<Hotspot> shell_sink;
    const bool modal = state.onboarding.active;
    Ui shell{canvas, state, *context.fonts, theme, context.dpi, modal ? shell_sink : hotspots};
    const int side_w = sidebar_width(rw(context.client), context.dpi);
    RECT side{context.client.left, context.client.top, context.client.left + side_w,
              context.client.bottom};
    paint_sidebar(shell, side);
    RECT content{side.right, context.client.top, context.client.right, context.client.bottom};
    paint_content(shell, content);

    Ui top{canvas, state, *context.fonts, theme, context.dpi, hotspots};
    if (modal) {
        paint_wizard(top, context.client);
    } else {
        hide_scroll(top, kScrollWizard);
    }
    paint_toast(top, context.client);
    paint_help_bubble(top, context.client);
    state.anim.end_frame();
}

}  // namespace gt::ui
