// First-run setup guide overlay. Modal: while it shows, only its own
// hotspots are interactive. It never starts the engine and never touches
// displays; calibration runs only on an explicit click.

#include "app_shell/ui_paint_internal.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace gt::ui::detail {
namespace {

int check_row(Ui& ui, int left, int top, int width, Severity severity, const wchar_t* title,
              const std::wstring& detail) {
    const int text_left = left + ui.px(44);
    const int text_w = width - ui.px(44);
    const int detail_h = wrapped_text_height(ui.c.dc(), ui.f.small, text_w, detail);
    const int h = std::max(ui.px(48), ui.px(24) + detail_h + ui.px(space::sm));
    const COLORREF color = severity_colour(ui.t, severity);
    ui.c.fill_circle(left + ui.px(14), top + ui.px(14), static_cast<float>(ui.px(14)),
                     mix(ui.t.surface, color, ui.t.dark ? 0.2f : 0.14f));
    draw_icon(ui.c, rect_of(left, top, ui.px(28), ui.px(28)), severity_glyph(severity), ui.f.icon_small, color);
    ui.c.text(ui.f.body_strong, ui.t.text, rect_of(text_left, top + ui.px(2), text_w, ui.px(22)), title);
    ui.c.text(ui.f.small, ui.t.text_dim, rect_of(text_left, top + ui.px(24), text_w, detail_h), detail,
              DT_LEFT | DT_TOP | DT_WORDBREAK);
    return h;
}

int feature_row(Ui& ui, int left, int top, int width, const wchar_t* icon, const wchar_t* title,
                const wchar_t* detail) {
    const int text_left = left + ui.px(52);
    const int text_w = width - ui.px(52);
    const int detail_h = wrapped_text_height(ui.c.dc(), ui.f.small, text_w, detail);
    RECT plate = rect_of(left, top, ui.px(36), ui.px(36));
    ui.c.fill_round(plate, ui.px(radius::md), ui.t.accent_soft);
    draw_icon(ui.c, plate, icon, ui.f.icon, ui.t.accent);
    ui.c.text(ui.f.body_strong, ui.t.text, rect_of(text_left, top, text_w, ui.px(20)), title);
    ui.c.text(ui.f.small, ui.t.text_dim, rect_of(text_left, top + ui.px(22), text_w, detail_h), detail,
              DT_LEFT | DT_TOP | DT_WORDBREAK);
    return std::max(ui.px(40), ui.px(22) + detail_h);
}

int body_welcome(Ui& ui, int left, int top, int width) {
    int y = top;
    y += paragraph(ui, rect_of(left, y, width, 0), ui.f.body, ui.t.text_dim,
                   L"RayNeo Spatial turns your RayNeo GT glasses into a private multi-monitor desk: "
                   L"real Windows screens float around you and stay put as you turn your head.");
    y += ui.px(space::xl);
    RECT preview = rect_of(left, y, width, ui.px(150));
    ui.c.fill_round(preview, ui.px(radius::lg), ui.t.canvas);
    paint_arc_preview(ui, preview, preset_layout(LayoutPreset::TripleArc), 1);
    y += ui.px(150) + ui.px(space::xl);
    y += feature_row(ui, left, y, width, glyph::glasses, L"Check the glasses",
                     L"A quick, read-only look at the display, sensor and driver.") +
         ui.px(space::lg);
    y += feature_row(ui, left, y, width, glyph::tracking, L"Calibrate head tracking",
                     L"About a minute of guided head movements, then an optional compass step.") +
         ui.px(space::lg);
    y += feature_row(ui, left, y, width, glyph::layout, L"Pick a layout",
                     L"Start with one, three or more screens; fine-tune any time on the Layout page.") +
         ui.px(space::lg);
    y += paragraph(ui, rect_of(left, y, width, 0), ui.f.small, ui.t.text_faint,
                   L"Nothing on your displays changes during this guide. You start the workspace yourself "
                   L"from Home when you are ready.");
    return y - top;
}

int body_glasses(Ui& ui, int left, int top, int width) {
    const DiagnosticsReport& report = ui.s.diagnostics;
    int y = top;
    y += paragraph(ui, rect_of(left, y, width, 0), ui.f.body, ui.t.text_dim,
                   L"Plug the glasses into a USB-C port that carries video. These checks only look; they "
                   L"change nothing.");
    y += ui.px(space::xl);
    y += check_row(ui, left, y, width, report.glasses_found ? Severity::Ok : Severity::Error,
                   L"Glasses display",
                   report.glasses_found ? L"Found as a Windows display."
                                        : L"Not found. Check the cable and that the glasses are switched on.") +
         ui.px(space::md);
    y += check_row(ui, left, y, width, report.glasses_found ? Severity::Ok : Severity::Warning,
                   L"Extend mode",
                   report.glasses_found
                       ? L"The glasses are a separate display, as they should be."
                       : L"Press Win+P and choose Extend. In Duplicate or PC-screen-only mode the glasses "
                         L"are invisible to the app.") +
         ui.px(space::md);
    y += check_row(ui, left, y, width, report.hid_device_count > 0 ? Severity::Ok : Severity::Error,
                   L"Head-tracking sensor",
                   report.hid_device_count > 0
                       ? L"The glasses' motion sensor answered over USB."
                       : L"No answer from the motion sensor. Reconnect the cable; try another USB-C port.") +
         ui.px(space::md);
    y += check_row(ui, left, y, width, report.vdd_ready ? Severity::Ok : Severity::Warning,
                   L"Virtual display driver",
                   report.vdd_ready
                       ? L"Parsec VDD is ready, so the full workspace can create real screens."
                       : L"Parsec VDD is " + report.vdd_status_text +
                             L". You can still use preview; install the signed Parsec VDD driver for the full "
                             L"workspace.") +
         ui.px(space::lg);
    RECT recheck = rect_of(left, y, ui.px(160), ui.px(size::control));
    button(ui, kUiWizardRecheck, recheck, L"Check again", ButtonStyle::Secondary, true, glyph::refresh);
    y += ui.px(size::control);
    return y - top;
}

int body_calibration(Ui& ui, int left, int top, int width, CalFlow flow) {
    AppState& s = ui.s;
    const DiagnosticsReport& report = s.diagnostics;
    const bool mag = flow == CalFlow::Mag;
    int y = top;
    if (mag) {
        y += paragraph(ui, rect_of(left, y, width, 0), ui.f.body, ui.t.text_dim,
                       L"The compass calibration reduces slow sideways drift by giving the glasses a magnetic "
                       L"reference. Do it with your earbuds in if you wear them, and away from magnets, "
                       L"speakers and metal desk frames. You can skip it and do it later.");
    } else {
        y += paragraph(ui, rect_of(left, y, width, 0), ui.f.body, ui.t.text_dim,
                       L"Put the glasses on and sit upright facing your desk. The guide asks you to hold still, "
                       L"turn, nod and tilt; each step starts by itself after a short countdown, so you never "
                       L"need the keyboard. It takes about a minute and only replaces the saved calibration if "
                       L"it succeeds.");
    }
    y += ui.px(space::lg);
    std::wstring note;
    Severity note_severity = Severity::Neutral;
    if (!s.paths.calibration_tool_found) {
        note = L"orientation_calibrate.exe was not found next to this app, so calibration cannot run here.";
        note_severity = Severity::Error;
    } else if (s.engine.busy()) {
        note = L"The engine is running. Stop it from Home first: calibration needs the sensor to itself.";
        note_severity = Severity::Warning;
    } else if (report.hid_device_count <= 0) {
        note = L"The glasses' motion sensor is not answering. Connect the glasses before starting.";
        note_severity = Severity::Warning;
    } else if (mag && !report.calibration_valid) {
        note = L"Do the head calibration first; the compass step builds on it.";
        note_severity = Severity::Warning;
    } else if (!mag && report.calibration_valid) {
        note = L"A valid calibration is already saved. Keep it and continue, or redo it if nodding tilts the screens.";
        note_severity = Severity::Ok;
    }
    if (!note.empty()) {
        y += check_row(ui, left, y, width, note_severity, mag ? L"Before you start" : L"Status", note) +
             ui.px(space::lg);
    }
    const bool this_flow = s.cal_run.phase != CalRunState::Phase::Idle && s.cal_run.flow == flow && s.cal_in_wizard;
    if (this_flow) {
        y += paint_calibration_run(ui, left, y, width, true);
        return y - top;
    }
    const bool can_start = s.paths.calibration_tool_found && !s.engine.busy() && !s.cal_run.running() &&
                           (!mag || report.calibration_valid);
    if (mag) {
        y += switch_row(ui, kUiCalEarbuds, left, y, std::min(width, ui.px(620)), L"Earbuds in",
                        L"Turn on if you are wearing in-ear earbuds now, the way you normally work.",
                        s.cal_earbuds, !s.cal_run.running(), kHelpTools, kRowCalEarbuds,
                        HelpTopic::CalibrationEarbuds) +
             ui.px(space::lg);
    }
    RECT start = rect_of(left, y, ui.px(230), ui.px(size::control_lg));
    button(ui, mag ? kUiCalMagStart : kUiCalHeadStart, start, mag ? L"Start compass calibration" : L"Start calibration",
           ButtonStyle::Primary, can_start, mag ? glyph::compass : glyph::play);
    y += ui.px(size::control_lg);
    return y - top;
}

int body_layout(Ui& ui, int left, int top, int width) {
    AppState& s = ui.s;
    int y = top;
    y += paragraph(ui, rect_of(left, y, width, 0), ui.f.body, ui.t.text_dim,
                   L"Pick a starting arrangement. It is saved when you finish the guide; you can fine-tune "
                   L"everything later on the Layout page.");
    y += ui.px(space::xl);
    const std::vector<LayoutPreset> presets = layout_presets();
    const int count = static_cast<int>(presets.size()) + 1;
    const int gap = ui.px(space::md);
    const int columns = width >= ui.px(620) ? 3 : 2;
    const int tile_w = (width - gap * (columns - 1)) / columns;
    const int tile_h = ui.px(132);
    for (int i = 0; i < count; ++i) {
        const int column = i % columns;
        const int row = i / columns;
        RECT tile = rect_of(left + column * (tile_w + gap), y + row * (tile_h + gap), tile_w, tile_h);
        const bool keep = i == 0;
        const int id = keep ? kUiWizardPresetKeep : kUiWizardPresetBase + static_cast<int>(presets[static_cast<size_t>(i - 1)]);
        const bool selected = keep ? s.onboarding.preset_choice < 0
                                   : s.onboarding.preset_choice == static_cast<int>(presets[static_cast<size_t>(i - 1)]);
        const WidgetState state = ui.ws(id);
        ui.c.fill_round(tile, ui.px(radius::lg), selected ? ui.t.accent_soft : mix(ui.t.surface_alt, ui.t.surface_hover, state.hover));
        ui.c.outline_round(tile, ui.px(radius::lg), static_cast<float>(selected ? ui.px(2) : 1),
                           selected ? ui.t.accent : ui.t.border);
        RECT preview = rect_of(tile.left + ui.px(space::md), tile.top + ui.px(space::md), tile_w - ui.px(space::md) * 2,
                               tile_h - ui.px(52));
        const Layout layout = keep ? s.layout : preset_layout(presets[static_cast<size_t>(i - 1)]);
        paint_arc_preview(ui, preview, layout, -1);
        const std::wstring name = keep ? std::wstring(L"Keep my current layout")
                                       : wide_from_utf8(layout_preset_label(presets[static_cast<size_t>(i - 1)]));
        ui.c.text(selected ? ui.f.small_strong : ui.f.small, selected ? ui.t.text : ui.t.text_dim,
                  rect_of(tile.left + ui.px(space::md), tile.bottom - ui.px(36), tile_w - ui.px(space::md) * 2, ui.px(24)),
                  name, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (state.focused) {
            draw_focus_ring(ui.c, tile, ui.px(radius::lg));
        }
        ui.hot(id, i, tile);
    }
    const int rows = (count + columns - 1) / columns;
    y += rows * tile_h + (rows - 1) * gap;
    return y - top;
}

int body_done(Ui& ui, int left, int top, int width) {
    const AppState& s = ui.s;
    int y = top;
    y += paragraph(ui, rect_of(left, y, width, 0), ui.f.body, ui.t.text_dim,
                   L"When you are ready, put the glasses on and press Start workspace on the Home page. "
                   L"Recenter any time the centre screen is not straight ahead.");
    y += ui.px(space::xl);
    const auto outcome_text = [](StepOutcome outcome, bool valid_now) -> std::wstring {
        if (outcome == StepOutcome::Completed) {
            return L"Done.";
        }
        if (valid_now) {
            return L"Skipped; the saved calibration is kept.";
        }
        return L"Skipped. You can run it from the Tracking page.";
    };
    const DiagnosticsReport& report = s.diagnostics;
    y += check_row(ui, left, y, width,
                   s.onboarding.head == StepOutcome::Completed || report.calibration_valid ? Severity::Ok
                                                                                          : Severity::Warning,
                   L"Head calibration", outcome_text(s.onboarding.head, report.calibration_valid)) +
         ui.px(space::md);
    y += check_row(ui, left, y, width, s.onboarding.mag == StepOutcome::Completed ? Severity::Ok : Severity::Neutral,
                   L"Compass calibration",
                   s.onboarding.mag == StepOutcome::Completed ? std::wstring(L"Done.")
                                                              : std::wstring(L"Skipped. Optional; available on the Tracking page.")) +
         ui.px(space::md);
    const std::wstring layout_text =
        s.onboarding.preset_choice < 0
            ? std::wstring(L"Your current layout is kept.")
            : L"'" + wide_from_utf8(layout_preset_label(static_cast<LayoutPreset>(s.onboarding.preset_choice))) +
                  L"' will be saved when you press Finish.";
    y += check_row(ui, left, y, width, Severity::Ok, L"Layout", layout_text);
    return y - top;
}

void paint_stepper(Ui& ui, int left, int top, int width) {
    const int steps = kOnboardingStepCount;
    const int current = ui.s.onboarding.index();
    const int r = ui.px(12);
    const bool labels = width >= ui.px(600);
    const int span = width - r * 2;
    const float progress = ui.track(anim::kProgress, 2, static_cast<float>(current), anim::kTauSlide);
    for (int i = 0; i < steps - 1; ++i) {
        const int x0 = left + r + span * i / (steps - 1);
        const int x1 = left + r + span * (i + 1) / (steps - 1);
        ui.c.line(static_cast<float>(x0 + r + ui.px(4)), static_cast<float>(top + r), static_cast<float>(x1 - r - ui.px(4)),
                  static_cast<float>(top + r), static_cast<float>(ui.px(2)), ui.t.border_strong);
        const float fill = std::clamp(progress - static_cast<float>(i), 0.0f, 1.0f);
        if (fill > 0.0f) {
            const float end = static_cast<float>(x0 + r + ui.px(4)) +
                              (static_cast<float>(x1 - r - ui.px(4)) - static_cast<float>(x0 + r + ui.px(4))) * fill;
            ui.c.line(static_cast<float>(x0 + r + ui.px(4)), static_cast<float>(top + r), end, static_cast<float>(top + r),
                      static_cast<float>(ui.px(2)), ui.t.accent);
        }
    }
    for (int i = 0; i < steps; ++i) {
        const int cx = left + r + span * i / (steps - 1);
        const int cy = top + r;
        RECT circle = rect_of(cx - r, cy - r, r * 2, r * 2);
        if (i < current) {
            ui.c.fill_circle(cx, cy, static_cast<float>(r), ui.t.accent);
            draw_icon(ui.c, circle, glyph::check, ui.f.icon_small, ui.t.accent_text);
        } else if (i == current) {
            ui.c.fill_circle(cx, cy, static_cast<float>(r), ui.t.accent_soft);
            ui.c.ring(cx, cy, static_cast<float>(r) - 1.0f, static_cast<float>(ui.px(2)), ui.t.accent);
            ui.c.text(ui.f.small_strong, ui.t.accent, circle, std::to_wstring(i + 1), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            ui.c.ring(cx, cy, static_cast<float>(r) - 1.0f, 1.5f, ui.t.border_strong);
            ui.c.text(ui.f.small, ui.t.text_faint, circle, std::to_wstring(i + 1), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (labels || i == current) {
            const auto step = static_cast<OnboardingStep>(i);
            // Centred under its circle; the first and last labels stop at
            // the stepper's edges, so they line up with their circle's
            // outer edge instead of drifting into the next gap.
            HFONT label_font = i == current ? ui.f.small_strong : ui.f.caption;
            const std::wstring text = onboarding_step_label(step);
            const int label_w = text_width(ui.c.dc(), label_font, text);
            int label_left = cx - label_w / 2;
            label_left = std::clamp(label_left, left, std::max(left, left + width - label_w));
            ui.c.text(label_font, i == current ? ui.t.text : ui.t.text_faint,
                      rect_of(label_left, cy + r + ui.px(6), label_w + 1, ui.px(18)), text,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

}  // namespace

void paint_wizard(Ui& ui, const RECT& client) {
    AppState& s = ui.s;
    const Theme& t = ui.t;
    ui.c.fill_round(client, 0, t.scrim, t.dark ? 170 : 120);
    const int margin = ui.px(space::xl);
    const int width = std::min(ui.px(860), rw(client) - margin * 2);
    const int height = std::min(ui.px(680), rh(client) - margin * 2);
    RECT frame = rect_of(client.left + (rw(client) - width) / 2, client.top + (rh(client) - height) / 2, width, height);
    ui.c.shadow(frame, ui.px(16), ui.px(10));
    ui.c.fill_round(frame, ui.px(16), t.surface);
    ui.c.outline_round(frame, ui.px(16), 1.0f, t.border);

    const int pad = width >= ui.px(640) ? ui.px(40) : ui.px(space::xl);
    const int inner_w = width - pad * 2;
    paint_stepper(ui, frame.left + pad, frame.top + pad, inner_w);
    int y = frame.top + pad + ui.px(56);
    ui.c.text(ui.f.display, t.text, rect_of(frame.left + pad, y, inner_w, ui.px(40)),
              onboarding_step_title(s.onboarding.step), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += ui.px(40) + ui.px(space::md);

    const int footer_h = ui.px(size::control) + pad;
    RECT view{frame.left + pad - ui.px(space::sm), y, frame.right - pad + ui.px(space::sm) - ui.px(space::xs),
              frame.bottom - footer_h - ui.px(space::md)};
    ScrollArea area = begin_scroll(ui, kScrollWizard, frame, view);
    const int content_top = y - area.offset;
    int content_h = 0;
    switch (s.onboarding.step) {
        case OnboardingStep::Welcome:
            content_h = body_welcome(ui, frame.left + pad, content_top, inner_w);
            break;
        case OnboardingStep::GlassesCheck:
            content_h = body_glasses(ui, frame.left + pad, content_top, inner_w);
            break;
        case OnboardingStep::HeadCalibration:
            content_h = body_calibration(ui, frame.left + pad, content_top, inner_w, CalFlow::Orientation);
            break;
        case OnboardingStep::MagCalibration:
            content_h = body_calibration(ui, frame.left + pad, content_top, inner_w, CalFlow::Mag);
            break;
        case OnboardingStep::LayoutPreset:
            content_h = body_layout(ui, frame.left + pad, content_top, inner_w);
            break;
        case OnboardingStep::Done:
        case OnboardingStep::Count:
        default:
            content_h = body_done(ui, frame.left + pad, content_top, inner_w);
            break;
    }
    end_scroll(ui, area, content_h + ui.px(space::md));

    // Footer.
    const int fy = frame.bottom - pad - ui.px(size::control);
    ui.c.fill(rect_of(frame.left, fy - ui.px(space::lg) - 1, width, 1), t.border);
    const bool running = s.cal_run.running();
    if (!s.onboarding.on_last_step()) {
        RECT skip_all = rect_of(frame.left + pad, fy, ui.px(132), ui.px(size::control));
        button(ui, kUiWizardSkipAll, skip_all, L"Skip setup", ButtonStyle::Ghost, !running);
    }
    int x = frame.right - pad;
    const wchar_t* next_label = L"Next";
    if (s.onboarding.step == OnboardingStep::Welcome) {
        next_label = L"Get started";
    } else if (s.onboarding.on_last_step()) {
        next_label = L"Finish";
    } else if ((s.onboarding.step == OnboardingStep::HeadCalibration && s.diagnostics.calibration_valid &&
                s.onboarding.head != StepOutcome::Completed)) {
        next_label = L"Keep and continue";
    }
    const int next_w = std::max(ui.px(150), text_width(ui.c.dc(), ui.f.body_strong, next_label) + ui.px(48));
    RECT next = rect_of(x - next_w, fy, next_w, ui.px(size::control));
    button(ui, kUiWizardNext, next, next_label, ButtonStyle::Primary, !running,
           s.onboarding.on_last_step() ? glyph::check : nullptr);
    x = next.left - ui.px(space::md);
    if (s.onboarding.step_skippable()) {
        RECT skip = rect_of(x - ui.px(110), fy, ui.px(110), ui.px(size::control));
        button(ui, kUiWizardSkipStep, skip, L"Skip", ButtonStyle::Ghost, !running);
        x = skip.left - ui.px(space::md);
    }
    if (s.onboarding.can_go_back()) {
        RECT back = rect_of(x - ui.px(110), fy, ui.px(110), ui.px(size::control));
        button(ui, kUiWizardBack, back, L"Back", ButtonStyle::Secondary, !running);
    }
}

}  // namespace gt::ui::detail
