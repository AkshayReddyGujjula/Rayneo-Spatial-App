#include "app_shell/ui_paint.h"

#include "app_shell/ui_help.h"
#include "app/engine_protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

namespace gt::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;

COLORREF mix_colors(COLORREF first, COLORREF second, int second_percent) {
    const int percent = std::clamp(second_percent, 0, 100);
    const int red = (GetRValue(first) * (100 - percent) + GetRValue(second) * percent) / 100;
    const int green = (GetGValue(first) * (100 - percent) + GetGValue(second) * percent) / 100;
    const int blue = (GetBValue(first) * (100 - percent) + GetBValue(second) * percent) / 100;
    return RGB(red, green, blue);
}

struct Metrics {
    int margin = 16;
    int gap = 14;
    int header = 58;
    int footer = 30;
    int left_width = 400;
    int pad = 14;
    int title = 24;
    int button = 32;
    int slider = 40;
    int row = 34;
};

Metrics metrics_for(UINT dpi, int client_width) {
    Metrics metrics;
    metrics.margin = scale_px(16, dpi);
    metrics.gap = scale_px(14, dpi);
    metrics.header = scale_px(58, dpi);
    metrics.footer = scale_px(30, dpi);
    metrics.left_width = std::clamp(client_width / 3, scale_px(340, dpi), scale_px(460, dpi));
    metrics.pad = scale_px(14, dpi);
    metrics.title = scale_px(24, dpi);
    metrics.button = scale_px(32, dpi);
    metrics.slider = scale_px(40, dpi);
    metrics.row = scale_px(34, dpi);
    return metrics;
}

RECT rect_of(int left, int top, int width, int height) {
    RECT rect{left, top, left + width, top + height};
    return rect;
}

void add_hotspot(std::vector<Hotspot>& hotspots, int id, int arg, const RECT& rect,
                 bool enabled = true, int panel = -1) {
    Hotspot spot;
    spot.id = id;
    spot.arg = arg;
    spot.rect = rect;
    spot.full = rect;
    spot.enabled = enabled;
    spot.panel = panel;
    hotspots.push_back(spot);
}

// Clipped variant for scrollable content: rect is already in view coordinates
// (content shifted by the offset). Fully hidden elements contribute nothing;
// partial ones stay interactive and keep the unclipped rect in full for drags.
void add_clipped_hotspot(std::vector<Hotspot>& hotspots, int id, int arg, const RECT& rect,
                         const RECT& view, int panel, bool enabled = true) {
    RECT intersection{};
    if (IntersectRect(&intersection, &rect, &view) == 0) {
        return;
    }
    Hotspot spot;
    spot.id = id;
    spot.arg = arg;
    spot.rect = intersection;
    spot.full = rect;
    spot.enabled = enabled;
    spot.panel = panel;
    hotspots.push_back(spot);
}

// Refreshes the portal cache, clamps the offset, draws the scrollbar when the
// content overflows, and returns the offset the panel must paint with. The
// scrollbar lives in a reserved gutter right of the view, so content never
// shifts when it appears.
int paint_scrollbar(Canvas& canvas, const PaintContext& context, ScrollPanel panel,
                    const RECT& panel_rect, const RECT& view_rect, int content_h,
                    bool follow_tail, std::vector<Hotspot>& hotspots) {
    AppState& state = *context.state;
    const UINT dpi = context.dpi;
    ScrollPortal& portal = state.scroll[panel];
    const int view_h = static_cast<int>(std::max(0L, view_rect.bottom - view_rect.top));
    const int max_offset = std::max(0, content_h - view_h);
    const int entry_offset = portal.offset;
    int offset = entry_offset;
    if (follow_tail && offset >= portal.last_max) {
        offset = max_offset;
    }
    offset = std::clamp(offset, 0, max_offset);
    if (offset != entry_offset) {
        // Paint-time correction (follow-tail, content shrink): the highlight
        // belongs to the cursor, not the moved content.
        state.hover_id = kUiNone;
    }
    portal.offset = offset;
    portal.panel = panel_rect;
    portal.view = view_rect;
    portal.content_h = content_h;
    portal.max_offset = max_offset;
    portal.last_max = max_offset;
    portal.visible = max_offset > 0 && view_h > 0;
    portal.scrollbar = RECT{};
    portal.thumb = RECT{};
    if (!portal.visible) {
        return offset;
    }
    const int bar_width = scale_px(10, dpi);
    RECT track{view_rect.right + scale_px(2, dpi), view_rect.top,
               view_rect.right + scale_px(2, dpi) + bar_width, view_rect.bottom};
    const int track_h = static_cast<int>(std::max(1L, track.bottom - track.top));
    const int natural_thumb = track_h * view_h / std::max(1, content_h);
    const int thumb_h =
        std::clamp(natural_thumb, std::min(track_h, scale_px(8, dpi)), track_h);
    const int travel = std::max(1, track_h - thumb_h);
    const int thumb_top = track.top + (max_offset > 0 ? offset * travel / max_offset : 0);
    RECT thumb{track.left, thumb_top, track.right,
                std::min(track.bottom, static_cast<LONG>(thumb_top) + thumb_h)};
    const int scroll_id = static_cast<int>(kUiScrollBase) + static_cast<int>(panel);
    draw_scrollbar(canvas, track, thumb, state.hover_id == scroll_id);
    portal.scrollbar = track;
    portal.thumb = thumb;
    add_hotspot(hotspots, scroll_id, static_cast<int>(panel), track, true,
                static_cast<int>(panel));
    return offset;
}

// Zeroes a portal when its panel has no room to paint.
void hide_portal(const PaintContext& context, ScrollPanel panel) {
    ScrollPortal& portal = context.state->scroll[panel];
    const int offset = portal.offset;
    const int last_max = portal.last_max;
    portal = ScrollPortal{};
    portal.offset = offset;
    portal.last_max = last_max;
}

// Splits available px among stacked panels: each gets its desired height when
// everything fits (surplus to flex_index), otherwise each shrinks toward its
// minimum in proportion to what it asked for. Never overlaps, never negative.
void distribute_heights(int available, int gap, const int desired[3], const int minimum[3],
                        int flex_index, int out[3]) {
    const int total_desired = desired[0] + desired[1] + desired[2] + gap * 2;
    if (available >= total_desired) {
        out[0] = desired[0];
        out[1] = desired[1];
        out[2] = desired[2];
        out[flex_index] += available - total_desired;
        return;
    }
    const int min_sum = minimum[0] + minimum[1] + minimum[2];
    if (available - gap * 2 <= min_sum) {
        const int each = std::max(0, (available - gap * 2) / 3);
        out[0] = out[1] = out[2] = each;
        return;
    }
    int weights = 0;
    for (int i = 0; i < 3; ++i) {
        weights += std::max(0, desired[i] - minimum[i]);
    }
    const int extra_total = available - gap * 2 - min_sum;
    for (int i = 0; i < 3; ++i) {
        const int slack = std::max(0, desired[i] - minimum[i]);
        out[i] = minimum[i] + (weights > 0 ? extra_total * slack / weights : 0);
    }
}

void draw_panel_frame(Canvas& canvas, const RECT& rect, const FontSet& fonts, UINT dpi,
                      const wchar_t* title) {
    canvas.fill_round(rect, scale_px(8, dpi), palette::panel);
    canvas.outline_round(rect, scale_px(8, dpi), 1, palette::border);
    if (title != nullptr) {
        RECT title_rect = rect;
        title_rect.left += scale_px(14, dpi);
        title_rect.top += scale_px(6, dpi);
        title_rect.right -= scale_px(14, dpi);
        title_rect.bottom = title_rect.top + scale_px(20, dpi);
        canvas.text(fonts.heading, palette::text, title_rect, title,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

std::wstring state_chip_text(const AppState& state) {
    switch (state.engine.snapshot().state) {
        case EngineProcessState::Starting:
            return L"Starting";
        case EngineProcessState::Running:
            return L"Running";
        case EngineProcessState::Stopping:
            return L"Stopping";
        case EngineProcessState::Exited:
            return L"Exited";
        case EngineProcessState::Failed:
            return L"Failed";
        case EngineProcessState::Stopped:
        default:
            return L"Stopped";
    }
}

COLORREF state_chip_color(const AppState& state) {
    switch (state.engine.snapshot().state) {
        case EngineProcessState::Running:
            return palette::ok;
        case EngineProcessState::Starting:
        case EngineProcessState::Stopping:
            return palette::warning;
        case EngineProcessState::Failed:
            return palette::error;
        case EngineProcessState::Exited:
            return palette::text_dim;
        case EngineProcessState::Stopped:
        default:
            return palette::text_faint;
    }
}

std::wstring mode_chip_text(const AppState& state) {
    if (state.engine.busy()) {
        if (state.engine.snapshot().mode == LaunchMode::Workspace) {
            return L"Full workspace";
        }
        if (state.engine.snapshot().mode == LaunchMode::Preview) {
            return L"Renderer-only preview";
        }
        return L"Engine running";
    }
    switch (state.config.last_mode) {
        case LaunchMode::Workspace:
            return L"Last: full workspace";
        case LaunchMode::Preview:
            return L"Last: preview";
        case LaunchMode::None:
        default:
            return L"No engine running";
    }
}

void paint_header(Canvas& canvas, const PaintContext& context, const Metrics& metrics,
                  std::vector<Hotspot>& hotspots) {
    const AppState& state = *context.state;
    const FontSet& fonts = *context.fonts;
    const UINT dpi = context.dpi;
    RECT header{context.client.left, context.client.top, context.client.right,
                context.client.top + metrics.header};
    canvas.fill(header, palette::background);

    RECT title_rect = header;
    title_rect.left += metrics.margin;
    title_rect.right = title_rect.left + scale_px(360, dpi);
    title_rect.top += scale_px(8, dpi);
    title_rect.bottom = title_rect.top + scale_px(24, dpi);
    canvas.text(fonts.title, palette::text, title_rect, L"RayNeo Spatial",
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT subtitle_rect = title_rect;
    subtitle_rect.top = title_rect.bottom;
    subtitle_rect.bottom = subtitle_rect.top + scale_px(18, dpi);
    canvas.text(fonts.small, palette::text_faint, subtitle_rect,
                L"controller for the RayNeo GT spatial workspace engine",
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int chip_height = scale_px(26, dpi);
    const int refresh_width = scale_px(86, dpi);
    RECT refresh = rect_of(header.right - metrics.margin - refresh_width,
                           header.top + (metrics.header - chip_height) / 2, refresh_width,
                           chip_height);
    draw_button(canvas, refresh, L"Refresh", ButtonStyle::Secondary,
                state.focus_id == kUiRefresh, state.hover_id == kUiRefresh, true, fonts.body);
    add_hotspot(hotspots, kUiRefresh, 0, refresh);
    RECT help_refresh = rect_of(refresh.left - metrics.gap - scale_px(24, dpi), refresh.top,
                                scale_px(24, dpi), chip_height);
    draw_chip(canvas, help_refresh, L"?", palette::text_faint,
              state.focus_id == ui_help_id(6, 0), state.hover_id == ui_help_id(6, 0), fonts.body);
    add_hotspot(hotspots, ui_help_id(6, 0), static_cast<int>(HelpTopic::HeaderRefresh),
                help_refresh);

    const int mode_width = scale_px(190, dpi);
    RECT mode_chip = rect_of(refresh.left - metrics.gap - mode_width, refresh.top, mode_width,
                             chip_height);
    const COLORREF mode_color = state.engine.busy() ? palette::accent : palette::text_faint;
    canvas.fill_round(mode_chip, chip_height / 2, palette::panel_alt);
    canvas.outline_round(mode_chip, chip_height / 2, 1, palette::border);
    draw_dot(canvas, mode_chip.left + scale_px(12, dpi),
             (mode_chip.top + mode_chip.bottom) / 2, scale_px(4, dpi), mode_color);
    RECT mode_text = mode_chip;
    mode_text.left += scale_px(22, dpi);
    canvas.text(fonts.small, palette::text_dim, mode_text, mode_chip_text(state),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int state_width = scale_px(120, dpi);
    RECT state_chip = rect_of(mode_chip.left - metrics.gap - state_width, refresh.top, state_width,
                              chip_height);
    const COLORREF chip_color = state_chip_color(state);
    canvas.fill_round(state_chip, chip_height / 2, palette::panel_alt);
    canvas.outline_round(state_chip, chip_height / 2, 1, palette::border);
    draw_dot(canvas, state_chip.left + scale_px(12, dpi),
             (state_chip.top + state_chip.bottom) / 2, scale_px(4, dpi), chip_color);
    RECT state_text = state_chip;
    state_text.left += scale_px(22, dpi);
    canvas.text(fonts.small, palette::text, state_text, state_chip_text(state),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT separator{header.left + metrics.margin, header.bottom - 1,
                   header.right - metrics.margin, header.bottom};
    canvas.separator(separator, palette::border);
}

void paint_banner(Canvas& canvas, const PaintContext& context, const Metrics& metrics, int& y) {
    const AppState& state = *context.state;
    if (state.banner.empty()) {
        return;
    }
    const FontSet& fonts = *context.fonts;
    const UINT dpi = context.dpi;
    const int height = scale_px(38, dpi);
    RECT banner{context.client.left + metrics.margin, y, context.client.right - metrics.margin,
                y + height};
    const COLORREF accent = severity_color(state.banner_severity);
    canvas.fill_round(banner, scale_px(6, dpi), mix_colors(palette::panel_alt, accent, 18));
    canvas.outline_round(banner, scale_px(6, dpi), 1, mix_colors(palette::border, accent, 60));
    RECT text_rect = banner;
    text_rect.left += scale_px(12, dpi);
    text_rect.right -= scale_px(12, dpi);
    canvas.text(fonts.body, palette::text, text_rect, ellipsize(state.banner, 220),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += height + scale_px(10, dpi);
}

void paint_status_panel(Canvas& canvas, const PaintContext& context, const Metrics& metrics,
                        const RECT& rect, std::vector<Hotspot>& hotspots) {
    const AppState& state = *context.state;
    const FontSet& fonts = *context.fonts;
    const UINT dpi = context.dpi;
    draw_panel_frame(canvas, rect, fonts, dpi, L"Status");

    const int label_width = scale_px(150, dpi);
    const int gutter = scale_px(12, dpi);
    RECT view{rect.left + metrics.pad, rect.top + metrics.pad + scale_px(20, dpi),
              rect.right - metrics.pad - gutter, rect.bottom - scale_px(6, dpi)};
    const int text_left = view.left;
    const int value_left = text_left + label_width;

    struct Row {
        std::wstring label;
        std::wstring value;
        std::wstring detail;
        COLORREF dot;
    };
    std::vector<Row> rows;
    const DiagnosticsReport& report = state.diagnostics;

    const EngineSnapshot& engine = state.engine.snapshot();
    {
        std::wstring value = engine_process_state_label(engine.state);
        if (engine.process_alive) {
            value += L"  pid " + std::to_wstring(engine.process_id);
        }
        std::wstring detail;
        if (!engine.last_error.empty()) {
            detail = wide_from_utf8(engine.last_error);
        } else if (state.engine_status.parsed) {
            detail = wide_from_utf8(engine_status_summary(state.engine_status));
        } else if (state.engine.busy()) {
            detail = L"engine window found, waiting for its status file";
        } else {
            detail = L"no engine running; choose Start workspace or Start preview";
        }
        rows.push_back({L"Engine", value, detail, state_chip_color(state)});
    }
    rows.push_back({L"RayNeo display",
                    report.glasses_found ? L"present" : L"not found",
                    report.glasses_detail,
                    report.glasses_found ? palette::ok : palette::error});
    rows.push_back({L"Orientation calibration",
                    report.calibration_valid ? L"valid" : L"not usable",
                    report.calibration_detail,
                    report.calibration_valid ? palette::ok : palette::warning});
    rows.push_back({L"RayNeo HID",
                    report.hid_device_count > 0 ? L"found" : L"not found",
                    report.hid_detail,
                    report.hid_device_count > 0 ? palette::ok : palette::error});
    rows.push_back({L"Parsec VDD",
                    report.vdd_ready ? L"ready" : L"not ready",
                    report.vdd_status_text,
                    report.vdd_ready ? palette::ok : palette::warning});
    rows.push_back({L"Layout file",
                    report.layout_ok ? L"valid" : L"problem",
                    report.layout_detail,
                    report.layout_ok ? palette::ok : palette::error});
    {
        std::wstring value = report.telemetry_present ? L"latest row" : L"no data";
        rows.push_back({L"Telemetry CSV", value, report.telemetry_detail,
                        report.telemetry_present ? palette::ok : palette::text_faint});
    }
    {
        std::error_code size_error;
        const uint64_t bytes = std::filesystem::file_size(state.paths.engine_log, size_error);
        std::wstring value = size_error ? L"not written yet" : format_bytes_wide(bytes);
        std::wstring detail = state.paths.engine_log.wstring();
        rows.push_back({L"Engine log", value, detail, palette::text_faint});
    }

    const int content_h = static_cast<int>(rows.size()) * metrics.row;
    const int offset =
        paint_scrollbar(canvas, context, kScrollStatus, rect, view, content_h, false, hotspots);
    canvas.push_clip(view);
    int y = view.top - offset;
    const int help_w = scale_px(24, dpi);
    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        const Row& row = rows[row_index];
        const int dot_x = text_left + scale_px(5, dpi);
        draw_dot(canvas, dot_x, y + scale_px(10, dpi), scale_px(4, dpi), row.dot);
        RECT label_rect = rect_of(text_left + scale_px(16, dpi), y, label_width, scale_px(20, dpi));
        canvas.text(fonts.small, palette::text_dim, label_rect, row.label,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT value_rect = rect_of(value_left, y, view.right - value_left - help_w - metrics.gap,
                                  scale_px(20, dpi));
        canvas.text(fonts.body, palette::text, value_rect, row.value,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT help_rect = rect_of(view.right - help_w, y, help_w, scale_px(20, dpi));
        const int help_id = ui_help_id(0, static_cast<int>(row_index));
        const int help_topic =
            static_cast<int>(HelpTopic::StatusEngine) + static_cast<int>(row_index);
        draw_chip(canvas, help_rect, L"?", palette::text_faint, state.focus_id == help_id,
                  state.hover_id == help_id, fonts.small);
        add_clipped_hotspot(hotspots, help_id, help_topic, help_rect, view, kScrollStatus);
        RECT detail_rect = rect_of(text_left + scale_px(16, dpi), y + scale_px(19, dpi),
                                   view.right - text_left - scale_px(16, dpi), scale_px(15, dpi));
        canvas.text(fonts.small, palette::text_faint, detail_rect, ellipsize(row.detail, 84),
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += metrics.row;
    }
    canvas.pop_clip();
}

void paint_engine_panel(Canvas& canvas, const PaintContext& context, const Metrics& metrics,
                        const RECT& rect, std::vector<Hotspot>& hotspots) {
    const AppState& state = *context.state;
    const FontSet& fonts = *context.fonts;
    const UINT dpi = context.dpi;
    draw_panel_frame(canvas, rect, fonts, dpi, L"Engine control");

    const int gutter = scale_px(12, dpi);
    const int left = rect.left + metrics.pad;
    const int width = rect.right - rect.left - metrics.pad * 2 - gutter;
    RECT view{left, rect.top + metrics.pad + scale_px(22, dpi), left + width,
              rect.bottom - metrics.pad};
    const bool busy = state.engine.busy();
    const std::wstring workspace_block = start_block_reason(state, LaunchMode::Workspace);
    const std::wstring preview_block = start_block_reason(state, LaunchMode::Preview);

    std::wstring note;
    COLORREF note_color = palette::text_faint;
    if (busy) {
        note = L"Engine runs in its own process; this window stays responsive. "
               L"Stop asks it to quit gracefully (virtual desktops are removed). "
               L"Ctrl+Shift+\\ exits the engine.";
    } else if (!workspace_block.empty()) {
        note = L"Start workspace blocked: " + workspace_block;
        note_color = palette::warning;
    } else if (!preview_block.empty()) {
        note = L"Start preview blocked: " + preview_block;
        note_color = palette::warning;
    } else {
        note = L"Start workspace creates real Parsec virtual desktops and switches the "
               L"laptop panel off until exit; Start preview renders labelled test screens only.";
    }
    const std::wstring note_text = ellipsize(note, 150);
    const int row_gap = scale_px(8, dpi);
    const int help_w = scale_px(24, dpi);
    const int controls_h =
        (metrics.button + row_gap) * 2 + scale_px(28, dpi) + scale_px(30, dpi);
    const int note_h =
        wrapped_text_height(canvas.dc(), fonts.small, width, note_text) + scale_px(6, dpi);
    const int offset = paint_scrollbar(canvas, context, kScrollEngine, rect, view,
                                       controls_h + note_h, false, hotspots);
    canvas.push_clip(view);
    int y = view.top - offset;

    const int half = (width - metrics.gap) / 2;
    RECT start_workspace = rect_of(left, y, half - help_w - metrics.gap, metrics.button);
    draw_button(canvas, start_workspace, L"Start workspace", ButtonStyle::Primary,
                state.focus_id == kUiStartWorkspace,
                state.hover_id == kUiStartWorkspace && !busy && workspace_block.empty(),
                !busy && workspace_block.empty(), fonts.body);
    add_clipped_hotspot(hotspots, kUiStartWorkspace, 0, start_workspace, view, kScrollEngine,
                        !busy && workspace_block.empty());
    RECT help_workspace =
        rect_of(start_workspace.right + metrics.gap, y, help_w, metrics.button);
    draw_chip(canvas, help_workspace, L"?", palette::text_faint,
              state.focus_id == ui_help_id(1, 0), state.hover_id == ui_help_id(1, 0),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(1, 0),
                        static_cast<int>(HelpTopic::EngineStartWorkspace), help_workspace, view,
                        kScrollEngine);

    RECT start_preview =
        rect_of(left + half + metrics.gap, y, half - help_w - metrics.gap, metrics.button);
    draw_button(canvas, start_preview, L"Start preview", ButtonStyle::Secondary,
                state.focus_id == kUiStartPreview,
                state.hover_id == kUiStartPreview && !busy && preview_block.empty(),
                !busy && preview_block.empty(), fonts.body);
    add_clipped_hotspot(hotspots, kUiStartPreview, 0, start_preview, view, kScrollEngine,
                        !busy && preview_block.empty());
    RECT help_preview = rect_of(start_preview.right + metrics.gap, y, help_w, metrics.button);
    draw_chip(canvas, help_preview, L"?", palette::text_faint,
              state.focus_id == ui_help_id(1, 1), state.hover_id == ui_help_id(1, 1),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(1, 1),
                        static_cast<int>(HelpTopic::EngineStartPreview), help_preview, view,
                        kScrollEngine);
    y += metrics.button + scale_px(8, dpi);

    const int third = (width - metrics.gap * 2) / 3;
    RECT stop = rect_of(left, y, third - help_w - metrics.gap, metrics.button);
    draw_button(canvas, stop, L"Stop", ButtonStyle::Danger, state.focus_id == kUiStopEngine,
                state.hover_id == kUiStopEngine, busy, fonts.body);
    add_clipped_hotspot(hotspots, kUiStopEngine, 0, stop, view, kScrollEngine, busy);
    RECT help_stop = rect_of(stop.right + metrics.gap, y, help_w, metrics.button);
    draw_chip(canvas, help_stop, L"?", palette::text_faint,
              state.focus_id == ui_help_id(1, 2), state.hover_id == ui_help_id(1, 2),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(1, 2), static_cast<int>(HelpTopic::EngineStop),
                        help_stop, view, kScrollEngine);

    RECT recenter =
        rect_of(left + third + metrics.gap, y, third - help_w - metrics.gap, metrics.button);
    draw_button(canvas, recenter, L"Recenter", ButtonStyle::Secondary,
                state.focus_id == kUiRecenter, state.hover_id == kUiRecenter, busy, fonts.body);
    add_clipped_hotspot(hotspots, kUiRecenter, 0, recenter, view, kScrollEngine, busy);
    RECT help_recenter = rect_of(recenter.right + metrics.gap, y, help_w, metrics.button);
    draw_chip(canvas, help_recenter, L"?", palette::text_faint,
              state.focus_id == ui_help_id(1, 3), state.hover_id == ui_help_id(1, 3),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(1, 3), static_cast<int>(HelpTopic::EngineRecenter),
                        help_recenter, view, kScrollEngine);

    RECT reload = rect_of(left + (third + metrics.gap) * 2, y, third - help_w - metrics.gap,
                          metrics.button);
    draw_button(canvas, reload, L"Reload layout", ButtonStyle::Ghost,
                state.focus_id == kUiReloadLayout, state.hover_id == kUiReloadLayout, busy,
                fonts.body);
    add_clipped_hotspot(hotspots, kUiReloadLayout, 0, reload, view, kScrollEngine, busy);
    RECT help_reload = rect_of(reload.right + metrics.gap, y, help_w, metrics.button);
    draw_chip(canvas, help_reload, L"?", palette::text_faint,
              state.focus_id == ui_help_id(1, 4), state.hover_id == ui_help_id(1, 4),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(1, 4),
                        static_cast<int>(HelpTopic::EngineReloadLayout), help_reload, view,
                        kScrollEngine);
    y += metrics.button + scale_px(8, dpi);

    const int toggle_width = (width - metrics.gap) / 2;
    RECT yaw_toggle = rect_of(left, y, toggle_width - help_w - metrics.gap, scale_px(24, dpi));
    const bool yaw_on = state.engine.snapshot().query_answered
                            ? (state.engine.snapshot().query_flags & kEngineFlagYawTracking) != 0
                            : true;
    draw_toggle(canvas, yaw_toggle, L"Yaw tracking (Ctrl+Alt+Y)", yaw_on,
                state.focus_id == kUiToggleYaw, state.hover_id == kUiToggleYaw, fonts.small);
    add_clipped_hotspot(hotspots, kUiToggleYaw, 0, yaw_toggle, view, kScrollEngine, busy);
    RECT help_yaw = rect_of(yaw_toggle.right + metrics.gap, y, help_w, scale_px(24, dpi));
    draw_chip(canvas, help_yaw, L"?", palette::text_faint,
              state.focus_id == ui_help_id(1, 5), state.hover_id == ui_help_id(1, 5),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(1, 5), static_cast<int>(HelpTopic::EngineYawToggle),
                        help_yaw, view, kScrollEngine);

    RECT pitch_toggle = rect_of(left + toggle_width + metrics.gap, y,
                                  toggle_width - help_w - metrics.gap, scale_px(24, dpi));
    const bool pitch_on =
        state.engine.snapshot().query_answered
            ? (state.engine.snapshot().query_flags & kEngineFlagPitchTracking) != 0
            : true;
    draw_toggle(canvas, pitch_toggle, L"Pitch tracking (Ctrl+Alt+P)", pitch_on,
                state.focus_id == kUiTogglePitch, state.hover_id == kUiTogglePitch, fonts.small);
    add_clipped_hotspot(hotspots, kUiTogglePitch, 0, pitch_toggle, view, kScrollEngine, busy);
    RECT help_pitch = rect_of(pitch_toggle.right + metrics.gap, y, help_w, scale_px(24, dpi));
    draw_chip(canvas, help_pitch, L"?", palette::text_faint,
              state.focus_id == ui_help_id(1, 6), state.hover_id == ui_help_id(1, 6),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(1, 6),
                        static_cast<int>(HelpTopic::EnginePitchToggle), help_pitch, view,
                        kScrollEngine);
    y += scale_px(28, dpi);

    RECT no_imu = rect_of(left, y, width - help_w - metrics.gap, scale_px(24, dpi));
    draw_toggle(canvas, no_imu, L"Preview without head tracking (--no-imu)",
                state.config.preview_without_head_tracking,
                state.focus_id == kUiPreviewWithoutHeadTracking,
                state.hover_id == kUiPreviewWithoutHeadTracking, fonts.small);
    add_clipped_hotspot(hotspots, kUiPreviewWithoutHeadTracking, 0, no_imu, view, kScrollEngine,
                        !busy);
    RECT help_no_imu = rect_of(no_imu.right + metrics.gap, y, help_w, scale_px(24, dpi));
    draw_chip(canvas, help_no_imu, L"?", palette::text_faint,
              state.focus_id == ui_help_id(1, 7), state.hover_id == ui_help_id(1, 7),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(1, 7), static_cast<int>(HelpTopic::EngineNoImu),
                        help_no_imu, view, kScrollEngine);
    y += scale_px(30, dpi);

    RECT note_rect = rect_of(left, y, width, note_h);
    canvas.text(fonts.small, note_color, note_rect, note_text,
                DT_LEFT | DT_TOP | DT_WORDBREAK);
    canvas.pop_clip();
}

void paint_diagnostics_panel(Canvas& canvas, const PaintContext& context, const Metrics& metrics,
                             const RECT& rect, std::vector<Hotspot>& hotspots) {
    const AppState& state = *context.state;
    const FontSet& fonts = *context.fonts;
    const UINT dpi = context.dpi;
    draw_panel_frame(canvas, rect, fonts, dpi, L"Diagnostics and recovery");

    const int gutter = scale_px(12, dpi);
    const int left = rect.left + metrics.pad;
    const int width = rect.right - rect.left - metrics.pad * 2 - gutter;
    RECT view{left, rect.top + metrics.pad + scale_px(22, dpi), left + width,
              rect.bottom - metrics.pad};
    const int half = (width - metrics.gap) / 2;
    const int small_button = scale_px(30, dpi);
    const int help_w = scale_px(24, dpi);
    const int line_h = scale_px(15, dpi);
    const int buttons_h = (small_button + scale_px(8, dpi)) * 3;
    const int events_h =
        static_cast<int>(std::max<size_t>(state.events.size(), 1)) * line_h + scale_px(4, dpi);
    const int offset = paint_scrollbar(canvas, context, kScrollDiagnostics, rect, view,
                                       buttons_h + events_h, true, hotspots);
    canvas.push_clip(view);
    int y = view.top - offset;

    RECT calibration = rect_of(left, y, half - help_w - metrics.gap, small_button);
    draw_button(canvas, calibration, L"Run calibration", ButtonStyle::Secondary,
                state.focus_id == kUiLaunchCalibration, state.hover_id == kUiLaunchCalibration,
                state.paths.calibration_tool_found, fonts.small);
    add_clipped_hotspot(hotspots, kUiLaunchCalibration, 0, calibration, view, kScrollDiagnostics,
                        state.paths.calibration_tool_found);
    RECT help_cal = rect_of(calibration.right + metrics.gap, y, help_w, small_button);
    draw_chip(canvas, help_cal, L"?", palette::text_faint,
              state.focus_id == ui_help_id(2, 0), state.hover_id == ui_help_id(2, 0),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(2, 0), static_cast<int>(HelpTopic::DiagCalibration),
                        help_cal, view, kScrollDiagnostics);

    RECT engine_log =
        rect_of(left + half + metrics.gap, y, half - help_w - metrics.gap, small_button);
    draw_button(canvas, engine_log, L"Open engine log", ButtonStyle::Ghost,
                state.focus_id == kUiOpenEngineLog, state.hover_id == kUiOpenEngineLog, true,
                fonts.small);
    add_clipped_hotspot(hotspots, kUiOpenEngineLog, 0, engine_log, view, kScrollDiagnostics);
    RECT help_log = rect_of(engine_log.right + metrics.gap, y, help_w, small_button);
    draw_chip(canvas, help_log, L"?", palette::text_faint,
              state.focus_id == ui_help_id(2, 1), state.hover_id == ui_help_id(2, 1),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(2, 1), static_cast<int>(HelpTopic::DiagEngineLog),
                        help_log, view, kScrollDiagnostics);
    y += small_button + scale_px(8, dpi);

    RECT logs_folder = rect_of(left, y, half - help_w - metrics.gap, small_button);
    draw_button(canvas, logs_folder, L"Open logs folder", ButtonStyle::Ghost,
                state.focus_id == kUiOpenLogs, state.hover_id == kUiOpenLogs, true, fonts.small);
    add_clipped_hotspot(hotspots, kUiOpenLogs, 0, logs_folder, view, kScrollDiagnostics);
    RECT help_folder = rect_of(logs_folder.right + metrics.gap, y, help_w, small_button);
    draw_chip(canvas, help_folder, L"?", palette::text_faint,
              state.focus_id == ui_help_id(2, 2), state.hover_id == ui_help_id(2, 2),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(2, 2), static_cast<int>(HelpTopic::DiagLogsFolder),
                        help_folder, view, kScrollDiagnostics);

    RECT tray_toggle =
        rect_of(left + half + metrics.gap, y, half - help_w - metrics.gap, small_button);
    draw_toggle(canvas, tray_toggle, L"Close to tray", state.config.close_to_tray,
                state.focus_id == kUiCloseToTray, state.hover_id == kUiCloseToTray, fonts.small);
    add_clipped_hotspot(hotspots, kUiCloseToTray, 0, tray_toggle, view, kScrollDiagnostics);
    RECT help_tray = rect_of(tray_toggle.right + metrics.gap, y, help_w, small_button);
    draw_chip(canvas, help_tray, L"?", palette::text_faint,
              state.focus_id == ui_help_id(2, 3), state.hover_id == ui_help_id(2, 3),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(2, 3), static_cast<int>(HelpTopic::DiagCloseToTray),
                        help_tray, view, kScrollDiagnostics);
    y += small_button + scale_px(8, dpi);

    RECT recover = rect_of(left, y, half - help_w - metrics.gap, small_button);
    draw_button(canvas, recover, L"Recover displays", ButtonStyle::Ghost,
                state.focus_id == kUiRecoverDisplays, state.hover_id == kUiRecoverDisplays, true,
                fonts.small);
    add_clipped_hotspot(hotspots, kUiRecoverDisplays, 0, recover, view, kScrollDiagnostics);
    RECT help_recover = rect_of(recover.right + metrics.gap, y, help_w, small_button);
    draw_chip(canvas, help_recover, L"?", palette::text_faint,
              state.focus_id == ui_help_id(2, 4), state.hover_id == ui_help_id(2, 4),
              fonts.small);
    add_clipped_hotspot(hotspots, ui_help_id(2, 4),
                        static_cast<int>(HelpTopic::DiagRecoverDisplays), help_recover, view,
                        kScrollDiagnostics);
    y += small_button + scale_px(8, dpi);

    if (state.events.empty()) {
        RECT line = rect_of(left, y, width, line_h);
        canvas.text(fonts.small, palette::text_faint, line, L"no controller events yet",
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    for (size_t index = 0; index < state.events.size(); ++index) {
        RECT line = rect_of(left, y, width, line_h);
        canvas.text(fonts.small, palette::text_faint, line, ellipsize(state.events[index], 96),
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += line_h;
    }
    canvas.pop_clip();
}

void paint_layout_panel(Canvas& canvas, const PaintContext& context, const Metrics& metrics,
                        const RECT& rect, std::vector<Hotspot>& hotspots) {
    const AppState& state = *context.state;
    const FontSet& fonts = *context.fonts;
    const UINT dpi = context.dpi;
    draw_panel_frame(canvas, rect, fonts, dpi, L"Layout");

    const int gutter = scale_px(12, dpi);
    const int left = rect.left + metrics.pad;
    const int width = rect.right - rect.left - metrics.pad * 2 - gutter;
    RECT view{left, rect.top + metrics.pad + scale_px(22, dpi), left + width,
              rect.bottom - metrics.pad};
    const int chip_height = scale_px(26, dpi);
    const int chip_gap = scale_px(6, dpi);

    // Pass 1: flow the chips and buttons into content rows (wrapping instead
    // of dropping overflow), so the content height is known before painting.
    struct FlowItem {
        RECT rc{};
        int id = 0;
        int arg = 0;
        int kind = 0;  // 0 = preset, 1 = screen, 2 = button, 3 = user preset, 6 = help, 7 = hint
    };
    std::vector<FlowItem> items;
    int content_y = 0;
    int flow_x = 0;
    int flow_row_h = 0;
    const auto place = [&](int item_width, int item_height, int id, int arg, int kind) {
        if (flow_x > 0 && flow_x + item_width > width) {
            flow_x = 0;
            content_y += flow_row_h + chip_gap;
            flow_row_h = 0;
        }
        FlowItem item;
        item.rc = rect_of(left + flow_x, content_y, item_width, item_height);
        item.id = id;
        item.arg = arg;
        item.kind = kind;
        items.push_back(item);
        flow_x += item_width + chip_gap;
        flow_row_h = std::max(flow_row_h, item_height);
    };
    const auto end_block = [&](int block_gap) {
        if (flow_row_h > 0) {
            content_y += flow_row_h + block_gap;
        }
        flow_x = 0;
        flow_row_h = 0;
    };
    for (const LayoutPreset preset : layout_presets()) {
        const std::wstring label = wide_from_utf8(layout_preset_name(preset));
        place(scale_px(static_cast<int>(label.size()) * 7 + 22, dpi), chip_height,
              ui_preset_id(preset), 0, 0);
    }
    place(scale_px(24, dpi), chip_height, ui_help_id(3, 0),
          static_cast<int>(HelpTopic::LayoutFactory), 6);
    end_block(scale_px(8, dpi));
    if (state.preset_names.empty()) {
        place(width, scale_px(16, dpi), 0, 0, 7);
        end_block(scale_px(2, dpi));
    }
    for (size_t index = 0; index < state.preset_names.size() && index < 32; ++index) {
        std::wstring label = wide_from_utf8(state.preset_names[index]);
        if (state.preset_names[index] == state.loaded_preset) {
            label += L" *";
        }
        place(scale_px(static_cast<int>(label.size()) * 7 + 22, dpi), chip_height,
              ui_user_preset_id(index), static_cast<int>(index), 3);
    }
    place(scale_px(24, dpi), chip_height, ui_help_id(3, 1),
          static_cast<int>(HelpTopic::LayoutUserPresets), 6);
    end_block(scale_px(8, dpi));
    for (size_t index = 0; index < state.layout.screens.size(); ++index) {
        place(scale_px(96, dpi), chip_height, ui_screen_id(index), static_cast<int>(index), 1);
    }
    end_block(scale_px(8, dpi));
    const int button_h = scale_px(30, dpi);
    place(scale_px(96, dpi), button_h, kUiAddScreen, 0, 2);
    place(scale_px(24, dpi), button_h, ui_help_id(3, 2), static_cast<int>(HelpTopic::LayoutAddScreen), 6);
    place(scale_px(96, dpi), button_h, kUiRemoveScreen, 0, 2);
    place(scale_px(24, dpi), button_h, ui_help_id(3, 3), static_cast<int>(HelpTopic::LayoutRemoveScreen), 6);
    place(scale_px(126, dpi), button_h, kUiSaveLayout, 0, 2);
    place(scale_px(24, dpi), button_h, ui_help_id(3, 4), static_cast<int>(HelpTopic::LayoutSave), 6);
    place(scale_px(96, dpi), button_h, kUiRevertLayout, 0, 2);
    place(scale_px(24, dpi), button_h, ui_help_id(3, 5), static_cast<int>(HelpTopic::LayoutRevert), 6);
    place(scale_px(150, dpi), button_h, kUiPresetSave, 0, 2);
    place(scale_px(24, dpi), button_h, ui_help_id(3, 6), static_cast<int>(HelpTopic::LayoutPresetSave), 6);
    place(scale_px(130, dpi), button_h, kUiPresetDelete, 0, 2);
    place(scale_px(24, dpi), button_h, ui_help_id(3, 7), static_cast<int>(HelpTopic::LayoutPresetDelete), 6);
    end_block(scale_px(6, dpi));
    const int summary_h = scale_px(16, dpi);

    const int offset = paint_scrollbar(canvas, context, kScrollLayout, rect, view,
                                       content_y + summary_h, false, hotspots);
    canvas.push_clip(view);
    const bool can_add = state.layout.screens.size() < 8;
    const bool can_remove = state.layout.screens.size() > 1;
    for (const FlowItem& item : items) {
        RECT rc = item.rc;
        rc.top += view.top - offset;
        rc.bottom += view.top - offset;
        if (item.kind == 0) {
            const LayoutPreset preset = static_cast<LayoutPreset>(item.id - kUiPresetBase);
            draw_chip(canvas, rc, wide_from_utf8(layout_preset_name(preset)), palette::accent,
                      state.focus_id == item.id, state.hover_id == item.id, fonts.small);
            add_clipped_hotspot(hotspots, item.id, 0, rc, view, kScrollLayout);
        } else if (item.kind == 3) {
            const std::string& name = state.preset_names[static_cast<size_t>(item.arg)];
            std::wstring label = wide_from_utf8(name);
            if (name == state.loaded_preset) {
                label += L" *";
            }
            draw_chip(canvas, rc, label, palette::accent, state.focus_id == item.id,
                      state.hover_id == item.id, fonts.small);
            add_clipped_hotspot(hotspots, item.id, item.arg, rc, view, kScrollLayout);
        } else if (item.kind == 6) {
            draw_chip(canvas, rc, L"?", palette::text_faint, state.focus_id == item.id,
                      state.hover_id == item.id, fonts.small);
            add_clipped_hotspot(hotspots, item.id, item.arg, rc, view, kScrollLayout);
        } else if (item.kind == 7) {
            canvas.text(fonts.small, palette::text_faint, rc,
                        L"No saved presets yet -- arrange screens, then Save as preset.",
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (item.kind == 1) {
            const ScreenLayout& screen = state.layout.screens[static_cast<size_t>(item.arg)];
            const bool selected = static_cast<size_t>(item.arg) == state.selected_screen;
            const COLORREF tint = RGB(static_cast<int>(screen.color[0] * 255.0f),
                                      static_cast<int>(screen.color[1] * 255.0f),
                                      static_cast<int>(screen.color[2] * 255.0f));
            canvas.fill_round(rc, chip_height / 2,
                              selected ? palette::accent_soft : palette::panel_alt);
            canvas.outline_round(rc, chip_height / 2, selected ? 2 : 1,
                                 selected ? palette::accent : palette::border);
            draw_dot(canvas, rc.left + scale_px(11, dpi), (rc.top + rc.bottom) / 2,
                     scale_px(4, dpi), tint);
            RECT chip_text = rc;
            chip_text.left += scale_px(20, dpi);
            chip_text.right -= scale_px(8, dpi);
            canvas.text(fonts.small, selected ? palette::text : palette::text_dim, chip_text,
                        ellipsize(wide_from_utf8(screen.id), 12),
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            add_clipped_hotspot(hotspots, item.id, item.arg, rc, view, kScrollLayout);
        } else if (item.id == kUiAddScreen) {
            draw_button(canvas, rc, L"Add screen", ButtonStyle::Secondary,
                        state.focus_id == item.id, state.hover_id == item.id, can_add, fonts.small);
            add_clipped_hotspot(hotspots, item.id, 0, rc, view, kScrollLayout, can_add);
        } else if (item.id == kUiRemoveScreen) {
            draw_button(canvas, rc, L"Remove", ButtonStyle::Danger, state.focus_id == item.id,
                        state.hover_id == item.id, can_remove, fonts.small);
            add_clipped_hotspot(hotspots, item.id, 0, rc, view, kScrollLayout, can_remove);
        } else if (item.id == kUiSaveLayout) {
            draw_button(canvas, rc, L"Save and reload", ButtonStyle::Primary,
                        state.focus_id == item.id, state.hover_id == item.id, true, fonts.small);
            add_clipped_hotspot(hotspots, item.id, 0, rc, view, kScrollLayout);
        } else if (item.id == kUiPresetSave) {
            draw_button(canvas, rc, L"Save as preset", ButtonStyle::Secondary,
                        state.focus_id == item.id, state.hover_id == item.id, true, fonts.small);
            add_clipped_hotspot(hotspots, item.id, 0, rc, view, kScrollLayout);
        } else if (item.id == kUiPresetDelete) {
            const bool can_delete = !state.loaded_preset.empty();
            draw_button(canvas, rc, L"Delete preset", ButtonStyle::Danger,
                        state.focus_id == item.id, state.hover_id == item.id, can_delete,
                        fonts.small);
            add_clipped_hotspot(hotspots, item.id, 0, rc, view, kScrollLayout, can_delete);
        } else {
            draw_button(canvas, rc, L"Revert", ButtonStyle::Ghost, state.focus_id == item.id,
                        state.hover_id == item.id, true, fonts.small);
            add_clipped_hotspot(hotspots, item.id, 0, rc, view, kScrollLayout);
        }
    }

    RECT summary = rect_of(left, content_y + view.top - offset, width, summary_h);
    std::wstring summary_text = wide_from_utf8(layout_summary(state.layout));
    if (state.layout_dirty) {
        summary_text += L"  |  unsaved changes";
    }
    canvas.text(fonts.small, state.layout_dirty ? palette::warning : palette::text_faint, summary,
                ellipsize(summary_text, 120),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    canvas.pop_clip();
}

void paint_editor_panel(Canvas& canvas, const PaintContext& context, const Metrics& metrics,
                        const RECT& rect, std::vector<Hotspot>& hotspots) {
    const AppState& state = *context.state;
    const FontSet& fonts = *context.fonts;
    const UINT dpi = context.dpi;
    draw_panel_frame(canvas, rect, fonts, dpi, L"Screen arc editor");

    RECT canvas_rect = rect;
    canvas_rect.left += metrics.pad;
    canvas_rect.right -= metrics.pad;
    canvas_rect.top += metrics.pad + scale_px(22, dpi);
    canvas_rect.bottom -= scale_px(34, dpi);
    if (canvas_rect.bottom <= canvas_rect.top + scale_px(40, dpi)) {
        return;
    }
    canvas.fill_round(canvas_rect, scale_px(8, dpi), palette::canvas);
    canvas.outline_round(canvas_rect, scale_px(8, dpi), 1, palette::border);

    const EditorGeometry geometry = editor_geometry(state.layout, canvas_rect, dpi);
    canvas.push_clip(canvas_rect);

    // Distance rings every metre, drawn as arcs in front of the head.
    const COLORREF ring_color = RGB(32, 40, 54);
    for (int metres = 1; metres <= 4; ++metres) {
        const int radius = static_cast<int>(metres * geometry.scale);
        if (radius > (canvas_rect.bottom - geometry.head.y) + scale_px(30, dpi)) {
            break;
        }
        POINT previous{0, 0};
        for (int step = -90; step <= 90; step += 6) {
            const float angle = static_cast<float>(step) * kPi / 180.0f;
            POINT point{geometry.head.x + static_cast<int>(std::sin(angle) * radius),
                        geometry.head.y - static_cast<int>(std::cos(angle) * radius)};
            if (step > -90) {
                HPEN pen = CreatePen(PS_SOLID, 1, ring_color);
                const HGDIOBJ saved = SelectObject(canvas.dc(), pen);
                MoveToEx(canvas.dc(), previous.x, previous.y, nullptr);
                LineTo(canvas.dc(), point.x, point.y);
                SelectObject(canvas.dc(), saved);
                DeleteObject(pen);
            }
            previous = point;
        }
    }
    // Placed label boxes; screen labels nudge down until they clear these.
    std::vector<RECT> label_blockers;
    // Yaw ticks.
    for (int angle_deg = -90; angle_deg <= 90; angle_deg += 15) {
        const float angle = static_cast<float>(angle_deg) * kPi / 180.0f;
        const int radius = scale_px(10, dpi);
        const int x = geometry.head.x + static_cast<int>(std::sin(angle) * radius);
        const int y = geometry.head.y - static_cast<int>(std::cos(angle) * radius);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(44, 54, 72));
        const HGDIOBJ saved = SelectObject(canvas.dc(), pen);
        MoveToEx(canvas.dc(), geometry.head.x, geometry.head.y, nullptr);
        LineTo(canvas.dc(), x, y);
        SelectObject(canvas.dc(), saved);
        DeleteObject(pen);
        if (angle_deg == -90 || angle_deg == 0 || angle_deg == 90) {
            // Only the cardinal ticks are labelled, at a radius where the
            // three boxes cannot touch each other.
            const int label_radius = scale_px(52, dpi);
            wchar_t label[8];
            std::swprintf(label, std::size(label), L"%d", angle_deg);
            RECT text_rect{geometry.head.x +
                               static_cast<int>(std::sin(angle) * label_radius) -
                               scale_px(18, dpi),
                           geometry.head.y -
                               static_cast<int>(std::cos(angle) * label_radius) -
                               scale_px(18, dpi),
                           0, 0};
            text_rect.right = text_rect.left + scale_px(36, dpi);
            text_rect.bottom = text_rect.top + scale_px(14, dpi);
            canvas.text(fonts.small, palette::text_faint, text_rect, label,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            label_blockers.push_back(text_rect);
        }
    }

    // The wearer.
    draw_dot(canvas, geometry.head.x, geometry.head.y, scale_px(5, dpi), palette::accent);
    RECT head_label = rect_of(geometry.head.x - scale_px(40, dpi), geometry.head.y + scale_px(6, dpi),
                              scale_px(80, dpi), scale_px(14, dpi));
    canvas.text(fonts.small, palette::text_faint, head_label, L"head", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    label_blockers.push_back(head_label);

    for (size_t index = 0; index < state.layout.screens.size(); ++index) {
        const ScreenLayout& screen = state.layout.screens[index];
        const float yaw = screen.yaw_deg * kPi / 180.0f;
        const POINT center = editor_screen_center(screen, geometry);
        const float half_width = std::max(static_cast<float>(scale_px(9, dpi)),
                                          screen.width_m * 0.5f * geometry.scale);
        const float dx = std::cos(yaw) * half_width;
        const float dy = std::sin(yaw) * half_width;
        const COLORREF tint = RGB(static_cast<int>(screen.color[0] * 255.0f),
                                  static_cast<int>(screen.color[1] * 255.0f),
                                  static_cast<int>(screen.color[2] * 255.0f));
        const bool selected = index == state.selected_screen;
        const int thickness = selected ? scale_px(6, dpi) : scale_px(4, dpi);
        HPEN pen = CreatePen(PS_SOLID, thickness, selected ? palette::accent : tint);
        const HGDIOBJ saved = SelectObject(canvas.dc(), pen);
        MoveToEx(canvas.dc(), center.x - static_cast<int>(dx), center.y - static_cast<int>(dy),
                 nullptr);
        LineTo(canvas.dc(), center.x + static_cast<int>(dx), center.y + static_cast<int>(dy));
        SelectObject(canvas.dc(), saved);
        DeleteObject(pen);
        draw_dot(canvas, center.x - static_cast<int>(dx), center.y - static_cast<int>(dy),
                 thickness / 2 + 1, selected ? palette::accent : tint);
        draw_dot(canvas, center.x + static_cast<int>(dx), center.y + static_cast<int>(dy),
                 thickness / 2 + 1, selected ? palette::accent : tint);

        wchar_t label[64];
        const std::wstring id_text = wide_from_utf8(screen.id);
        std::swprintf(label, std::size(label), L"%s  %.0f deg", id_text.c_str(),
                      static_cast<double>(screen.yaw_deg));
        RECT label_rect = rect_of(center.x - scale_px(70, dpi), center.y - scale_px(28, dpi),
                                  scale_px(140, dpi), scale_px(15, dpi));
        // Greedy de-collision: nudge the label down until it clears every
        // placed label (bounded; the canvas clip contains the rest).
        const int label_step = scale_px(15, dpi);
        for (int attempt = 0; attempt < 8; ++attempt) {
            bool overlaps = false;
            for (const RECT& placed : label_blockers) {
                RECT intersection{};
                RECT inflated = label_rect;
                InflateRect(&inflated, scale_px(2, dpi), scale_px(1, dpi));
                if (IntersectRect(&intersection, &inflated, &placed) != 0) {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) {
                break;
            }
            label_rect.top += label_step;
            label_rect.bottom += label_step;
        }
        label_blockers.push_back(label_rect);
        canvas.text(fonts.small, selected ? palette::text : palette::text_dim, label_rect, label,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    canvas.pop_clip();

    RECT hint = rect_of(rect.left + metrics.pad, canvas_rect.bottom + scale_px(4, dpi),
                        rect.right - rect.left - metrics.pad * 2, scale_px(26, dpi));
    std::wstring hint_text = L"Drag a screen left/right for yaw, up/down for pitch. "
                             L"Tab to the arc and use the arrow keys for keyboard control.";
    canvas.text(fonts.small, palette::text_faint, hint, ellipsize(hint_text, 150),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT help_editor = rect_of(rect.right - metrics.pad - scale_px(24, dpi),
                                 rect.top + metrics.pad + scale_px(1, dpi), scale_px(24, dpi),
                                 scale_px(20, dpi));
    draw_chip(canvas, help_editor, L"?", palette::text_faint,
              state.focus_id == ui_help_id(5, 0), state.hover_id == ui_help_id(5, 0), fonts.small);
    add_hotspot(hotspots, ui_help_id(5, 0), static_cast<int>(HelpTopic::EditorCanvas), help_editor);

    Hotspot editor;
    editor.id = kUiEditorCanvas;
    editor.arg = 0;
    editor.rect = canvas_rect;
    editor.enabled = true;
    hotspots.push_back(editor);
}

void paint_fields_panel(Canvas& canvas, const PaintContext& context, const Metrics& metrics,
                        const RECT& rect, std::vector<Hotspot>& hotspots) {
    const AppState& state = *context.state;
    const FontSet& fonts = *context.fonts;
    const UINT dpi = context.dpi;
    draw_panel_frame(canvas, rect, fonts, dpi, L"Selected screen, view and capture");

    const bool has_screen = !state.layout.screens.empty() &&
                            state.selected_screen < state.layout.screens.size();
    if (!has_screen) {
        RECT empty = rect_of(rect.left + metrics.pad, rect.top + metrics.pad + scale_px(22, dpi),
                             rect.right - rect.left - metrics.pad * 2, scale_px(24, dpi));
        canvas.text(fonts.body, palette::warning, empty,
                    L"No screens in the layout; add a screen to edit it.",
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        hide_portal(context, kScrollFields);
        return;
    }
    const ScreenLayout& screen = state.layout.screens[state.selected_screen];

    const LayoutField fields[] = {
        LayoutField::Yaw,       LayoutField::Pitch,     LayoutField::Roll,
        LayoutField::Distance,  LayoutField::Width,     LayoutField::Height,
        LayoutField::Fov,       LayoutField::ActiveFps, LayoutField::MidFps,
        LayoutField::IdleFps,   LayoutField::EnterDeg,  LayoutField::LeaveDeg,
    };
    const int columns = 2;
    const int rows = static_cast<int>(std::size(fields)) / columns;
    const int gutter = scale_px(12, dpi);
    const int left = rect.left + metrics.pad;
    const int width = rect.right - rect.left - metrics.pad * 2 - gutter;
    RECT view{left, rect.top + metrics.pad + scale_px(22, dpi), left + width,
              rect.bottom - scale_px(6, dpi)};
    const int column_width = (width - metrics.gap) / columns;
    const int content_h = rows * metrics.slider;
    const int offset =
        paint_scrollbar(canvas, context, kScrollFields, rect, view, content_h, false, hotspots);
    canvas.push_clip(view);
    int y = view.top - offset;

    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const LayoutField field = fields[row * columns + column];
            float minimum = 0.0f;
            float maximum = 0.0f;
            float step = 0.0f;
            layout_field_range(field, minimum, maximum, step);
            const float value = layout_field_value(state.layout, screen, field);
            const float fraction = maximum > minimum ? (value - minimum) / (maximum - minimum) : 0.0f;
            const int help_w = scale_px(26, dpi);
            RECT slider = rect_of(left + column * (column_width + metrics.gap), y,
                                  column_width - help_w - metrics.gap, metrics.slider);
            draw_slider(canvas, slider, layout_field_label(field),
                        wide_from_utf8(format_layout_field_value(field, value)), fraction,
                        state.focus_id == ui_field_id(field), state.hover_id == ui_field_id(field),
                        true, fonts.small, fonts.body);
            add_clipped_hotspot(hotspots, ui_field_id(field), static_cast<int>(field), slider,
                                view, kScrollFields);
            RECT help_rect = rect_of(slider.right + metrics.gap, y, help_w, metrics.slider);
            const int help_id = ui_help_id(4, static_cast<int>(field));
            draw_chip(canvas, help_rect, L"?", palette::text_faint, state.focus_id == help_id,
                      state.hover_id == help_id, fonts.small);
            add_clipped_hotspot(hotspots, help_id, static_cast<int>(help_for_field(field)),
                                help_rect, view, kScrollFields);
        }
        y += metrics.slider;
    }
    canvas.pop_clip();
}

void paint_footer(Canvas& canvas, const PaintContext& context, const Metrics& metrics) {
    const AppState& state = *context.state;
    const FontSet& fonts = *context.fonts;
    const UINT dpi = context.dpi;
    RECT footer{context.client.left + metrics.margin, context.client.bottom - metrics.footer,
                context.client.right - metrics.margin, context.client.bottom};
    canvas.fill(footer, palette::background);
    RECT line{footer.left, footer.top, footer.right, footer.top + 1};
    canvas.separator(line, palette::border);

    std::wstring text = L"Tab: move focus  |  arrows: adjust  |  wheel: scroll  |  "
                        L"Space: activate  |  Ctrl+Shift+R: recenter  |  Ctrl+Alt+Y/P/Q: hotkeys  |  "
                        L"Ctrl+Shift+\\: exit workspace";
    COLORREF color = palette::text_faint;
    if (!state.toast.empty() && state.now_s < state.toast_expiry_s) {
        text = state.toast;
        color = palette::accent;
    }
    RECT text_rect = footer;
    text_rect.top += scale_px(2, dpi);
    canvas.text(fonts.small, color, text_rect, ellipsize(text, 170),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT hint_rect = footer;
    hint_rect.top += scale_px(2, dpi);
    canvas.text(fonts.small, palette::text_faint, hint_rect,
                state.engine.busy() ? L"engine running" : L"engine stopped",
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
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
    const float usable_width = static_cast<float>(rect.right - rect.left) * 0.5f -
                               static_cast<float>(scale_px(14, dpi));
    const float usable_height = static_cast<float>(geometry.head.y - rect.top) -
                                static_cast<float>(scale_px(8, dpi));
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

void paint_help_bubble(Canvas& canvas, const PaintContext& context, const Metrics& metrics,
                         const std::vector<Hotspot>& hotspots) {
    const AppState& state = *context.state;
    const FontSet& fonts = *context.fonts;
    const UINT dpi = context.dpi;
    int id = state.hover_id;
    if (id < kUiHelpBase || id >= kUiHelpBase + 7 * 64) {
        id = state.focus_id;
    }
    if (id < kUiHelpBase || id >= kUiHelpBase + 7 * 64) {
        return;
    }
    int topic = -1;
    RECT anchor{};
    bool found = false;
    for (const Hotspot& spot : hotspots) {
        if (spot.id == id) {
            topic = spot.arg;
            anchor = spot.rect;
            found = true;
            break;
        }
    }
    if (!found || topic < 0 || topic >= static_cast<int>(HelpTopic::Count)) {
        return;
    }
    const wchar_t* text = help_text(static_cast<HelpTopic>(topic));
    if (text == nullptr || *text == L'\0') {
        return;
    }
    const int margin = metrics.margin;
    const int max_width =
        std::min(scale_px(380, dpi), static_cast<int>(context.client.right - context.client.left) -
                                            margin * 2);
    if (max_width < scale_px(160, dpi)) {
        return;
    }
    const int pad = scale_px(8, dpi);
    const int height =
        wrapped_text_height(canvas.dc(), fonts.small, max_width - pad * 2, text) + pad * 2;
    int x = anchor.left;
    int y = anchor.bottom + scale_px(4, dpi);
    if (x + max_width > context.client.right - margin) {
        x = context.client.right - margin - max_width;
    }
    if (x < context.client.left + margin) {
        x = context.client.left + margin;
    }
    if (y + height > context.client.bottom - metrics.footer) {
        y = anchor.top - height - scale_px(4, dpi);
    }
    const RECT box{x, y, x + max_width, y + height};
    canvas.fill_round(box, scale_px(6, dpi), palette::panel_alt);
    canvas.outline_round(box, scale_px(6, dpi), 1, palette::border);
    const RECT text_rect{box.left + pad, box.top + pad, box.right - pad, box.bottom - pad};
    canvas.text(fonts.small, palette::text, text_rect, text, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void paint_window(Canvas& canvas, const PaintContext& context, std::vector<Hotspot>& hotspots) {
    hotspots.clear();
    if (context.state == nullptr || context.fonts == nullptr) {
        return;
    }
    const Metrics metrics = metrics_for(context.dpi, canvas.width());

    canvas.fill(context.client, palette::background);
    paint_header(canvas, context, metrics, hotspots);

    int y = context.client.top + metrics.header + scale_px(10, context.dpi);
    paint_banner(canvas, context, metrics, y);

    const int content_top = y;
    const int content_bottom =
        context.client.bottom - metrics.footer - scale_px(8, context.dpi);
    const int left = context.client.left + metrics.margin;
    const int right = context.client.right - metrics.margin;
    const int left_width = metrics.left_width;
    const int right_left = left + left_width + metrics.gap;
    const int right_width = right - right_left;
    const int available_height = content_bottom - content_top;
    const int min_panel = scale_px(60, context.dpi);

    const int desired_left[3] = {scale_px(316, context.dpi), scale_px(236, context.dpi),
                                 scale_px(220, context.dpi)};
    const int minimum_left[3] = {scale_px(150, context.dpi), scale_px(170, context.dpi),
                                 scale_px(110, context.dpi)};
    int left_h[3] = {0, 0, 0};
    distribute_heights(available_height, metrics.gap, desired_left, minimum_left, 2, left_h);
    RECT status_rect = rect_of(left, content_top, left_width, left_h[0]);
    RECT engine_rect = rect_of(left, status_rect.bottom + metrics.gap, left_width, left_h[1]);
    RECT diagnostics_rect =
        rect_of(left, engine_rect.bottom + metrics.gap, left_width, left_h[2]);
    if (left_h[0] >= min_panel) {
        paint_status_panel(canvas, context, metrics, status_rect, hotspots);
    } else {
        hide_portal(context, kScrollStatus);
    }
    if (left_h[1] >= min_panel) {
        paint_engine_panel(canvas, context, metrics, engine_rect, hotspots);
    } else {
        hide_portal(context, kScrollEngine);
    }
    if (left_h[2] >= min_panel) {
        paint_diagnostics_panel(canvas, context, metrics, diagnostics_rect, hotspots);
    } else {
        hide_portal(context, kScrollDiagnostics);
    }

    const int desired_right[3] = {scale_px(168, context.dpi), scale_px(300, context.dpi),
                                  scale_px(292, context.dpi)};
    const int minimum_right[3] = {scale_px(120, context.dpi), scale_px(100, context.dpi),
                                  scale_px(140, context.dpi)};
    int right_h[3] = {0, 0, 0};
    distribute_heights(available_height, metrics.gap, desired_right, minimum_right, 1, right_h);
    RECT layout_rect = rect_of(right_left, content_top, right_width, right_h[0]);
    RECT editor_rect =
        rect_of(right_left, layout_rect.bottom + metrics.gap, right_width, right_h[1]);
    RECT fields_rect =
        rect_of(right_left, editor_rect.bottom + metrics.gap, right_width, right_h[2]);
    if (right_h[0] >= min_panel) {
        paint_layout_panel(canvas, context, metrics, layout_rect, hotspots);
    } else {
        hide_portal(context, kScrollLayout);
    }
    if (right_h[1] >= min_panel) {
        paint_editor_panel(canvas, context, metrics, editor_rect, hotspots);
    }
    if (right_h[2] >= min_panel) {
        paint_fields_panel(canvas, context, metrics, fields_rect, hotspots);
    } else {
        hide_portal(context, kScrollFields);
    }

    paint_footer(canvas, context, metrics);
    paint_help_bubble(canvas, context, metrics, hotspots);
}

}  // namespace gt::ui
