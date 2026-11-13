// Page painters: Home, Comfort, Tracking, Diagnostics, Settings and the
// shared calibration-progress block. The Layout page lives in
// ui_page_layout.cpp and the setup guide in ui_onboarding.cpp.

#include "app/engine_protocol.h"
#include "app_shell/ui_cal_guide.h"
#include "app_shell/ui_paint_internal.h"
#include "imu/orientation_calibration.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace gt::ui::detail {
namespace {

// Lays a page body out inside the page scroll area. fn(left, top, width)
// paints from top and returns the bottom y of what it drew.
template <typename Fn>
void scroll_page(Ui& ui, const RECT& body, Fn&& fn) {
    const int bleed = ui.px(space::sm);
    RECT view{body.left - bleed, body.top - ui.px(space::xs), body.right + bleed, body.bottom};
    ScrollArea area = begin_scroll(ui, kScrollPage, view, view);
    const int top = body.top - area.offset;
    const int bottom = fn(static_cast<int>(body.left), top, rw(body));
    end_scroll(ui, area, bottom - top + ui.px(space::xxl) + ui.px(space::xs));
}

std::wstring uptime_text(double seconds) {
    const int total = static_cast<int>(std::max(0.0, seconds));
    const int h = total / 3600;
    const int m = (total / 60) % 60;
    const int s = total % 60;
    if (h > 0) {
        return format_wide(L"%dh %02dm", h, m);
    }
    return format_wide(L"%dm %02ds", m, s);
}

// Small round status icon used by checklists.
void status_icon(Ui& ui, int cx, int cy, Severity severity) {
    const COLORREF color = severity_colour(ui.t, severity);
    ui.c.fill_circle(cx, cy, static_cast<float>(ui.px(14)), mix(ui.t.surface, color, ui.t.dark ? 0.2f : 0.14f));
    draw_icon(ui.c, rect_of(cx - ui.px(14), cy - ui.px(14), ui.px(28), ui.px(28)), severity_glyph(severity),
              ui.f.icon_small, color);
}

int metric_tile(Ui& ui, int left, int top, int width, const wchar_t* label, const std::wstring& value) {
    const int h = ui.px(64);
    RECT tile = rect_of(left, top, width, h);
    ui.c.fill_round(tile, ui.px(radius::md), ui.t.surface_alt);
    ui.c.text(ui.f.caption, ui.t.text_faint, rect_of(left + ui.px(14), top + ui.px(10), width - ui.px(20), ui.px(16)),
              label);
    ui.c.text(ui.f.title, ui.t.text, rect_of(left + ui.px(14), top + ui.px(28), width - ui.px(20), ui.px(28)),
              value, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return h;
}

int empty_state(Ui& ui, int left, int top, int width, const wchar_t* glyph_text, const std::wstring& text) {
    const int h = ui.px(96);
    draw_icon(ui.c, rect_of(left, top + ui.px(8), width, ui.px(36)), glyph_text, ui.f.icon_large,
              ui.t.border_strong);
    RECT text_rect = rect_of(left + ui.px(space::lg), top + ui.px(52), width - ui.px(space::xxl), ui.px(40));
    ui.c.text(ui.f.small, ui.t.text_faint, text_rect, text, DT_CENTER | DT_TOP | DT_WORDBREAK);
    return h;
}

// --- Home -----------------------------------------------------------------

// Startup steps under the hero text: one dot per step (done, active,
// pending) joined by a hairline, the label beside it, and the active step's
// progress bar below when it has one. Returns the height used.
int startup_steps_height(Ui& ui, const StartupView& startup) {
    if (startup.step_count <= 0 && startup.progress < 0.0) {
        return 0;
    }
    int h = 0;
    if (startup.step_count > 0) {
        h += ui.px(24);
    }
    if (startup.progress >= 0.0) {
        h += (h > 0 ? ui.px(space::md) : 0) + ui.px(8);
    }
    return h;
}

void paint_startup_steps(Ui& ui, int left, int top, int width, const StartupView& startup) {
    const Theme& t = ui.t;
    int y = top;
    if (startup.step_count > 0) {
        const int dot_r = ui.px(9);
        const int slot_w = width / startup.step_count;
        const int cy = y + ui.px(12);
        for (int i = 0; i < startup.step_count; ++i) {
            const int x = left + slot_w * i;
            const int cx = x + dot_r;
            const bool done = i < startup.active_step;
            const bool active = i == startup.active_step;
            if (i + 1 < startup.step_count) {
                // Hairline to the next dot, behind the label space.
                const int line_left = cx + dot_r + ui.px(space::xs);
                const int label_w = text_width(ui.c.dc(), active ? ui.f.small_strong : ui.f.small, startup.steps[i]);
                const int run_left = line_left + label_w + ui.px(space::sm);
                const int run_right = x + slot_w - ui.px(space::sm);
                if (run_right > run_left) {
                    ui.c.fill(rect_of(run_left, cy, run_right - run_left, std::max(1, ui.px(1))),
                              done ? t.ok : t.border);
                }
            }
            if (done) {
                ui.c.fill_circle(cx, cy, static_cast<float>(dot_r), mix(t.surface, t.ok, t.dark ? 0.24f : 0.16f));
                draw_icon(ui.c, rect_of(cx - dot_r, cy - dot_r, dot_r * 2, dot_r * 2), glyph::check, ui.f.icon_small,
                          t.ok);
            } else if (active) {
                ui.c.fill_circle(cx, cy, static_cast<float>(dot_r), mix(t.surface, t.accent, t.dark ? 0.24f : 0.16f));
                ui.c.fill_circle(cx, cy, static_cast<float>(ui.px(4)), t.accent);
            } else {
                ui.c.ring(cx, cy, static_cast<float>(dot_r) - 0.5f, static_cast<float>(std::max(1, ui.px(1))),
                          t.border_strong, 255);
            }
            ui.c.text(active ? ui.f.small_strong : ui.f.small, active ? t.text : done ? t.text_dim : t.text_faint,
                      rect_of(cx + dot_r + ui.px(space::xs), y, slot_w - dot_r * 2 - ui.px(space::xs), ui.px(24)),
                      startup.steps[i], DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        y += ui.px(24) + ui.px(space::md);
    }
    if (startup.progress >= 0.0) {
        draw_progress_bar(ui.c, rect_of(left, y, width, ui.px(8)), static_cast<float>(startup.progress), t.accent);
    }
}

int paint_hero(Ui& ui, int left, int top, int width) {
    AppState& s = ui.s;
    const Theme& t = ui.t;
    EngineView view = engine_view(s, t);
    const bool busy = view.busy;
    // Startup phases reported by the engine replace the generic Starting /
    // Running copy with plain steps; an engine without the phase key keeps
    // the generic view.
    const StartupView startup = engine_startup_view(s);
    const bool prompt = startup.visible && (startup.hold_still || startup.get_ready);
    if (startup.visible) {
        view.headline = startup.headline;
        view.detail = startup.detail;
        view.color = t.accent;
        // A calm, still screen while the wearer must keep still.
        view.transitional = !startup.hold_still;
    }
    const int pad = ui.px(28);
    const int ring_r = ui.px(30);
    const bool wide = width >= ui.px(820);
    const int metrics_w = wide && busy && !startup.visible ? ui.px(320) : 0;
    const int text_left = left + pad + ring_r * 2 + ui.px(space::xl);
    const int text_w = width - (text_left - left) - pad - (metrics_w > 0 ? metrics_w + ui.px(space::xl) : 0);
    // The hold-still prompt sits on an accent panel in the display font.
    const int panel_pad = prompt ? ui.px(space::md) : 0;
    const int inner_w = text_w - panel_pad * 2;
    const HFONT headline_font = prompt ? ui.f.display : ui.f.title;
    const int headline_h = prompt ? ui.px(38) : ui.px(32);
    const int detail_h = wrapped_text_height(ui.c.dc(), ui.f.body, inner_w, view.detail);
    const int text_h = headline_h + ui.px(4) + detail_h + panel_pad * 2;
    const int block_top = top + pad;
    const int content_h = std::max(ring_r * 2, text_h);
    const int steps_h = startup.visible ? startup_steps_height(ui, startup) : 0;
    const int steps_top = block_top + content_h + ui.px(space::xl);
    const int buttons_top = steps_top + (steps_h > 0 ? steps_h + ui.px(space::xl) : 0);
    const int button_h = ui.px(size::control_lg);
    const std::wstring workspace_block = start_block_reason(s, LaunchMode::Workspace);
    const std::wstring preview_block = start_block_reason(s, LaunchMode::Preview);
    // The headline already explains the workspace block; a different
    // reason that also blocks preview gets its own line.
    std::wstring note;
    if (!busy && !preview_block.empty() && preview_block != workspace_block) {
        note = L"Preview: " + preview_block;
    }
    // Before a start: when the wearer will be asked to keep still (the
    // startup gyro window is the biggest drift source when it sees motion).
    const std::wstring hint = !busy && (workspace_block.empty() || preview_block.empty())
                                  ? std::wstring(kStartStillHint)
                                  : std::wstring();
    const int note_w = width - pad * 2;
    const int hint_h = hint.empty() ? 0 : wrapped_text_height(ui.c.dc(), ui.f.small, note_w, hint) + ui.px(space::md);
    const int note_h = note.empty() ? 0 : wrapped_text_height(ui.c.dc(), ui.f.small, note_w, note) + ui.px(space::md);
    const int height = buttons_top - top + button_h + hint_h + note_h + pad;
    RECT hero = rect_of(left, top, width, height);
    card(ui, hero);

    // Status ring.
    const int cx = left + pad + ring_r;
    const int cy = block_top + ring_r;
    const float phase = pulse_phase(ui, view.transitional);
    if (phase >= 0.0f) {
        ui.c.ring(cx, cy, static_cast<float>(ring_r) + phase * static_cast<float>(ui.px(12)),
                  static_cast<float>(ui.px(2)), view.color, static_cast<int>((1.0f - phase) * 160.0f));
    }
    ui.c.fill_circle(cx, cy, static_cast<float>(ring_r), mix(t.surface, view.color, t.dark ? 0.2f : 0.13f));
    const wchar_t* hero_glyph = prompt ? glyph::glasses
                                : busy ? (view.transitional ? glyph::refresh : glyph::glasses)
                                : view.color == t.accent ? glyph::play
                                                         : glyph::warning;
    draw_icon(ui.c, rect_of(cx - ring_r, cy - ring_r, ring_r * 2, ring_r * 2), hero_glyph, ui.f.icon_large,
              view.color);

    const int text_top = block_top + (content_h - text_h) / 2;
    if (prompt) {
        ui.c.fill_round(rect_of(text_left, text_top, text_w, text_h), ui.px(radius::lg), t.accent_soft);
    }
    // text on accent_soft is a checked contrast pair; text_dim is not, so
    // the prompt's detail uses text too.
    ui.c.text(headline_font, t.text, rect_of(text_left + panel_pad, text_top + panel_pad, inner_w, headline_h),
              view.headline, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ui.c.text(ui.f.body, prompt ? t.text : t.text_dim,
              rect_of(text_left + panel_pad, text_top + panel_pad + headline_h + ui.px(4), inner_w, detail_h),
              view.detail, DT_LEFT | DT_TOP | DT_WORDBREAK);
    if (steps_h > 0) {
        paint_startup_steps(ui, text_left, steps_top, width - (text_left - left) - pad, startup);
    }

    if (metrics_w > 0) {
        const int tile_w = (metrics_w - ui.px(space::sm) * 2) / 3;
        const int tiles_left = hero.right - pad - metrics_w;
        const EngineSnapshot& engine = s.engine.snapshot();
        const int screens = s.engine_status.parsed ? s.engine_status.screens
                            : engine.query_answered ? engine.query_screens
                                                    : static_cast<int>(s.layout.screens.size());
        metric_tile(ui, tiles_left, block_top, tile_w, L"Screens", std::to_wstring(screens));
        metric_tile(ui, tiles_left + tile_w + ui.px(space::sm), block_top, tile_w, L"Frame rate",
                    s.engine_status.parsed && s.engine_status.fps > 0.0
                        ? format_wide(L"%.0f fps", s.engine_status.fps)
                        : std::wstring(L"--"));
        metric_tile(ui, tiles_left + (tile_w + ui.px(space::sm)) * 2, block_top, tile_w, L"Uptime",
                    uptime_text(engine.uptime_s));
    }

    // Actions.
    const int help_w = help_mark_width(ui.dpi);
    int x = left + pad;
    const auto action = [&](int id, const wchar_t* label, ButtonStyle style, bool enabled, const wchar_t* icon,
                            int button_w, int help_row, HelpTopic topic) {
        RECT r = rect_of(x, buttons_top, button_w, button_h);
        button(ui, id, r, label, style, enabled, icon);
        RECT help_rect = rect_of(r.right + ui.px(space::sm), buttons_top + (button_h - help_w) / 2, help_w, help_w);
        ui.help(kHelpEngine, help_row, topic, help_rect);
        x = help_rect.right + ui.px(space::lg);
    };
    const int narrow_w = ui.px(150);
    if (!busy) {
        action(kUiStartWorkspace, L"Start workspace", ButtonStyle::Primary, workspace_block.empty(), glyph::play,
               ui.px(212), kRowStartWorkspace, HelpTopic::EngineStartWorkspace);
        action(kUiStartPreview, L"Start preview", ButtonStyle::Secondary, preview_block.empty(), glyph::preview,
               ui.px(180), kRowStartPreview, HelpTopic::EngineStartPreview);
    } else {
        action(kUiRecenter, L"Recenter", ButtonStyle::Primary, busy, glyph::recenter, narrow_w, kRowRecenter,
               HelpTopic::EngineRecenter);
        action(kUiReloadLayout, L"Reload layout", ButtonStyle::Secondary, busy, glyph::refresh, ui.px(170),
               kRowReload, HelpTopic::EngineReloadLayout);
        action(kUiStopEngine, L"Stop", ButtonStyle::Danger, busy, glyph::stop, ui.px(120), kRowStop,
               HelpTopic::EngineStop);
    }
    if (!hint.empty()) {
        ui.c.text(ui.f.small, t.text_dim, rect_of(left + pad, buttons_top + button_h + ui.px(space::md), note_w, hint_h),
                  hint, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    if (!note.empty()) {
        ui.c.text(ui.f.small, t.warning,
                  rect_of(left + pad, buttons_top + button_h + hint_h + ui.px(space::md), note_w, note_h), note,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    return hero.bottom;
}

int paint_readiness(Ui& ui, int left, int top, int width) {
    const std::vector<HealthItem> items = health_items(ui.s);
    const int base_row_h = ui.px(64);
    const int rows = 5;
    const int pad = ui.px(space::xl);
    const int help_w = help_mark_width(ui.dpi);
    const int text_left = left + pad + ui.px(44);
    struct RowAction {
        int id = 0;
        const wchar_t* label = nullptr;
    };
    const auto row_action = [&](int i, const HealthItem& item) {
        RowAction action;
        if (item.severity != Severity::Ok) {
            if (i == 1) {
                action = {kUiGoBase + static_cast<int>(AppPage::Diagnostics), L"Troubleshoot"};
            } else if (i == 2) {
                action = {kUiGoBase + static_cast<int>(AppPage::Tracking), L"Calibrate"};
            }
            // The layout-file row has no button of its own: the Layout card
            // beside it always shows "Edit layout" with the same go-to id,
            // and one id registered twice traps Tab focus (cycle_focus finds
            // the first copy, so forward Tab loops and never reaches the
            // sidebar again).
        }
        return action;
    };
    const auto row_text_w = [&](const RowAction& action) {
        const int action_w = action.label != nullptr ? ui.px(132) : 0;
        return width - (text_left - left) - pad - help_w - ui.px(space::md) -
               (action_w > 0 ? action_w + ui.px(space::md) : 0);
    };
    // A fix-it detail that does not fit on one line (a row with an action
    // button is narrower) wraps instead of being cut off mid-word; the row
    // grows by the extra lines.
    const int detail_line_h = wrapped_text_height(ui.c.dc(), ui.f.small, width, L"Ag");
    int row_heights[rows + 1] = {};
    int rows_h = 0;
    for (int i = 1; i <= rows; ++i) {
        const HealthItem& item = items[static_cast<size_t>(i)];
        const int detail_h = wrapped_text_height(ui.c.dc(), ui.f.small, row_text_w(row_action(i, item)), item.detail);
        row_heights[i] = base_row_h + std::max(0, detail_h - detail_line_h);
        rows_h += row_heights[i];
    }
    RECT frame = rect_of(left, top, width, 0);
    // Measure header first so the card can be drawn before its content.
    const int header_h = ui.px(30) + wrapped_text_height(ui.c.dc(), ui.f.small, width - pad * 2,
                                                        L"What the workspace needs before it can start.") +
                         ui.px(space::lg) + pad;
    frame.bottom = top + header_h + rows_h + pad - ui.px(space::sm);
    card(ui, frame);
    int y = card_header(ui, frame, L"Readiness", L"What the workspace needs before it can start.");
    for (int i = 1; i <= rows; ++i) {
        const HealthItem& item = items[static_cast<size_t>(i)];
        const int row_top = y;
        const int row_h = row_heights[i];
        if (i > 1) {
            ui.c.fill(rect_of(left + pad, row_top, width - pad * 2, 1), ui.t.border);
        }
        status_icon(ui, left + pad + ui.px(14), row_top + row_h / 2, item.severity);
        const RowAction act = row_action(i, item);
        const int action_id = act.id;
        const wchar_t* action_label = act.label;
        const int action_w = action_label != nullptr ? ui.px(132) : 0;
        const int text_w = row_text_w(act);
        ui.c.text(ui.f.body_strong, ui.t.text, rect_of(text_left, row_top + ui.px(11), text_w, ui.px(22)),
                  std::wstring(item.title) + L"  ·  " + item.value,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (row_h == base_row_h) {
            ui.c.text(ui.f.small, ui.t.text_dim, rect_of(text_left, row_top + ui.px(33), text_w, ui.px(20)),
                      item.detail, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            const int one_line_top = row_top + ui.px(33) + (ui.px(20) - detail_line_h) / 2;
            ui.c.text(ui.f.small, ui.t.text_dim, rect_of(text_left, one_line_top, text_w, row_h), item.detail,
                      DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        const int right = left + width - pad;
        RECT help_rect = rect_of(right - help_w, row_top + (row_h - help_w) / 2, help_w, help_w);
        ui.help(kHelpStatus, item.status_row, item.topic, help_rect);
        if (action_label != nullptr) {
            RECT action = rect_of(help_rect.left - ui.px(space::md) - action_w, row_top + (row_h - ui.px(34)) / 2,
                                  action_w, ui.px(34));
            draw_button(ui.c, action, action_label, ButtonStyle::Secondary, ui.ws(action_id), ui.f.small_strong);
            ui.hot(action_id, 0, action);
        }
        y += row_h;
    }
    return frame.bottom;
}

int paint_layout_glance(Ui& ui, int left, int top, int width) {
    const int pad = ui.px(space::xl);
    const int preview_h = ui.px(150);
    std::wstring summary = wide_from_utf8(layout_display_summary(ui.s.layout));
    if (ui.s.layout_dirty) {
        summary += L"  ·  unsaved changes";
    }
    const int summary_h = wrapped_text_height(ui.c.dc(), ui.f.small, width - pad * 2, summary);
    const int height = pad + ui.px(30) + ui.px(space::md) + preview_h + ui.px(space::md) + summary_h +
                       ui.px(space::lg) + ui.px(size::control_sm) + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    ui.c.text(ui.f.title, ui.t.text, rect_of(left + pad, top + pad, width - pad * 2, ui.px(28)), L"Layout");
    if (!ui.s.loaded_preset.empty()) {
        const std::wstring name = wide_from_utf8(ui.s.loaded_preset);
        const int badge_w = text_width(ui.c.dc(), ui.f.small_strong, name) + ui.px(24);
        draw_badge(ui.c, rect_of(frame.right - pad - badge_w, top + pad + ui.px(2), badge_w, ui.px(24)), name,
                   ui.t.accent, ui.f.small_strong);
    }
    int y = top + pad + ui.px(30) + ui.px(space::md);
    RECT preview = rect_of(left + pad, y, width - pad * 2, preview_h);
    ui.c.fill_round(preview, ui.px(radius::md), ui.t.canvas);
    paint_arc_preview(ui, preview, ui.s.layout, -1);
    y += preview_h + ui.px(space::md);
    ui.c.text(ui.f.small, ui.s.layout_dirty ? ui.t.warning : ui.t.text_dim,
              rect_of(left + pad, y, width - pad * 2, summary_h), summary, DT_LEFT | DT_TOP | DT_WORDBREAK);
    y += summary_h + ui.px(space::lg);
    const int go = kUiGoBase + static_cast<int>(AppPage::Layout);
    RECT edit = rect_of(left + pad, y, ui.px(150), ui.px(size::control_sm));
    draw_button(ui.c, edit, L"Edit layout", ButtonStyle::Secondary, ui.ws(go), ui.f.small_strong);
    ui.hot(go, 0, edit);
    return frame.bottom;
}

int paint_live_tracking(Ui& ui, int left, int top, int width) {
    const int pad = ui.px(space::xl);
    const TelemetrySample& sample = ui.s.diagnostics.telemetry;
    const bool live = ui.s.engine.busy() && ui.s.diagnostics.telemetry_present && sample.valid;
    const int body_h = live ? ui.px(64) + ui.px(space::md) + ui.px(20) : ui.px(96);
    const int height = pad + ui.px(30) + ui.px(space::md) + body_h + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    ui.c.text(ui.f.title, ui.t.text, rect_of(left + pad, top + pad, width - pad * 2, ui.px(28)), L"Head tracking");
    int y = top + pad + ui.px(30) + ui.px(space::md);
    if (!live) {
        empty_state(ui, left, y, width, glyph::tracking,
                    L"Live head angles appear here while the engine runs.");
        return frame.bottom;
    }
    const int tile_w = (width - pad * 2 - ui.px(space::sm) * 2) / 3;
    metric_tile(ui, left + pad, y, tile_w, L"Yaw", format_wide(L"%+.1f°", static_cast<double>(sample.yaw_deg)));
    metric_tile(ui, left + pad + tile_w + ui.px(space::sm), y, tile_w, L"Pitch",
                format_wide(L"%+.1f°", static_cast<double>(sample.pitch_deg)));
    metric_tile(ui, left + pad + (tile_w + ui.px(space::sm)) * 2, y, tile_w, L"Roll",
                format_wide(L"%+.1f°", static_cast<double>(sample.roll_deg)));
    y += ui.px(64) + ui.px(space::md);
    std::wstring status = sample.rest ? L"At rest" : (sample.still ? L"Still" : L"Moving");
    status += L"  ·  bias: ";
    status += adapt_state_label(sample.adapt_state);
    ui.c.text(ui.f.small, ui.t.text_dim, rect_of(left + pad, y, width - pad * 2, ui.px(20)), status);
    return frame.bottom;
}

// --- Comfort ---------------------------------------------------------------

int comfort_card_stabilise(Ui& ui, int left, int top, int width) {
    const ViewComfort& comfort = ui.s.config.comfort;
    const int pad = ui.px(space::xl);
    const wchar_t* description =
        L"Holds the screens steady while your head is nearly still, so small text stops swimming.";
    const int header_h = ui.px(30) + wrapped_text_height(ui.c.dc(), ui.f.small, width - pad * 2, description) +
                         ui.px(space::lg);
    const int height = pad + header_h + ui.px(size::control) + ui.px(space::md) + ui.px(20) + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    int y = card_header(ui, frame, L"Reading stabilisation", description);
    static const wchar_t* const kNames[] = {L"Off", L"Low", L"Medium", L"High", L"Ultra"};
    const int help_w = help_mark_width(ui.dpi);
    RECT seg = rect_of(left + pad, y, width - pad * 2 - help_w - ui.px(space::md), ui.px(size::control));
    segmented(ui, kUiStabiliseBase, seg, kNames, kComfortStabiliseLevels, comfort.stabilise_level);
    ui.help(kHelpEngine, kRowStabilise, HelpTopic::ComfortStabilise,
            rect_of(seg.right + ui.px(space::md), y + (rh(seg) - help_w) / 2, help_w, help_w));
    y += rh(seg) + ui.px(space::md);
    ui.c.text(ui.f.caption, ui.t.text_faint, rect_of(left + pad, y, width - pad * 2, ui.px(20)),
              L"Ctrl+Alt+S cycles the level while the workspace runs.");
    return frame.bottom;
}

int comfort_card_dimming(Ui& ui, int left, int top, int width) {
    AppState& s = ui.s;
    const ViewComfort& comfort = s.config.comfort;
    const int pad = ui.px(space::xl);
    const int help_w = help_mark_width(ui.dpi);
    const wchar_t* description =
        L"Dim screens you are not using: each at its own level, or automatically by where you look.";
    const int header_h = ui.px(30) + wrapped_text_height(ui.c.dc(), ui.f.small, width - pad * 2, description) +
                         ui.px(space::lg);
    struct SliderSpec {
        int id;
        std::wstring label;
        int value;
        int minimum;
        HelpTopic topic;
        int help_row;
    };
    std::vector<SliderSpec> sliders;
    if (comfort.dim_mode == DimMode::Manual) {
        const size_t screens = std::min(s.layout.screens.size(), static_cast<size_t>(kComfortMaxScreens));
        for (size_t i = 0; i < screens; ++i) {
            sliders.push_back({kUiBrightnessBase + static_cast<int>(i),
                               L"Screen '" + wide_from_utf8(s.layout.screens[i].id) + L"'",
                               comfort.screen_brightness_pct[i], kComfortMinBrightnessPct,
                               HelpTopic::ComfortBrightness, kRowBrightness + static_cast<int>(i)});
        }
    } else if (comfort.dim_mode == DimMode::Focus) {
        sliders.push_back({kUiFocusDim, L"Screens you are not looking at", comfort.focus_dim_pct,
                           kComfortMinBrightnessPct, HelpTopic::ComfortFocusDim, kRowFocusDim});
    }
    const int inner_w = width - pad * 2;
    const int columns = inner_w >= ui.px(620) && sliders.size() > 1 ? 2 : 1;
    const int col_gap = ui.px(space::xl);
    const int col_w = (inner_w - col_gap * (columns - 1)) / columns;
    const int slider_rows = static_cast<int>((sliders.size() + static_cast<size_t>(columns) - 1) /
                                             static_cast<size_t>(columns));
    const int sliders_h = slider_rows > 0 ? ui.px(space::lg) + slider_rows * (ui.px(size::slider) + ui.px(space::sm))
                                          : 0;
    const int height = pad + header_h + ui.px(size::control) + sliders_h + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    int y = card_header(ui, frame, L"Screen dimming", description);
    static const wchar_t* const kNames[] = {L"Off", L"Manual", L"Focus"};
    RECT seg = rect_of(left + pad, y, std::min(inner_w - help_w - ui.px(space::md), ui.px(420)), ui.px(size::control));
    segmented(ui, kUiDimModeBase, seg, kNames, kDimModeCount, static_cast<int>(comfort.dim_mode));
    ui.help(kHelpEngine, kRowDimMode, HelpTopic::ComfortDimMode,
            rect_of(seg.right + ui.px(space::md), y + (rh(seg) - help_w) / 2, help_w, help_w));
    y += rh(seg) + (sliders.empty() ? 0 : ui.px(space::lg));
    for (size_t i = 0; i < sliders.size(); ++i) {
        const SliderSpec& spec = sliders[i];
        const int column = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;
        const int sx = left + pad + column * (col_w + col_gap);
        const int sy = y + row * (ui.px(size::slider) + ui.px(space::sm));
        RECT r = rect_of(sx, sy, col_w - help_w - ui.px(space::md), ui.px(size::slider));
        const float fraction = static_cast<float>(spec.value - spec.minimum) / static_cast<float>(100 - spec.minimum);
        slider(ui, spec.id, 0, r, spec.label, format_wide(L"%d%%", spec.value), fraction, true,
               s.drag_comfort == spec.id);
        ui.help(kHelpEngine, spec.help_row, spec.topic,
                rect_of(r.right + ui.px(space::md), sy + ui.px(28), help_w, help_w));
    }
    return frame.bottom;
}

int comfort_card_tint(Ui& ui, int left, int top, int width) {
    const ViewComfort& comfort = ui.s.config.comfort;
    const int pad = ui.px(space::xl);
    const int help_w = help_mark_width(ui.dpi);
    const std::wstring description = L"Warms every screen by cutting blue light, for evening use. Applies live.";
    const int inner_w = width - pad * 2;
    // Measure the switch row with the same text width switch_row uses.
    const int text_w = inner_w - ui.px(size::switch_w) - help_w - ui.px(space::lg) * 2;
    const int row_h = std::max(ui.px(44), ui.px(24) + wrapped_text_height(ui.c.dc(), ui.f.small, text_w, description));
    const int height = pad + row_h + ui.px(space::lg) + ui.px(size::slider) + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    int y = top + pad;
    y += switch_row(ui, kUiNightTint, left + pad, y, inner_w, L"Night tint", description, comfort.night_tint, true,
                    kHelpEngine, kRowNightTint, HelpTopic::ComfortNightTint);
    y += ui.px(space::lg);
    RECT r = rect_of(left + pad, y, inner_w - help_w - ui.px(space::md), ui.px(size::slider));
    slider(ui, kUiNightTintStrength, 0, r, L"Warmth", format_wide(L"%d%%", comfort.night_tint_pct),
           static_cast<float>(comfort.night_tint_pct) / 100.0f, comfort.night_tint,
           ui.s.drag_comfort == kUiNightTintStrength);
    ui.help(kHelpEngine, kRowNightWarmth, HelpTopic::ComfortNightStrength,
            rect_of(r.right + ui.px(space::md), y + ui.px(28), help_w, help_w));
    return frame.bottom;
}

int comfort_card_cursor(Ui& ui, int left, int top, int width) {
    const int pad = ui.px(space::xl);
    const int help_w = help_mark_width(ui.dpi);
    const bool running = ui.s.engine.busy();
    const int button_w = ui.px(170);
    const int text_w = width - pad * 2 - button_w - help_w - ui.px(space::lg) * 2;
    const std::wstring description = running ? L"Ctrl+Alt+F does the same from anywhere."
                                             : L"Available while the workspace runs (Ctrl+Alt+F).";
    const int desc_h = wrapped_text_height(ui.c.dc(), ui.f.small, text_w, description);
    const int height = pad * 2 + std::max(ui.px(size::control), ui.px(24) + desc_h);
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    ui.c.text(ui.f.body_strong, ui.t.text, rect_of(left + pad, top + pad, text_w, ui.px(22)),
              L"Find the pointer");
    ui.c.text(ui.f.small, ui.t.text_dim, rect_of(left + pad, top + pad + ui.px(24), text_w, desc_h), description,
              DT_LEFT | DT_TOP | DT_WORDBREAK);
    const int by = top + (height - ui.px(size::control)) / 2;
    RECT b = rect_of(frame.right - pad - help_w - ui.px(space::md) - button_w, by, button_w, ui.px(size::control));
    button(ui, kUiCursorToCenter, b, L"Cursor to centre", ButtonStyle::Secondary, running, glyph::cursor);
    ui.help(kHelpEngine, kRowCursor, HelpTopic::ComfortCursorCenter,
            rect_of(b.right + ui.px(space::md), by + (ui.px(size::control) - help_w) / 2, help_w, help_w));
    return frame.bottom;
}

// --- Tracking ----------------------------------------------------------------

int tracking_card_head(Ui& ui, int left, int top, int width) {
    AppState& s = ui.s;
    const int pad = ui.px(space::xl);
    const int inner_w = width - pad * 2;
    const bool busy = s.engine.busy();
    const EngineSnapshot& engine = s.engine.snapshot();
    const bool yaw_on = engine.query_answered ? (engine.query_flags & kEngineFlagYawTracking) != 0 : true;
    const bool pitch_on = engine.query_answered ? (engine.query_flags & kEngineFlagPitchTracking) != 0 : true;
    const std::wstring live_note = busy ? L"" : L" Available while the engine runs.";
    struct Row {
        int id;
        const wchar_t* title;
        std::wstring description;
        bool value;
        bool enabled;
        int help_row;
        HelpTopic topic;
    };
    const Row rows[] = {
        {kUiToggleYaw, L"Yaw tracking", L"Looking left and right pans the view (Ctrl+Alt+Y)." + live_note, yaw_on,
         busy, kRowYaw, HelpTopic::EngineYawToggle},
        {kUiTogglePitch, L"Pitch tracking", L"Looking up and down moves the view (Ctrl+Alt+P)." + live_note,
         pitch_on, busy, kRowPitch, HelpTopic::EnginePitchToggle},
        {kUiPreviewWithoutHeadTracking, L"Preview without head tracking",
         L"Start preview pins the camera still, for arranging screens at your desk (--no-imu).",
         s.config.preview_without_head_tracking, !busy, kRowNoImu, HelpTopic::EngineNoImu},
        {kUiMagTrackHardIron, L"Earbud magnet compensation (experimental)",
         L"Keeps the compass steady when earbuds go in or out. Untested on glasses. Applies at the next "
         L"start.",
         s.config.engine.mag_track_hard_iron, true, kRowMagTrack, HelpTopic::EngineMagTrackHardIron},
    };
    const std::wstring predict_desc =
        L"Shows the world slightly ahead during fast head turns. Raise it if screens trail a quick turn, "
        L"lower it if they overshoot. Applies at the next start.";
    // Measure.
    const int text_w = inner_w - ui.px(size::switch_w) - help_mark_width(ui.dpi) - ui.px(space::lg) * 2;
    int rows_h = 0;
    for (const Row& row : rows) {
        rows_h += std::max(ui.px(44), ui.px(24) + wrapped_text_height(ui.c.dc(), ui.f.small, text_w, row.description)) +
                  ui.px(space::lg) * 2 + 1;
    }
    const int predict_desc_h = wrapped_text_height(ui.c.dc(), ui.f.small, text_w, predict_desc);
    const int predict_h = ui.px(24) + predict_desc_h + ui.px(space::md) + ui.px(size::control) +
                          ui.px(space::lg) * 2 + 1;
    const std::wstring lock_detail = heading_lock_detail(s.heading_lock);
    const int lock_h = ui.px(24) + wrapped_text_height(ui.c.dc(), ui.f.small, text_w, lock_detail);
    const int height =
        pad + ui.px(30) + ui.px(space::sm) + rows_h + predict_h + std::max(ui.px(44), lock_h) + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    ui.c.text(ui.f.title, ui.t.text, rect_of(left + pad, top + pad, inner_w, ui.px(28)), L"Head tracking");
    int y = top + pad + ui.px(30) + ui.px(space::sm);
    for (const Row& row : rows) {
        y += ui.px(space::lg);
        y += switch_row(ui, row.id, left + pad, y, inner_w, row.title, row.description, row.value, row.enabled,
                        kHelpEngine, row.help_row, row.topic);
        y += ui.px(space::lg);
        ui.c.fill(rect_of(left + pad, y, inner_w, 1), ui.t.border);
        y += 1;
    }
    // Turn prediction (--predict-ms): Off / 20 / 35 / 50 ms.
    {
        const int help_w = help_mark_width(ui.dpi);
        y += ui.px(space::lg);
        ui.c.text(ui.f.body_strong, ui.t.text, rect_of(left + pad, y, text_w, ui.px(22)), L"Turn prediction");
        ui.c.text(ui.f.small, ui.t.text_dim, rect_of(left + pad, y + ui.px(24), text_w, predict_desc_h), predict_desc,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
        y += ui.px(24) + predict_desc_h + ui.px(space::md);
        static const wchar_t* const kNames[kPredictMsChoiceCount] = {L"Off", L"20 ms", L"35 ms", L"50 ms"};
        RECT seg = rect_of(left + pad, y, std::min(inner_w - help_w - ui.px(space::md), ui.px(420)),
                           ui.px(size::control));
        segmented(ui, kUiPredictBase, seg, kNames, kPredictMsChoiceCount,
                  predict_ms_choice_index(s.config.engine.predict_ms));
        ui.help(kHelpEngine, kRowPrediction, HelpTopic::EngineMotionPrediction,
                rect_of(seg.right + ui.px(space::md), y + (rh(seg) - help_w) / 2, help_w, help_w));
        y += rh(seg) + ui.px(space::lg);
        ui.c.fill(rect_of(left + pad, y, inner_w, 1), ui.t.border);
        y += 1;
    }
    // Magnetic heading lock status (reported by the engine).
    y += ui.px(space::lg);
    COLORREF dot = ui.t.text_faint;
    switch (s.heading_lock) {
        case HeadingLockState::Locked:
            dot = ui.t.ok;
            break;
        case HeadingLockState::Disturbed:
            dot = ui.t.warning;
            break;
        case HeadingLockState::Relearning:
        case HeadingLockState::Acquiring:
            dot = ui.t.accent;
            break;
        case HeadingLockState::Off:
        case HeadingLockState::Unknown:
        default:
            break;
    }
    ui.c.text(ui.f.body_strong, ui.t.text, rect_of(left + pad, y, text_w, ui.px(22)), L"Magnetic heading lock");
    ui.c.text(ui.f.small, ui.t.text_dim, rect_of(left + pad, y + ui.px(24), text_w, lock_h - ui.px(24)), lock_detail,
              DT_LEFT | DT_TOP | DT_WORDBREAK);
    const int help_w = help_mark_width(ui.dpi);
    const std::wstring lock_label = heading_lock_label(s.heading_lock);
    const int badge_w = text_width(ui.c.dc(), ui.f.small_strong, lock_label) + ui.px(40);
    RECT badge = rect_of(frame.right - pad - help_w - ui.px(space::lg) - badge_w, y + ui.px(4), badge_w, ui.px(28));
    draw_badge(ui.c, badge, L"", dot, ui.f.small_strong);
    draw_status_dot(ui.c, badge.left + ui.px(14), (badge.top + badge.bottom) / 2, ui.px(4), dot,
                    pulse_phase(ui, s.heading_lock == HeadingLockState::Relearning ||
                                        s.heading_lock == HeadingLockState::Acquiring));
    ui.c.text(ui.f.small_strong, dot == ui.t.text_faint ? ui.t.text_dim : dot,
              RECT{badge.left + ui.px(26), badge.top, badge.right - ui.px(10), badge.bottom}, lock_label);
    ui.help(kHelpEngine, kRowHeadingLock, HelpTopic::HeadingLockStatus,
            rect_of(frame.right - pad - help_w, y + ui.px(4) + (ui.px(28) - help_w) / 2, help_w, help_w));
    return frame.bottom;
}

int tracking_card_calibration(Ui& ui, int left, int top, int width, CalFlow flow) {
    AppState& s = ui.s;
    const int pad = ui.px(space::xl);
    const int inner_w = width - pad * 2;
    const bool mag = flow == CalFlow::Mag;
    const DiagnosticsReport& report = s.diagnostics;
    const bool tool = s.paths.calibration_tool_found;
    const bool busy = s.engine.busy();
    const bool run_here = s.cal_run.phase != CalRunState::Phase::Idle && s.cal_run.flow == flow && !s.cal_in_wizard;
    const bool run_active = s.cal_run.running();
    const wchar_t* title = mag ? L"Compass calibration" : L"Head calibration";
    std::wstring description =
        mag ? L"Gives the glasses a magnetic reference so the centre screen cannot slowly drift sideways. "
              L"Optional; do it wearing what you normally wear, earbuds included, away from magnets."
            : L"Teaches the app how the sensor sits on your head. Wear the glasses, sit upright and follow "
              L"the prompts (each movement three times); it takes about a minute and a half.";
    std::wstring status;
    Severity severity = Severity::Neutral;
    if (!mag) {
        status = report.calibration_valid ? L"Saved and valid" : L"Not calibrated yet";
        severity = report.calibration_valid ? Severity::Ok : Severity::Warning;
    } else {
        status = report.calibration_valid ? L"Optional" : L"Needs a head calibration first";
        severity = report.calibration_valid ? Severity::Neutral : Severity::Warning;
    }
    std::wstring blocker;
    if (!tool) {
        blocker = L"orientation_calibrate.exe was not found next to this app.";
    } else if (busy) {
        blocker = L"Stop the engine first: calibration needs the glasses' sensor to itself.";
    } else if (mag && !report.calibration_valid) {
        blocker = L"Run the head calibration first.";
    }
    const int desc_h = wrapped_text_height(ui.c.dc(), ui.f.small, inner_w, description);
    const int blocker_h = blocker.empty() ? 0 : ui.px(space::md) + wrapped_text_height(ui.c.dc(), ui.f.small, inner_w, blocker);
    const int run_h = run_here ? ui.px(space::lg) + paint_calibration_run(ui, left + pad, 0, inner_w, false) : 0;
    const std::wstring earbuds_desc = L"Turn on if you wear in-ear earbuds for this calibration.";
    const int earbuds_text_w = inner_w - ui.px(size::switch_w) - help_mark_width(ui.dpi) - ui.px(space::lg) * 2;
    const int earbuds_h =
        mag ? std::max(ui.px(44), ui.px(24) + wrapped_text_height(ui.c.dc(), ui.f.small, earbuds_text_w, earbuds_desc)) +
                  ui.px(space::lg)
            : 0;
    const int height =
        pad + ui.px(30) + desc_h + ui.px(space::lg) + earbuds_h + ui.px(size::control) + blocker_h + run_h + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    ui.c.text(ui.f.title, ui.t.text, rect_of(left + pad, top + pad, inner_w, ui.px(28)), title);
    const int badge_w = text_width(ui.c.dc(), ui.f.small_strong, status) + ui.px(24);
    draw_badge(ui.c, rect_of(frame.right - pad - badge_w, top + pad + ui.px(2), badge_w, ui.px(24)), status,
               severity == Severity::Neutral ? ui.t.text_dim : severity_colour(ui.t, severity), ui.f.small_strong);
    int y = top + pad + ui.px(30);
    ui.c.text(ui.f.small, ui.t.text_dim, rect_of(left + pad, y, inner_w, desc_h), description,
              DT_LEFT | DT_TOP | DT_WORDBREAK);
    y += desc_h + ui.px(space::lg);
    if (mag) {
        switch_row(ui, kUiCalEarbuds, left + pad, y, inner_w, L"Earbuds in", earbuds_desc, s.cal_earbuds, !run_active,
                   kHelpTools, kRowCalEarbuds, HelpTopic::CalibrationEarbuds);
        y += earbuds_h;
    }
    const int help_w = help_mark_width(ui.dpi);
    const bool can_start = tool && !busy && !run_active && (!mag || report.calibration_valid);
    int x = left + pad;
    RECT start = rect_of(x, y, ui.px(196), ui.px(size::control));
    button(ui, mag ? kUiCalMagStart : kUiCalHeadStart, start, mag ? L"Calibrate compass" : L"Calibrate now",
           report.calibration_valid && !mag ? ButtonStyle::Secondary : ButtonStyle::Primary, can_start,
           mag ? glyph::compass : glyph::tracking);
    ui.help(kHelpTools, mag ? kRowCalMag : kRowCalHead, mag ? HelpTopic::CalibrationMag : HelpTopic::CalibrationHeadInApp,
            rect_of(start.right + ui.px(space::sm), y + (ui.px(size::control) - help_w) / 2, help_w, help_w));
    x = start.right + ui.px(space::sm) + help_w + ui.px(space::lg);
    RECT console = rect_of(x, y, ui.px(170), ui.px(size::control));
    button(ui, mag ? kUiCalMagConsole : kUiLaunchCalibration, console, L"Open in console", ButtonStyle::Ghost,
           tool && !run_active && (!mag || report.calibration_valid), glyph::document);
    if (!mag) {
        ui.help(kHelpTools, kRowCalConsole, HelpTopic::DiagCalibration,
                rect_of(console.right + ui.px(space::sm), y + (ui.px(size::control) - help_w) / 2, help_w, help_w));
    }
    y += ui.px(size::control);
    if (!blocker.empty()) {
        y += ui.px(space::md);
        y += paragraph(ui, rect_of(left + pad, y, inner_w, 0), ui.f.small, ui.t.warning, blocker);
    }
    if (run_here) {
        y += ui.px(space::lg);
        paint_calibration_run(ui, left + pad, y, inner_w, true);
    }
    return frame.bottom;
}

// --- Diagnostics -----------------------------------------------------------

int diagnostics_card_health(Ui& ui, int left, int top, int width) {
    const std::vector<HealthItem> items = health_items(ui.s);
    const int pad = ui.px(space::xl);
    const int row_h = ui.px(62);
    const int height = pad + ui.px(30) + ui.px(space::sm) + static_cast<int>(items.size()) * row_h + pad -
                       ui.px(space::sm);
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    ui.c.text(ui.f.title, ui.t.text, rect_of(left + pad, top + pad, width - pad * 2, ui.px(28)), L"Health checks");
    int y = top + pad + ui.px(30) + ui.px(space::sm);
    const int help_w = help_mark_width(ui.dpi);
    const bool wide = width >= ui.px(720);
    const int label_w = wide ? ui.px(210) : ui.px(160);
    for (const HealthItem& item : items) {
        if (&item != &items.front()) {
            ui.c.fill(rect_of(left + pad, y, width - pad * 2, 1), ui.t.border);
        }
        const COLORREF color = severity_colour(ui.t, item.severity);
        draw_dot(ui.c, left + pad + ui.px(5), y + ui.px(21), ui.px(5), color);
        const int text_left = left + pad + ui.px(22);
        ui.c.text(ui.f.body_strong, ui.t.text, rect_of(text_left, y + ui.px(10), label_w, ui.px(22)), item.title);
        const int value_left = text_left + label_w;
        const int value_w = left + width - pad - help_w - ui.px(space::md) - value_left;
        ui.c.text(ui.f.body, item.severity == Severity::Neutral ? ui.t.text_dim : color,
                  rect_of(value_left, y + ui.px(10), value_w, ui.px(22)), item.value,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        ui.c.text(ui.f.small, ui.t.text_faint,
                  rect_of(text_left, y + ui.px(34), left + width - pad - help_w - ui.px(space::md) - text_left, ui.px(18)),
                  item.raw.empty() ? item.detail : item.raw,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                      (item.topic == HelpTopic::StatusEngineLog ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS));
        ui.help(kHelpStatus, item.status_row, item.topic,
                rect_of(left + width - pad - help_w, y + ui.px(11), help_w, help_w));
        y += row_h;
    }
    return frame.bottom;
}

int diagnostics_card_tools(Ui& ui, int left, int top, int width) {
    const int pad = ui.px(space::xl);
    const int help_w = help_mark_width(ui.dpi);
    const int inner_w = width - pad * 2;
    struct Tool {
        int id;
        const wchar_t* label;
        const wchar_t* icon;
        int help_row;
        HelpTopic topic;
    };
    const Tool tools[] = {
        {kUiRecoverDisplays, L"Recover displays", glyph::glasses, kRowRecover, HelpTopic::DiagRecoverDisplays},
        {kUiOpenEngineLog, L"Open engine log", glyph::document, kRowEngineLog, HelpTopic::DiagEngineLog},
        {kUiOpenLogs, L"Open logs folder", glyph::folder, kRowLogsFolder, HelpTopic::DiagLogsFolder},
    };
    const int item_w = ui.px(196) + help_w + ui.px(space::sm);
    const int per_row = std::max(1, (inner_w + ui.px(space::lg)) / (item_w + ui.px(space::lg)));
    const int rows = (3 + per_row - 1) / per_row;
    const int height = pad + ui.px(30) + ui.px(space::md) + rows * (ui.px(size::control) + ui.px(space::md)) -
                       ui.px(space::md) + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    ui.c.text(ui.f.title, ui.t.text, rect_of(left + pad, top + pad, inner_w, ui.px(28)), L"Recovery and logs");
    const int y0 = top + pad + ui.px(30) + ui.px(space::md);
    for (int i = 0; i < 3; ++i) {
        const int x = left + pad + (i % per_row) * (item_w + ui.px(space::lg));
        const int y = y0 + (i / per_row) * (ui.px(size::control) + ui.px(space::md));
        RECT r = rect_of(x, y, ui.px(196), ui.px(size::control));
        button(ui, tools[i].id, r, tools[i].label, ButtonStyle::Secondary, true, tools[i].icon);
        ui.help(kHelpTools, tools[i].help_row, tools[i].topic,
                rect_of(r.right + ui.px(space::sm), y + (ui.px(size::control) - help_w) / 2, help_w, help_w));
    }
    return frame.bottom;
}

int diagnostics_card_lines(Ui& ui, int left, int top, int width, const wchar_t* title,
                           const std::vector<std::wstring>& lines, HFONT font, const wchar_t* empty_text) {
    const int pad = ui.px(space::xl);
    const int line_h = ui.px(22);
    const int body_h = lines.empty() ? ui.px(96) : static_cast<int>(lines.size()) * line_h;
    const int height = pad + ui.px(30) + ui.px(space::md) + body_h + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    ui.c.text(ui.f.title, ui.t.text, rect_of(left + pad, top + pad, width - pad * 2, ui.px(28)), title);
    int y = top + pad + ui.px(30) + ui.px(space::md);
    if (lines.empty()) {
        empty_state(ui, left, y, width, glyph::info, empty_text);
        return frame.bottom;
    }
    for (const std::wstring& line : lines) {
        ui.c.text(font, ui.t.text_dim, rect_of(left + pad, y, width - pad * 2, line_h), line,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += line_h;
    }
    return frame.bottom;
}

// --- Settings -------------------------------------------------------------

int settings_card_appearance(Ui& ui, int left, int top, int width) {
    const int pad = ui.px(space::xl);
    const int help_w = help_mark_width(ui.dpi);
    const int height = pad + ui.px(30) + ui.px(24) + ui.px(space::lg) + ui.px(size::control) + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    int y = card_header(ui, frame, L"Appearance", L"Choose how the controller looks.");
    static const wchar_t* const kNames[] = {L"Dark", L"Light", L"Match Windows"};
    RECT seg = rect_of(left + pad, y, std::min(width - pad * 2 - help_w - ui.px(space::md), ui.px(440)),
                       ui.px(size::control));
    segmented(ui, kUiThemeBase, seg, kNames, kThemeModeCount, static_cast<int>(ui.s.config.theme));
    ui.help(kHelpTools, kRowTheme, HelpTopic::SettingsTheme,
            rect_of(seg.right + ui.px(space::md), y + (rh(seg) - help_w) / 2, help_w, help_w));
    return frame.bottom;
}

int settings_card_window(Ui& ui, int left, int top, int width) {
    const int pad = ui.px(space::xl);
    const int inner_w = width - pad * 2;
    const std::wstring description =
        L"Closing the window keeps RayNeo Spatial in the notification area, so a running workspace "
        L"stays up. Turn off to quit on close.";
    const int text_w = inner_w - ui.px(size::switch_w) - help_mark_width(ui.dpi) - ui.px(space::lg) * 2;
    const int row_h = std::max(ui.px(44), ui.px(24) + wrapped_text_height(ui.c.dc(), ui.f.small, text_w, description));
    RECT frame = rect_of(left, top, width, pad * 2 + row_h);
    card(ui, frame);
    switch_row(ui, kUiCloseToTray, left + pad, top + pad, inner_w, L"Keep running in the notification area",
               description, ui.s.config.close_to_tray, true, kHelpTools, kRowCloseToTray, HelpTopic::DiagCloseToTray);
    return frame.bottom;
}

int settings_card_guide(Ui& ui, int left, int top, int width) {
    const int pad = ui.px(space::xl);
    const int help_w = help_mark_width(ui.dpi);
    const int button_w = ui.px(196);
    const int text_w = width - pad * 2 - button_w - help_w - ui.px(space::lg) * 2;
    const std::wstring description =
        L"Check the glasses, calibrate and pick a starting layout again. Nothing starts by itself.";
    const int desc_h = wrapped_text_height(ui.c.dc(), ui.f.small, text_w, description);
    const int height = pad * 2 + std::max(ui.px(size::control), ui.px(24) + desc_h);
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    ui.c.text(ui.f.body_strong, ui.t.text, rect_of(left + pad, top + pad, text_w, ui.px(22)), L"Setup guide");
    ui.c.text(ui.f.small, ui.t.text_dim, rect_of(left + pad, top + pad + ui.px(24), text_w, desc_h), description,
              DT_LEFT | DT_TOP | DT_WORDBREAK);
    const int by = top + (height - ui.px(size::control)) / 2;
    RECT b = rect_of(frame.right - pad - help_w - ui.px(space::md) - button_w, by, button_w, ui.px(size::control));
    button(ui, kUiRunSetupGuide, b, L"Run setup guide", ButtonStyle::Secondary, true, glyph::sparkle);
    ui.help(kHelpTools, kRowSetupGuide, HelpTopic::SettingsSetupGuide,
            rect_of(b.right + ui.px(space::md), by + (ui.px(size::control) - help_w) / 2, help_w, help_w));
    return frame.bottom;
}

int settings_card_shortcuts(Ui& ui, int left, int top, int width) {
    struct Shortcut {
        const wchar_t* keys;
        const wchar_t* action;
    };
    static const Shortcut kShortcuts[] = {
        // Engine hotkeys (spatial_desk.cpp register_global_hotkeys), then
        // this window's keys (AppWindow::on_key_down / on_mouse_wheel).
        {L"Ctrl+Shift+R", L"Recenter head tracking"},
        {L"Ctrl+Alt+Y / P", L"Toggle yaw / pitch tracking"},
        {L"Ctrl+Alt+S", L"Cycle reading stabilisation"},
        {L"Ctrl+Alt+F", L"Cursor to the centre screen"},
        {L"Ctrl+Alt+Q", L"Exit the workspace (or Ctrl+Shift+\\)"},
        {L"Ctrl+1 ... 6", L"Switch pages in this window"},
        {L"Tab / arrows", L"Move focus / adjust the focused control"},
        {L"Space / Enter", L"Press the focused control"},
        {L"PgUp / PgDn", L"Bigger steps; Home / End jump to the ends"},
        {L"Mouse wheel", L"Scroll; zoom in the 3D arrangement view"},
        {L"Esc", L"Leave the fullscreen editor, or close the window"},
        {L"Ctrl+S", L"Save and apply the layout"},
    };
    const int pad = ui.px(space::xl);
    const int row_h = ui.px(30);
    const int columns = width >= ui.px(760) ? 2 : 1;
    const int count = static_cast<int>(std::size(kShortcuts));
    const int rows = (count + columns - 1) / columns;
    const int height = pad + ui.px(30) + ui.px(space::md) + rows * row_h + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    ui.c.text(ui.f.title, ui.t.text, rect_of(left + pad, top + pad, width - pad * 2, ui.px(28)), L"Keyboard shortcuts");
    const int y0 = top + pad + ui.px(30) + ui.px(space::md);
    const int col_w = (width - pad * 2) / columns;
    for (int i = 0; i < count; ++i) {
        const int column = i / rows;
        const int row = i % rows;
        const int x = left + pad + column * col_w;
        const int y = y0 + row * row_h;
        const int key_w = text_width(ui.c.dc(), ui.f.small_strong, kShortcuts[i].keys) + ui.px(16);
        RECT key = rect_of(x, y + ui.px(3), key_w, ui.px(24));
        ui.c.fill_round(key, ui.px(radius::sm), ui.t.surface_alt);
        ui.c.outline_round(key, ui.px(radius::sm), 1.0f, ui.t.border);
        ui.c.text(ui.f.small_strong, ui.t.text, key, kShortcuts[i].keys, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ui.c.text(ui.f.small, ui.t.text_dim, rect_of(x + std::max(key_w, ui.px(118)) + ui.px(space::md), y,
                                                      col_w - std::max(key_w, ui.px(118)) - ui.px(space::lg), row_h),
                  kShortcuts[i].action, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    return frame.bottom;
}

int settings_card_about(Ui& ui, int left, int top, int width) {
    const AppState& s = ui.s;
    const int pad = ui.px(space::xl);
    const int line_h = ui.px(22);
    const int height = pad + ui.px(44) + ui.px(space::lg) + line_h * 3 + ui.px(space::lg) + ui.px(size::control) + pad;
    RECT frame = rect_of(left, top, width, height);
    card(ui, frame);
    const int logo = ui.px(44);
    RECT plate = rect_of(left + pad, top + pad, logo, logo);
    ui.c.fill_round(plate, logo * 28 / 100, ui.t.accent);
    draw_icon(ui.c, plate, glyph::glasses, ui.f.icon, ui.t.accent_text);
    const int text_left = plate.right + ui.px(space::lg);
    ui.c.text(ui.f.title, ui.t.text, rect_of(text_left, top + pad, width - (text_left - left) - pad, ui.px(24)),
              L"RayNeo Spatial");
    ui.c.text(ui.f.small, ui.t.text_dim, rect_of(text_left, top + pad + ui.px(24), width - (text_left - left) - pad, ui.px(20)),
              std::wstring(L"Version ") + kAppVersion + L"  ·  workspace controller for RayNeo GT glasses");
    int y = top + pad + ui.px(44) + ui.px(space::lg);
    const auto info = [&](const wchar_t* label, const std::wstring& value) {
        ui.c.text(ui.f.small, ui.t.text_faint, rect_of(left + pad, y, ui.px(150), line_h), label);
        ui.c.text(ui.f.small, ui.t.text_dim, rect_of(left + pad + ui.px(150), y, width - pad * 2 - ui.px(150), line_h),
                  value, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        y += line_h;
    };
    info(L"Data folder", s.paths.root.wstring());
    info(L"Render engine", s.paths.engine_found ? s.paths.engine_executable.wstring() : std::wstring(L"not found"));
    info(L"Calibration tool",
         s.paths.calibration_tool_found ? s.paths.calibration_executable.wstring() : std::wstring(L"not found"));
    y += ui.px(space::lg);
    RECT quit = rect_of(left + pad, y, ui.px(200), ui.px(size::control));
    button(ui, kUiQuit, quit, L"Quit RayNeo Spatial", ButtonStyle::Danger, true, glyph::close);
    return frame.bottom;
}

}  // namespace

// --- Shared calibration progress block --------------------------------------

int paint_calibration_run(Ui& ui, int left, int top, int width, bool draw) {
    const CalRunState& run = ui.s.cal_run;
    const Theme& t = ui.t;
    int y = top;
    const auto text_block = [&](HFONT font, COLORREF color, const std::wstring& text) {
        const int h = wrapped_text_height(ui.c.dc(), font, width, text);
        if (draw) {
            ui.c.text(font, color, rect_of(left, y, width, h), text, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        y += h;
    };
    const int box_pad = ui.px(space::lg);
    // Contained in a tinted panel so it reads as one live unit.
    const int inner_left = left + box_pad;
    const int inner_w = width - box_pad * 2;
    const int saved_left = left;
    left = inner_left;
    width = inner_w;
    y += box_pad;
    const int box_top = top;
    if (run.running()) {
        const float phase = draw ? pulse_phase(ui, true) : -1.0f;
        const std::wstring step_text =
            run.steps > 0 ? format_wide(L"Step %d of %d", std::max(1, run.step), run.steps) : L"Starting";
        const double countdown = run.countdown_s(ui.s.now_s);
        const std::wstring countdown_text =
            countdown > 0.0 ? format_wide(L"Starts in %d s", static_cast<int>(std::ceil(countdown)))
                            : std::wstring(L"Starting now");
        const wchar_t* state_text = L"Starting";
        COLORREF state_color = t.text_dim;
        if (run.has_event) {
            switch (run.state) {
                case CalStepState::Waiting:
                    state_text = countdown >= 0.0 ? countdown_text.c_str() : L"Waiting for you";
                    state_color = t.accent;
                    break;
                case CalStepState::Recording:
                    state_text = L"Recording";
                    state_color = t.warning;
                    break;
                case CalStepState::Ok:
                    state_text = L"Step done";
                    state_color = t.ok;
                    break;
                case CalStepState::Retry:
                    state_text = L"Let's try that again";
                    state_color = t.warning;
                    break;
                case CalStepState::Failed:
                case CalStepState::Done:
                default:
                    break;
            }
        }
        if (draw) {
            draw_status_dot(ui.c, left + ui.px(5), y + ui.px(11), ui.px(5), state_color, phase);
            ui.c.text(ui.f.small_strong, t.text_dim, rect_of(left + ui.px(18), y, width / 2, ui.px(22)), step_text);
            ui.c.text(ui.f.small_strong, state_color, rect_of(left + width / 2, y, width / 2, ui.px(22)), state_text,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        y += ui.px(26);
        if (draw) {
            const float shown = ui.track(anim::kProgress, 1, run.fraction(), anim::kTauProgress);
            draw_progress_bar(ui.c, rect_of(left, y, width, ui.px(8)), shown, t.accent);
        }
        y += ui.px(8) + ui.px(space::lg);
        const std::wstring prompt = run.has_event && !run.prompt.empty()
                                        ? wide_from_utf8(run.prompt)
                                        : std::wstring(L"Starting the calibration tool. Put the glasses on.");
        text_block(ui.f.title, t.text, prompt);
        // Live guide (tools that stream @@live): the same picture the glasses
        // show, smaller. Older tools keep the text-only view.
        if (run.tool_has_live && run.flow == CalFlow::Orientation &&
            (run.state == CalStepState::Waiting || run.state == CalStepState::Recording)) {
            y += ui.px(space::md);
            const int guide_h = std::clamp(width * 9 / 20, ui.px(170), ui.px(250));
            if (draw) {
                CalGuideView view = cal_guide_view(run, ui.s.now_s);
                if (run.state == CalStepState::Waiting) {
                    view.subline.clear();  // the prompt above already says how
                }
                const RECT guide = rect_of(left, y, width, guide_h);
                ui.c.push_clip(guide);
                const float fov = ui.s.layout.fov_deg > 5.0f ? ui.s.layout.fov_deg : kCalGuideDefaultFovDeg;
                paint_cal_guide(ui.c, guide, view, panel_guide_style(t, ui.f), fov);
                ui.c.pop_clip();
            }
            y += guide_h;
        }
        if (!run.retry_reason.empty() && run.state != CalStepState::Ok) {
            y += ui.px(space::xs);
            text_block(ui.f.body, t.warning, L"Again: " + wide_from_utf8(run.retry_reason));
        } else if (!run.detail.empty() && run.state != CalStepState::Waiting) {
            y += ui.px(space::xs);
            text_block(ui.f.body, t.text_dim, wide_from_utf8(cal_reason_text(run.detail)));
        }
        if (run.quality >= 0.0f) {
            y += ui.px(space::md);
            const COLORREF qc = run.quality >= 0.7f ? t.ok : run.quality >= 0.4f ? t.warning : t.error;
            if (draw) {
                ui.c.text(ui.f.caption, t.text_faint, rect_of(left, y, ui.px(110), ui.px(18)), L"Signal quality");
                draw_progress_bar(ui.c, rect_of(left + ui.px(116), y + ui.px(6), std::min(ui.px(200), width - ui.px(170)), ui.px(6)),
                                  run.quality, qc);
                ui.c.text(ui.f.caption, qc, rect_of(left + ui.px(116) + std::min(ui.px(200), width - ui.px(170)) + ui.px(8), y,
                                                    ui.px(50), ui.px(18)),
                          format_wide(L"%d%%", static_cast<int>(std::lround(run.quality * 100.0f))));
            }
            y += ui.px(18);
        }
        y += ui.px(space::lg);
        if (draw) {
            int x = left;
            if (run.awaiting_user()) {
                // --auto: the step starts by itself; this only skips the wait.
                RECT ready = rect_of(x, y, ui.px(150), ui.px(size::control));
                button(ui, kUiCalContinue, ready, run.auto_advance ? L"Start now" : L"I'm ready",
                       ButtonStyle::Primary, true, glyph::play);
                x = ready.right + ui.px(space::md);
            }
            RECT cancel = rect_of(x, y, ui.px(120), ui.px(size::control));
            button(ui, kUiCalCancel, cancel, L"Cancel", ButtonStyle::Secondary, true);
        }
        y += ui.px(size::control);
    } else if (run.phase == CalRunState::Phase::Succeeded) {
        if (draw) {
            // Saved either way; a low final score warns instead of ticking.
            status_icon(ui, left + ui.px(14), y + ui.px(14),
                        run.quality >= 0.0f && run.quality < 0.4f ? Severity::Warning : Severity::Ok);
            ui.c.text(ui.f.title, t.text, rect_of(left + ui.px(40), y, width - ui.px(40), ui.px(28)),
                      run.flow == CalFlow::Mag ? L"Compass calibrated" : L"Calibration saved");
        }
        y += ui.px(30);
        // The tool's final score in plain words (same bands as the bar above).
        const char* band = gt::calibration_quality_label(run.quality);
        if (band[0] != '\0') {
            const COLORREF qc = run.quality >= 0.7f ? t.ok : run.quality >= 0.4f ? t.warning : t.error;
            if (draw) {
                ui.c.text(ui.f.small_strong, qc, rect_of(left, y, width, ui.px(20)),
                          L"Calibration quality: " + wide_from_utf8(band), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            y += ui.px(20) + ui.px(space::xs);
        }
        // Plain words here; the tool's raw metrics go to the Diagnostics activity log.
        text_block(ui.f.body, t.text_dim, wide_from_utf8(calibration_saved_text(run.flow, run.quality, run.detail)));
        if (!ui.s.cal_in_wizard) {
            y += ui.px(space::lg);
            if (draw) {
                button(ui, kUiCalDismiss, rect_of(left, y, ui.px(120), ui.px(size::control)), L"Done",
                       ButtonStyle::Secondary);
            }
            y += ui.px(size::control);
        }
    } else if (run.phase == CalRunState::Phase::Failed || run.phase == CalRunState::Phase::Cancelled) {
        const bool cancelled = run.phase == CalRunState::Phase::Cancelled;
        if (draw) {
            status_icon(ui, left + ui.px(14), y + ui.px(14), cancelled ? Severity::Warning : Severity::Error);
            ui.c.text(ui.f.title, t.text, rect_of(left + ui.px(40), y, width - ui.px(40), ui.px(28)),
                      cancelled ? L"Calibration cancelled" : L"Calibration did not finish");
        }
        y += ui.px(30);
        text_block(ui.f.body, t.text_dim,
                   cancelled ? std::wstring(L"Nothing was changed. Try again when you are ready.")
                             : wide_from_utf8(run.failure) + L" Nothing was changed.");
        if (!cancelled && !run.recent_output.empty()) {
            y += ui.px(space::sm);
            const size_t first = run.recent_output.size() > 3 ? run.recent_output.size() - 3 : 0;
            for (size_t i = first; i < run.recent_output.size(); ++i) {
                if (draw) {
                    ui.c.text(ui.f.mono, t.text_faint, rect_of(left, y, width, ui.px(18)),
                              wide_from_utf8(run.recent_output[i]), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                y += ui.px(18);
            }
        }
        y += ui.px(space::lg);
        if (draw) {
            RECT retry = rect_of(left, y, ui.px(140), ui.px(size::control));
            button(ui, kUiCalRetry, retry, L"Try again", ButtonStyle::Primary, !ui.s.engine.busy(), glyph::refresh);
            button(ui, kUiCalDismiss, rect_of(retry.right + ui.px(space::md), y, ui.px(120), ui.px(size::control)),
                   L"Dismiss", ButtonStyle::Ghost);
        }
        y += ui.px(size::control);
    }
    y += box_pad;
    if (draw) {
        // The tint goes under the content: content was painted already, so
        // draw the frame as an outline only (text stays crisp on the card).
        RECT box = rect_of(saved_left, box_top, inner_w + box_pad * 2, y - box_top);
        ui.c.outline_round(box, ui.px(radius::md), 1.0f, t.border_strong);
    }
    return y - top;
}

// --- Pages -------------------------------------------------------------------

void paint_home(Ui& ui, const RECT& body) {
    scroll_page(ui, body, [&](int left, int top, int width) {
        int y = paint_hero(ui, left, top, width);
        y += ui.px(space::xl);
        if (width >= ui.px(880)) {
            const int gap = ui.px(space::xl);
            const int left_w = (width - gap) * 57 / 100;
            const int right_w = width - gap - left_w;
            const int left_bottom = paint_readiness(ui, left, y, left_w);
            int right_y = paint_layout_glance(ui, left + left_w + gap, y, right_w);
            right_y = paint_live_tracking(ui, left + left_w + gap, right_y + gap, right_w);
            return std::max(left_bottom, right_y);
        }
        y = paint_readiness(ui, left, y, width) + ui.px(space::xl);
        y = paint_layout_glance(ui, left, y, width) + ui.px(space::xl);
        return paint_live_tracking(ui, left, y, width);
    });
}

void paint_comfort(Ui& ui, const RECT& body) {
    scroll_page(ui, body, [&](int left, int top, int width) {
        const int gap = ui.px(space::xl);
        const int column_w = std::min(width, ui.px(880));
        int y = comfort_card_stabilise(ui, left, top, column_w) + gap;
        y = comfort_card_dimming(ui, left, y, column_w) + gap;
        y = comfort_card_tint(ui, left, y, column_w) + gap;
        return comfort_card_cursor(ui, left, y, column_w);
    });
}

void paint_tracking(Ui& ui, const RECT& body) {
    scroll_page(ui, body, [&](int left, int top, int width) {
        const int gap = ui.px(space::xl);
        const int column_w = std::min(width, ui.px(880));
        int y = tracking_card_head(ui, left, top, column_w) + gap;
        y = tracking_card_calibration(ui, left, y, column_w, CalFlow::Orientation) + gap;
        return tracking_card_calibration(ui, left, y, column_w, CalFlow::Mag);
    });
}

void paint_diagnostics(Ui& ui, const RECT& body) {
    scroll_page(ui, body, [&](int left, int top, int width) {
        const int gap = ui.px(space::xl);
        int y = diagnostics_card_health(ui, left, top, width) + gap;
        y = diagnostics_card_tools(ui, left, y, width) + gap;
        std::vector<std::wstring> events;
        for (auto it = ui.s.events.rbegin(); it != ui.s.events.rend() && events.size() < 14; ++it) {
            events.push_back(*it);
        }
        std::vector<std::wstring> log_lines;
        {
            std::wistringstream stream(ui.s.diagnostics.engine_log_tail);
            std::wstring line;
            std::vector<std::wstring> all;
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == L'\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    all.push_back(line);
                }
            }
            const size_t first = all.size() > 10 ? all.size() - 10 : 0;
            log_lines.assign(all.begin() + static_cast<std::ptrdiff_t>(first), all.end());
        }
        if (width >= ui.px(980)) {
            const int half = (width - gap) / 2;
            const int a = diagnostics_card_lines(ui, left, y, half, L"Activity (newest first)", events, ui.f.small,
                                                 L"No controller events yet.");
            const int b = diagnostics_card_lines(ui, left + half + gap, y, width - half - gap, L"Engine log (latest lines)",
                                                 log_lines, ui.f.mono, L"The engine has not written a log yet.");
            return std::max(a, b);
        }
        y = diagnostics_card_lines(ui, left, y, width, L"Activity (newest first)", events, ui.f.small,
                                   L"No controller events yet.") +
            gap;
        return diagnostics_card_lines(ui, left, y, width, L"Engine log (latest lines)", log_lines, ui.f.mono,
                                      L"The engine has not written a log yet.");
    });
}

void paint_settings(Ui& ui, const RECT& body) {
    scroll_page(ui, body, [&](int left, int top, int width) {
        const int gap = ui.px(space::xl);
        const int column_w = std::min(width, ui.px(880));
        int y = settings_card_appearance(ui, left, top, column_w) + gap;
        y = settings_card_window(ui, left, y, column_w) + gap;
        y = settings_card_guide(ui, left, y, column_w) + gap;
        y = settings_card_shortcuts(ui, left, y, column_w) + gap;
        return settings_card_about(ui, left, y, column_w);
    });
}

}  // namespace gt::ui::detail
