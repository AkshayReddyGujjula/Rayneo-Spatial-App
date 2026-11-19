// ui_snapshot - renders the real controller UI offscreen.
//
// Paints paint_window() into a memory DC for a matrix of pages, window sizes,
// DPI scales and themes with synthetic (never hardware-derived) state, then
// checks the hotspot list for layout defects: every interactive element must
// lie inside the window, no two enabled elements may overlap, and every help
// mark must resolve to a help topic. With --out DIR it also writes each frame
// as a PNG so the result can be looked at.
//
// Nothing here creates a window, starts the engine, opens the glasses or
// touches a display: diagnostics are filled in by hand.

#include "app_shell/app_state.h"
#include "app_shell/calibration_guide.h"
#include "app_shell/ui_cal_guide.h"
#include "app_shell/ui_help.h"
#include "app_shell/ui_paint.h"
#include "app_shell/ui_paint_internal.h"
#include "app_shell/ui_widgets.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

namespace {

int failures = 0;

void fail(const std::string& shot, const std::string& what) {
    std::printf("  [FAIL] %s: %s\n", shot.c_str(), what.c_str());
    ++failures;
}

// WCAG 2.x relative luminance and contrast ratio of two opaque colours.
double relative_luminance(COLORREF color) {
    const auto channel = [](BYTE value) {
        const double c = static_cast<double>(value) / 255.0;
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(GetRValue(color)) + 0.7152 * channel(GetGValue(color)) +
           0.0722 * channel(GetBValue(color));
}

double contrast_ratio(COLORREF a, COLORREF b) {
    const double la = relative_luminance(a);
    const double lb = relative_luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// Every token pair the paint code draws text or a state-carrying UI part
// with must meet WCAG AA: 4.5:1 for text (the UI has no text large enough
// for the 3:1 large-text rule except titles, which use `text`), 3:1 for the
// focus ring. Status colours are also drawn as text on their own tinted
// badge, so that background is checked with draw_badge's tint factor.
void check_contrast() {
    struct Pair {
        const char* fg;
        COLORREF gt::ui::Theme::*fg_token;
        const char* bg;
        COLORREF gt::ui::Theme::*bg_token;
        double minimum;
    };
    using T = gt::ui::Theme;
    const Pair pairs[] = {
        {"text", &T::text, "window", &T::window, 4.5},
        {"text", &T::text, "surface_alt", &T::surface_alt, 4.5},
        {"text", &T::text, "accent_soft", &T::accent_soft, 4.5},
        {"text_dim", &T::text_dim, "window", &T::window, 4.5},
        {"text_dim", &T::text_dim, "sidebar", &T::sidebar, 4.5},
        {"text_dim", &T::text_dim, "surface", &T::surface, 4.5},
        {"text_dim", &T::text_dim, "surface_alt", &T::surface_alt, 4.5},
        {"text_faint", &T::text_faint, "window", &T::window, 4.5},
        {"text_faint", &T::text_faint, "sidebar", &T::sidebar, 4.5},
        {"text_faint", &T::text_faint, "surface", &T::surface, 4.5},
        {"text_faint", &T::text_faint, "surface_alt", &T::surface_alt, 4.5},
        {"text_faint", &T::text_faint, "canvas", &T::canvas, 4.5},
        {"accent", &T::accent, "window", &T::window, 4.5},
        {"accent", &T::accent, "surface", &T::surface, 4.5},
        {"accent", &T::accent, "accent_soft", &T::accent_soft, 4.5},
        {"accent_text", &T::accent_text, "accent_fill", &T::accent_fill, 4.5},
        {"accent_text", &T::accent_text, "accent_hover", &T::accent_hover, 4.5},
        {"ok", &T::ok, "window", &T::window, 4.5},
        {"ok", &T::ok, "surface", &T::surface, 4.5},
        {"warning", &T::warning, "window", &T::window, 4.5},
        {"warning", &T::warning, "surface", &T::surface, 4.5},
        {"error", &T::error, "window", &T::window, 4.5},
        {"error", &T::error, "surface", &T::surface, 4.5},
        {"focus", &T::focus, "window", &T::window, 3.0},
        {"focus", &T::focus, "surface", &T::surface, 3.0},
    };
    const struct {
        const char* name;
        const gt::ui::Theme& theme;
    } themes[] = {{"dark", gt::ui::dark_theme()}, {"light", gt::ui::light_theme()}};
    std::printf("ui_snapshot: WCAG contrast of theme token pairs\n");
    for (const auto& entry : themes) {
        const gt::ui::Theme& t = entry.theme;
        const auto report = [&](const std::string& label, COLORREF fg, COLORREF bg, double minimum) {
            const double ratio = contrast_ratio(fg, bg);
            std::printf("  [%s] %-5s %-36s %5.2f (>= %.1f)\n", ratio >= minimum ? "ok" : "..", entry.name,
                        label.c_str(), ratio, minimum);
            if (ratio < minimum) {
                fail(std::string("contrast-") + entry.name, label + " is below WCAG AA");
            }
        };
        for (const Pair& pair : pairs) {
            report(std::string(pair.fg) + " on " + pair.bg, t.*pair.fg_token, t.*pair.bg_token, pair.minimum);
        }
        // Badge: the status colour as text on its own tint (draw_badge).
        const float tint = t.dark ? 0.18f : 0.12f;
        for (const auto& [name, color] : {std::pair<const char*, COLORREF>{"accent", t.accent},
                                          std::pair<const char*, COLORREF>{"ok", t.ok},
                                          std::pair<const char*, COLORREF>{"warning", t.warning},
                                          std::pair<const char*, COLORREF>{"error", t.error}}) {
            report(std::string(name) + " on its badge", color, gt::ui::mix(t.surface, color, tint), 4.5);
        }
    }
}

// Arrangement-editor screen labels keep the angle for the longest id the
// layout accepts (64 characters) and stay a readable length.
void check_editor_labels() {
    std::printf("ui_snapshot: arrangement editor labels\n");
    for (const std::string& id : {std::string("centre"), std::string(56, 'a'), std::string(64, 'w')}) {
        gt::ScreenLayout screen;
        screen.id = id;
        screen.yaw_deg = -45.0f;
        const std::wstring label = gt::ui::detail::editor_screen_label(screen);
        const std::wstring tail = L"-45°";
        const bool angle = label.size() >= tail.size() && label.compare(label.size() - tail.size(), tail.size(), tail) == 0;
        std::printf("  [%s] id of %zu chars -> label of %zu chars\n", angle && label.size() <= 32 ? "ok" : "..",
                    id.size(), label.size());
        if (!angle) {
            fail("editor-label", "a " + std::to_string(id.size()) + "-character id loses the angle");
        }
        if (label.size() > 32) {
            fail("editor-label", "a " + std::to_string(id.size()) + "-character id makes a " +
                                     std::to_string(label.size()) + "-character label");
        }
    }
}

bool save_png(HBITMAP bitmap, const std::filesystem::path& path) {
    // {557CF406-1A04-11D3-9A73-0000F81EF32E}: the built-in PNG encoder.
    const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    Gdiplus::Bitmap image(bitmap, nullptr);
    return image.Save(path.wstring().c_str(), &png, nullptr) == Gdiplus::Ok;
}

void base_state(gt::AppState& state, bool ready) {
    state.paths.root = L"C:\\Users\\you\\RayNeo-Spatial";
    state.paths.engine_executable = state.paths.root / "spatial_desk.exe";
    state.paths.calibration_executable = state.paths.root / "orientation_calibrate.exe";
    state.paths.engine_log = state.paths.root / "logs" / "engine.log";
    state.paths.log_dir = state.paths.root / "logs";
    state.paths.engine_found = true;
    state.paths.calibration_tool_found = true;
    state.layout = gt::preset_layout(gt::LayoutPreset::TripleArc);
    state.preset_names = {"triple", "ultrawide", "reading-desk"};
    state.loaded_preset = "triple";
    state.config.onboarding_complete = true;
    gt::DiagnosticsReport& d = state.diagnostics;
    d.glasses_found = ready;
    d.glasses_detail = ready ? L"TCL SmartGlasses 1920x1080 at (1920, 0), Extend mode" : L"";
    d.calibration_present = ready;
    d.calibration_valid = ready;
    d.calibration_detail = ready ? L"config/orientation.json: head-right X, forward Y, up Z (quality 0.97)"
                                 : L"config/orientation.json is missing";
    d.hid_device_count = ready ? 1 : 0;
    d.hid_detail = ready ? L"RayNeo GT IMU interface (usage page 0xFF00)" : L"no RayNeo GT HID interface";
    d.vdd_ready = ready;
    d.vdd_status_text = ready ? L"installed and ready" : L"not installed";
    d.layout_ok = true;
    d.layout_detail = L"config/layouts/default.json: 3 screens";
    d.telemetry_detail = L"";
    state.events = {L"controller ready; paths root: C:\\Users\\you\\RayNeo-Spatial",
                    L"archiving session logs left by a previous run",
                    L"preset 'triple' loaded"};
}

void running_state(gt::AppState& state) {
    gt::EngineSnapshot snapshot;
    snapshot.state = gt::EngineProcessState::Running;
    snapshot.process_alive = true;
    snapshot.owns_process = true;
    snapshot.process_id = 4242;
    snapshot.uptime_s = 1834.0;
    snapshot.query_answered = true;
    snapshot.query_flags = gt::kEngineFlagYawTracking | gt::kEngineFlagPitchTracking;
    snapshot.query_screens = 3;
    snapshot.mode = gt::LaunchMode::Workspace;
    state.engine.set_preview_snapshot(snapshot, true);
    state.engine_status.parsed = true;
    state.engine_status.state = "running";
    state.engine_status.mode = "workspace";
    state.engine_status.screens = 3;
    state.engine_status.fps = 60.0;
    state.engine_status.vdd = true;
    state.engine_status.imu = true;
    state.diagnostics.telemetry_present = true;
    state.diagnostics.telemetry.valid = true;
    state.diagnostics.telemetry.yaw_deg = -12.4f;
    state.diagnostics.telemetry.pitch_deg = 3.1f;
    state.diagnostics.telemetry.roll_deg = 0.4f;
    state.diagnostics.telemetry.rest = true;
    state.diagnostics.telemetry_detail = L"logs/telemetry.csv, newest row 0.4 s old";
    state.diagnostics.engine_log_tail =
        L"[engine] glasses display 1920x1080 found at (1920,0)\r\n[engine] 3 virtual displays attached\r\n"
        L"[engine] head tracking calibrated in 1.3 s, bias 0.21 deg/s\r\n[engine] 60.0 fps, 3 screens\r\n";
    state.heading_lock = gt::HeadingLockState::Locked;
}

// The engine mid-startup: the display takeover runs while the controller
// still says Starting (no engine window yet); the sensor phases run after the
// window exists, so the controller already says Running.
void startup_state(gt::AppState& state, const char* phase, double progress) {
    running_state(state);
    gt::EngineSnapshot snapshot = state.engine.snapshot();
    const bool takeover = std::string(phase) == gt::kPhaseTakeover;
    snapshot.state = takeover ? gt::EngineProcessState::Starting : gt::EngineProcessState::Running;
    snapshot.uptime_s = takeover ? 9.0 : 24.0;
    snapshot.query_answered = !takeover;
    state.engine.set_preview_snapshot(snapshot, true);
    state.engine_status.state = takeover ? "starting" : "ready";
    state.engine_status.fps = takeover ? 0.0 : 60.0;
    state.engine_status.phase = phase;
    state.engine_status.phase_progress = progress;
    state.heading_lock = gt::HeadingLockState::Unknown;
}

// Live calibration guide states, fed as the tool's own lines.
struct GuideCase {
    std::string name;
    std::function<void(gt::CalRunState&, double)> setup;
};

std::string guide_progress(int step, const char* state) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "@@progress {\"flow\":\"orientation\",\"step\":%d,\"steps\":4,\"state\":\"%s\",\"prompt\":\"%s\","
                  "\"detail\":\"\",\"quality\":-1}",
                  step, state,
                  step == 1   ? "Look straight ahead and hold completely still for 4 seconds."
                  : step == 2 ? "Slowly turn your head LEFT about 30 degrees, then return to centre. Do this 3 times."
                  : step == 3 ? "Slowly nod DOWN toward your chest, then return to centre. Do this 3 times."
                              : "Slowly tilt your head toward your RIGHT shoulder, then return to centre. Do this 3 "
                                "times.");
    return buffer;
}

std::string guide_live(int step, const char* kind, const char* state, float remaining, float yaw, float pitch,
                       float roll, float along, float off, float rms, float peak, int repeats, const char* hint) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "@@live {\"step\":%d,\"kind\":\"%s\",\"state\":\"%s\",\"elapsed\":2.00,\"remaining\":%.2f,"
                  "\"frame\":\"file\",\"yaw\":%.2f,\"pitch\":%.2f,\"roll\":%.2f,\"along\":%.2f,\"off\":%.2f,"
                  "\"speed\":18.0,\"still_rms\":%.2f,\"still_gate\":1.5,\"gate\":12,\"target\":%d,\"peak\":%.2f,"
                  "\"repeats\":%d,\"repeats_required\":%d,\"hint\":\"%s\"}",
                  step, kind, state, static_cast<double>(remaining), static_cast<double>(yaw),
                  static_cast<double>(pitch), static_cast<double>(roll), static_cast<double>(along),
                  static_cast<double>(off), static_cast<double>(rms), std::strcmp(kind, "turn") == 0 ? 25 : 20,
                  static_cast<double>(peak), repeats, std::strcmp(kind, "still") == 0 ? 0 : 3, hint);
    return buffer;
}

const std::vector<GuideCase>& guide_cases() {
    static const std::vector<GuideCase> cases = {
        {"still", [](gt::CalRunState& run, double now) {
             run.begin(gt::CalFlow::Orientation, true, 5.0);
             run.consume_line(guide_progress(1, "recording"), now - 1.0);
             run.consume_line(guide_live(1, "still", "recording", 1.6f, 0.3f, -0.2f, 0.1f, 0.0f, 0.37f, 0.62f, 0.0f,
                                         0, "STEADY"),
                              now);
         }},
        {"turn-on-axis", [](gt::CalRunState& run, double now) {
             run.begin(gt::CalFlow::Orientation, true, 5.0);
             run.consume_line(guide_progress(2, "recording"), now - 3.0);
             run.consume_line(guide_live(2, "turn", "recording", 8.4f, 19.0f, -0.6f, 0.4f, 19.0f, 0.72f, 0.4f, 19.0f,
                                         1, "FURTHER"),
                              now);
         }},
        {"turn-off-axis", [](gt::CalRunState& run, double now) {
             run.begin(gt::CalFlow::Orientation, true, 5.0);
             run.consume_line(guide_progress(2, "recording"), now - 3.0);
             run.consume_line(guide_live(2, "turn", "recording", 8.0f, 16.0f, -5.2f, 1.8f, 16.0f, 5.5f, 0.4f, 16.0f,
                                         0, "OFF_AXIS"),
                              now);
         }},
        {"nod", [](gt::CalRunState& run, double now) {
             run.begin(gt::CalFlow::Orientation, true, 5.0);
             run.consume_line(guide_progress(3, "recording"), now - 5.0);
             run.consume_line(guide_live(3, "nod", "recording", 6.0f, 0.5f, -21.0f, -0.3f, 21.0f, 0.6f, 0.4f, 21.0f, 2,
                                         "BACK_TO_CENTRE"),
                              now);
         }},
        {"tilt", [](gt::CalRunState& run, double now) {
             run.begin(gt::CalFlow::Orientation, true, 5.0);
             run.consume_line(guide_progress(4, "recording"), now - 2.0);
             run.consume_line(guide_live(4, "tilt", "recording", 10.0f, 0.8f, 0.4f, 14.0f, 14.0f, 0.9f, 0.4f, 14.0f, 0,
                                         "FURTHER"),
                              now);
         }},
        {"waiting-countdown", [](gt::CalRunState& run, double now) {
             run.begin(gt::CalFlow::Orientation, true, 5.0);
             run.consume_line(guide_progress(2, "waiting"), now - 1.6);
             run.consume_line(guide_live(2, "turn", "waiting", 3.4f, 0.4f, 0.2f, 0.0f, 0.4f, 0.2f, 0.3f, 0.0f, 0,
                                         "GET_READY"),
                              now);
         }},
        {"retry-tip", [](gt::CalRunState& run, double now) {
             run.begin(gt::CalFlow::Orientation, true, 5.0);
             run.consume_line(guide_progress(2, "recording"), now - 9.0);
             for (int i = 0; i < 12; ++i) {
                 run.consume_line(guide_live(2, "turn", "recording", 4.0f, 15.0f, -6.0f, 1.0f, 15.0f, 6.1f, 0.4f,
                                             15.0f, 1, "OFF_AXIS"),
                                  now - 3.0);
             }
             run.consume_line(
                 "@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"retry\",\"prompt\":\"x\","
                 "\"detail\":\"LOW_DOMINANCE: the turn was not a clean single-axis motion\",\"quality\":0}",
                 now - 2.0);
             run.consume_line(guide_progress(2, "waiting"), now - 1.0);
             run.consume_line(guide_live(2, "turn", "waiting", 4.0f, 0.2f, 0.1f, 0.0f, 0.2f, 0.1f, 0.3f, 0.0f, 0,
                                         "GET_READY"),
                              now);
         }},
    };
    return cases;
}

struct Shot {
    std::string name;
    int width;
    int height;
    UINT dpi;
    bool dark;
    std::function<void(gt::AppState&)> setup;
    // Optional pixel check on the finished frame (32-bit top-down BGRA).
    std::function<void(const std::string&, const gt::AppState&, const uint32_t*,
                       const std::vector<gt::ui::Hotspot>&)>
        verify;
};

// A slider's knob must be where the pointer maps that value: AppWindow
// turns a press at x into (x - rect.left) / width of the slider's hit rect.
// The knob's white centre dot is found on the track row and mapped back.
void check_slider_knob(const std::string& shot, int width, UINT dpi, const uint32_t* bits,
                       const std::vector<gt::ui::Hotspot>& hotspots, int id, float fraction) {
    const auto spot = std::find_if(hotspots.begin(), hotspots.end(),
                                   [id](const gt::ui::Hotspot& h) { return h.id == id; });
    if (spot == hotspots.end()) {
        fail(shot, "slider " + std::to_string(id) + " was not painted");
        return;
    }
    const RECT r = spot->full;
    const int label_h = gt::ui::scale_px(24, dpi);
    const int cy = r.top + label_h + (r.bottom - r.top - label_h) / 2;
    const int knob = gt::ui::scale_px(8, dpi);
    long sum = 0;
    int count = 0;
    for (int x = std::max(0, static_cast<int>(r.left) - knob * 2); x < std::min(width, static_cast<int>(r.right) + knob * 2);
         ++x) {
        const uint32_t value = bits[static_cast<size_t>(cy) * static_cast<size_t>(width) + static_cast<size_t>(x)];
        if (((value >> 16) & 0xff) > 245 && ((value >> 8) & 0xff) > 245 && (value & 0xff) > 245) {
            sum += x;
            ++count;
        }
    }
    if (count == 0) {
        fail(shot, "slider " + std::to_string(id) + ": no knob found on the track row");
        return;
    }
    const double centre = static_cast<double>(sum) / count + 0.5;
    const double mapped = (centre - r.left) / static_cast<double>(r.right - r.left);
    if (std::fabs(mapped - fraction) * (r.right - r.left) > 1.5) {
        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "slider %d: knob at %.1f px maps to %.3f, painted value %.3f", id,
                      centre - r.left, mapped, static_cast<double>(fraction));
        fail(shot, buffer);
    }
}

// The sidebar's engine card dot (full sidebar: 20 px in from the card's
// left edge, vertically centred in the 68 px card) must be drawn in want.
void check_status_dot(const std::string& shot, int width, int height, UINT dpi, const uint32_t* bits,
                      COLORREF want, const char* what) {
    using gt::ui::scale_px;
    const int x = scale_px(gt::ui::space::md, dpi) + scale_px(20, dpi);
    const int card_h = scale_px(68, dpi);
    const int y = height - scale_px(gt::ui::space::lg, dpi) - card_h + card_h / 2;
    const uint32_t value = bits[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
    const int r = static_cast<int>((value >> 16) & 0xff);
    const int g = static_cast<int>((value >> 8) & 0xff);
    const int b = static_cast<int>(value & 0xff);
    if (std::abs(r - GetRValue(want)) > 8 || std::abs(g - GetGValue(want)) > 8 || std::abs(b - GetBValue(want)) > 8) {
        fail(shot, std::string("sidebar engine dot is not ") + what + " (got " + std::to_string(r) + "," +
                       std::to_string(g) + "," + std::to_string(b) + ")");
    }
}

void render(const Shot& shot, const std::filesystem::path& out_dir) {
    gt::AppState state;
    shot.setup(state);
    state.dark = shot.dark;
    state.anim.set_reduce_motion(true);  // a snapshot shows settled values
    gt::ui::FontSet fonts;
    fonts.create(shot.dpi);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = shot.width;
    info.bmiHeader.biHeight = -shot.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (dc == nullptr || bitmap == nullptr) {
        fail(shot.name, "could not create the offscreen bitmap");
        return;
    }
    const HGDIOBJ previous = SelectObject(dc, bitmap);
    const RECT client{0, 0, shot.width, shot.height};
    gt::ui::PaintContext context;
    context.state = &state;
    context.fonts = &fonts;
    context.dpi = shot.dpi;
    context.client = client;
    std::vector<gt::ui::Hotspot> hotspots;
    // Paint until settled: the first pass measures scroll content and
    // registers animation targets.
    for (int pass = 0; pass < 3; ++pass) {
        gt::ui::Canvas canvas(dc, client, gt::ui::current_theme(state), shot.dpi);
        gt::ui::paint_window(canvas, context, hotspots);
        if (!state.repaint_requested && pass > 0) {
            break;
        }
    }
    GdiFlush();
    if (shot.verify && bits != nullptr) {
        shot.verify(shot.name, state, static_cast<const uint32_t*>(bits), hotspots);
    }

    // Layout checks.
    int interactive = 0;
    for (size_t i = 0; i < hotspots.size(); ++i) {
        const gt::ui::Hotspot& a = hotspots[i];
        if (a.rect.left < client.left || a.rect.top < client.top || a.rect.right > client.right ||
            a.rect.bottom > client.bottom || a.rect.right <= a.rect.left || a.rect.bottom <= a.rect.top) {
            fail(shot.name, "hotspot " + std::to_string(a.id) + " lies outside the window");
        }
        if (a.id >= gt::kUiHelpBase && a.id < gt::kUiHelpBase + 7 * 64 &&
            (a.arg < 0 || a.arg >= static_cast<int>(gt::HelpTopic::Count))) {
            fail(shot.name, "help mark " + std::to_string(a.id) + " has no topic");
        }
        if (!a.enabled) {
            continue;
        }
        ++interactive;
        for (size_t j = i + 1; j < hotspots.size(); ++j) {
            const gt::ui::Hotspot& b = hotspots[j];
            RECT overlap{};
            if (b.enabled && IntersectRect(&overlap, &a.rect, &b.rect) != 0) {
                fail(shot.name, "hotspots " + std::to_string(a.id) + " and " + std::to_string(b.id) +
                                    " overlap at (" + std::to_string(overlap.left) + "," +
                                    std::to_string(overlap.top) + ")");
            }
        }
        for (size_t j = i + 1; j < hotspots.size(); ++j) {
            if (hotspots[j].id == a.id && hotspots[j].enabled) {
                fail(shot.name, "hotspot id " + std::to_string(a.id) + " is registered twice");
            }
        }
    }
    if (interactive == 0) {
        fail(shot.name, "no interactive elements were painted");
    }
    if (state.page == gt::AppPage::Layout && !state.onboarding.active &&
        std::none_of(hotspots.begin(), hotspots.end(),
                     [](const gt::ui::Hotspot& h) { return h.id == gt::kUiEditorCanvas; })) {
        fail(shot.name, "the Layout page painted no arrangement editor");
    }
    // 2D editor labels: inside the canvas, clear of each other and of the
    // fixed captions (yaw ticks, "you"), and the selected screen's is shown.
    const auto canvas_spot = std::find_if(hotspots.begin(), hotspots.end(),
                                          [](const gt::ui::Hotspot& h) { return h.id == gt::kUiEditorCanvas; });
    if (state.page == gt::AppPage::Layout && !state.editor_3d && !state.onboarding.active &&
        canvas_spot != hotspots.end()) {
        const RECT canvas_rect = canvas_spot->full;
        const auto labels = gt::ui::detail::editor_2d_labels(dc, fonts, state.layout, state.selected_screen,
                                                             canvas_rect, shot.dpi);
        const auto captions = gt::ui::detail::editor_2d_captions(
            gt::ui::editor_geometry(state.layout, canvas_rect, shot.dpi), shot.dpi);
        bool selected_shown = state.selected_screen >= state.layout.screens.size();
        for (size_t i = 0; i < labels.size(); ++i) {
            const RECT& a = labels[i].rect;
            selected_shown = selected_shown || labels[i].selected;
            if (a.left < canvas_rect.left || a.right > canvas_rect.right || a.top < canvas_rect.top ||
                a.bottom > canvas_rect.bottom) {
                fail(shot.name, "editor label of screen " + std::to_string(labels[i].index) + " leaves the canvas");
            }
            RECT overlap{};
            for (size_t j = i + 1; j < labels.size(); ++j) {
                if (IntersectRect(&overlap, &a, &labels[j].rect) != 0) {
                    fail(shot.name, "editor labels of screens " + std::to_string(labels[i].index) + " and " +
                                        std::to_string(labels[j].index) + " overlap");
                }
            }
            for (const RECT& caption : captions) {
                if (!labels[i].selected && IntersectRect(&overlap, &a, &caption) != 0) {
                    fail(shot.name, "editor label of screen " + std::to_string(labels[i].index) +
                                        " covers a caption");
                }
            }
        }
        if (!selected_shown) {
            fail(shot.name, "the selected screen's editor label is missing");
        }
    }
    std::printf("  [%s] %s: %dx%d @%u%% %s, %d interactive\n", failures == 0 ? "ok" : "..",
                shot.name.c_str(), shot.width, shot.height, shot.dpi * 100 / 96, shot.dark ? "dark" : "light",
                interactive);
    if (!out_dir.empty()) {
        const std::filesystem::path file = out_dir / (shot.name + ".png");
        if (!save_png(bitmap, file)) {
            fail(shot.name, "could not write " + file.string());
        }
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    fonts.destroy();
}

std::vector<Shot> shots() {
    using gt::AppPage;
    using gt::AppState;
    const auto page = [](AppPage p, bool ready, bool running) {
        return [=](AppState& s) {
            base_state(s, ready);
            if (running) {
                running_state(s);
            }
            s.page = p;
        };
    };
    std::vector<Shot> list;
    list.push_back({"home-ready-1280x800", 1280, 800, 96, true, page(AppPage::Home, true, false)});
    list.push_back({"home-running-1920x1080-150", 1920, 1080, 144, true, page(AppPage::Home, true, true)});
    list.push_back({"home-setup-needed-1280x800-light", 1280, 800, 96, false, page(AppPage::Home, false, false)});
    list.push_back({"home-narrow-820x640", 820, 640, 96, true, page(AppPage::Home, true, true)});
    const auto starting = [](const char* phase, double progress) {
        return [=](AppState& s) {
            base_state(s, true);
            startup_state(s, phase, progress);
            s.page = AppPage::Home;
        };
    };
    list.push_back({"home-startup-takeover-1280x800", 1280, 800, 96, true, starting(gt::kPhaseTakeover, -1.0)});
    list.push_back({"home-startup-warmup-1280x800-light", 1280, 800, 96, false,
                    starting(gt::kPhaseImuWarmup, 0.35)});
    list.push_back({"home-startup-get-ready-1280x800", 1280, 800, 96, true, starting(gt::kPhaseImuWarmup, 0.8)});
    list.push_back({"home-startup-hold-still-1920x1080-150", 1920, 1080, 144, true,
                    starting(gt::kPhaseHoldStill, 0.55)});
    list.push_back({"home-startup-hold-still-1280x800-light", 1280, 800, 96, false,
                    starting(gt::kPhaseHoldStill, 0.0)});
    list.push_back({"home-startup-hold-still-narrow-820x640", 820, 640, 96, true,
                    starting(gt::kPhaseHoldStill, 0.3)});
    list.push_back({"layout-1280x800", 1280, 800, 96, true, [](AppState& s) {
                        base_state(s, true);
                        s.page = AppPage::Layout;
                        s.layout_dirty = true;
                        s.selected_screen = 1;
                    }});
    list.push_back({"layout-3d-1920x1080-150", 1920, 1080, 144, true, [](AppState& s) {
                        base_state(s, true);
                        s.page = AppPage::Layout;
                        s.editor_3d = true;
                    }});
    list.push_back({"layout-fullscreen-1280x800-light", 1280, 800, 96, false, [](AppState& s) {
                        base_state(s, true);
                        s.page = AppPage::Layout;
                        s.editor_fullscreen = true;
                        s.layout_dirty = true;
                    }});
    list.push_back({"layout-narrow-820x640", 820, 640, 96, true, page(AppPage::Layout, true, false)});
    // A portrait screen on the left (a monitor turned sideways for long
    // chats), selected so the inspector shows its Portrait switch on.
    const auto portrait_left = [](bool dark, bool three_d, bool fullscreen) {
        return [=](AppState& s) {
            base_state(s, dark);
            s.page = AppPage::Layout;
            std::string error;
            gt::set_screen_portrait(s.layout, s.layout.screens[0], true, error);
            s.selected_screen = 0;
            s.layout_dirty = true;
            s.editor_3d = three_d;
            s.editor_fullscreen = fullscreen;
        };
    };
    list.push_back({"layout-portrait-1280x800", 1280, 800, 96, true, portrait_left(true, false, false)});
    list.push_back({"layout-portrait-1280x800-light", 1280, 800, 96, false, portrait_left(false, false, false)});
    list.push_back({"layout-portrait-narrow-820x640", 820, 640, 96, true, portrait_left(true, false, false)});
    list.push_back({"layout-portrait-3d-1920x1080-150", 1920, 1080, 144, true, portrait_left(true, true, false)});
    list.push_back({"layout-portrait-fullscreen-3d-1280x800-light", 1280, 800, 96, false,
                    portrait_left(false, true, true)});
    list.push_back({"comfort-1280x800", 1280, 800, 96, true, [](AppState& s) {
                        base_state(s, true);
                        running_state(s);
                        s.page = AppPage::Comfort;
                        s.config.comfort.dim_mode = gt::DimMode::Manual;
                        s.config.comfort.night_tint = true;
                    }});
    list.push_back({"tracking-calibrating-1280x800", 1280, 800, 96, true, [](AppState& s) {
                        base_state(s, true);
                        s.page = AppPage::Tracking;
                        // Real tool shape (--auto): a retry, then the countdown.
                        s.cal_run.begin(gt::CalFlow::Orientation, true, 5.0);
                        s.cal_run.consume_line(
                            "@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"retry\","
                            "\"prompt\":\"x\",\"detail\":\"SMALL_EXCURSION: turn further left, about 30 degrees, "
                            "then back to centre\",\"quality\":0}",
                            s.now_s - 2.2);
                        s.cal_run.consume_line(
                            "@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"waiting\","
                            "\"prompt\":\"Slowly turn your head LEFT about 30 degrees, then return to centre. Do not "
                            "tilt or nod. Slow and smooth is fine.\",\"detail\":\"\",\"quality\":-1}",
                            s.now_s - 2.2);
                    }});
    // Finished calibrations: the tool's final quality as a plain-language band.
    list.push_back({"tracking-calibration-saved-good-1280x800", 1280, 800, 96, true, [](AppState& s) {
                        base_state(s, true);
                        s.page = AppPage::Tracking;
                        s.cal_run.begin(gt::CalFlow::Orientation, true, 5.0);
                        s.cal_run.consume_line(
                            "@@progress {\"flow\":\"orientation\",\"step\":4,\"steps\":4,\"state\":\"done\","
                            "\"prompt\":\"Orientation calibration saved.\",\"detail\":\"up_vs_yaw 0.84 deg, "
                            "nod_level 1.10 deg, nod_tilt 1.92 deg, handedness 1.35 deg; 3 repeats: spread 1.05 "
                            "deg, estimated axis error 0.74 deg\",\"quality\":0.87}");
                        s.cal_run.process_exited(0);
                        s.scroll[gt::kScrollPage].offset = 2000;
                        s.scroll[gt::kScrollPage].max_offset = 2000;  // clamped by the paint pass
                    }});
    list.push_back({"tracking-compass-earbuds-1280x800", 1280, 800, 96, true, [](AppState& s) {
                        base_state(s, true);
                        s.page = AppPage::Tracking;
                        s.cal_earbuds = true;
                        s.config.engine.mag_track_hard_iron = true;
                        s.scroll[gt::kScrollPage].offset = 2000;
                        s.scroll[gt::kScrollPage].max_offset = 2000;  // clamped by the paint pass
                        s.cal_run.begin(gt::CalFlow::Mag, true, 5.0);
                        s.cal_run.consume_line(
                            "@@progress {\"flow\":\"mag\",\"step\":2,\"steps\":2,\"state\":\"recording\","
                            "\"prompt\":\"Now nod up and down, tilt ear to shoulder both ways, and look far left, far "
                            "right, up and down until the timer ends.\",\"detail\":\"15 of 45 s\",\"quality\":-1}",
                            s.now_s);
                    }});
    list.push_back({"tracking-running-1920x1080-150-light", 1920, 1080, 144, false,
                    page(AppPage::Tracking, true, true)});
    list.push_back({"diagnostics-1280x800", 1280, 800, 96, true, page(AppPage::Diagnostics, true, true)});
    list.push_back({"diagnostics-problems-1920x1080-150", 1920, 1080, 144, true, [](AppState& s) {
                        base_state(s, false);
                        s.page = AppPage::Diagnostics;
                        s.set_banner(L"The previous engine run failed: glasses display was not found  (open the "
                                     L"engine log for the full output)",
                                     gt::Severity::Warning);
                    }});
    list.push_back({"settings-1280x800-light", 1280, 800, 96, false, page(AppPage::Settings, true, false)});
    list.push_back({"settings-focus-1280x800", 1280, 800, 96, true, [](AppState& s) {
                        base_state(s, true);
                        s.page = AppPage::Settings;
                        s.focus_visible = true;
                        s.focus_id = gt::kUiThemeBase;
                        s.hover_id = gt::ui_help_id(2, 7);
                    }});
    const auto wizard = [](gt::OnboardingStep step, bool ready, std::function<void(AppState&)> extra) {
        return [=](AppState& s) {
            base_state(s, ready);
            s.config.onboarding_complete = false;
            s.onboarding.start();
            while (s.onboarding.step != step) {
                s.onboarding.next();
            }
            if (extra) {
                extra(s);
            }
        };
    };
    list.push_back({"wizard-1-welcome-1280x800", 1280, 800, 96, true,
                    wizard(gt::OnboardingStep::Welcome, true, nullptr)});
    list.push_back({"wizard-2-glasses-problems-1280x800", 1280, 800, 96, true,
                    wizard(gt::OnboardingStep::GlassesCheck, false, nullptr)});
    list.push_back({"wizard-3-head-recording-1920x1080-150", 1920, 1080, 144, true,
                    wizard(gt::OnboardingStep::HeadCalibration, true, [](AppState& s) {
                        s.cal_in_wizard = true;
                        s.cal_run.begin(gt::CalFlow::Orientation, true, 5.0);
                        s.cal_run.consume_line(
                            "@@progress {\"flow\":\"orientation\",\"step\":3,\"steps\":4,\"state\":\"recording\","
                            "\"prompt\":\"Slowly nod down and back up\",\"detail\":\"Move only your head.\","
                            "\"quality\":0.64}");
                    })});
    list.push_back({"wizard-3-head-failed-1280x800-light", 1280, 800, 96, false,
                    wizard(gt::OnboardingStep::HeadCalibration, true, [](AppState& s) {
                        s.cal_in_wizard = true;
                        s.cal_run.begin(gt::CalFlow::Orientation, true, 5.0);
                        s.cal_run.consume_line(
                            "@@progress {\"flow\":\"orientation\",\"step\":1,\"steps\":4,\"state\":\"failed\","
                            "\"prompt\":\"Connect the glasses and try again.\",\"detail\":\"NO_DEVICE: no RayNeo GT "
                            "HID device found\",\"quality\":-1}");
                        s.cal_run.consume_line("no RayNeo GT HID device found; connect the glasses and retry");
                        s.cal_run.process_exited(1);
                    })});
    list.push_back({"wizard-3-head-saved-retry-1280x800-light", 1280, 800, 96, false,
                    wizard(gt::OnboardingStep::HeadCalibration, true, [](AppState& s) {
                        s.cal_in_wizard = true;
                        s.cal_run.begin(gt::CalFlow::Orientation, true, 5.0);
                        s.cal_run.consume_line(
                            "@@progress {\"flow\":\"orientation\",\"step\":4,\"steps\":4,\"state\":\"done\","
                            "\"prompt\":\"Orientation calibration saved.\",\"detail\":\"up_vs_yaw 6.10 deg, "
                            "nod_level 5.40 deg, nod_tilt 12.80 deg, handedness 9.95 deg\",\"quality\":0.31}");
                        s.cal_run.process_exited(0);
                    })});
    list.push_back({"wizard-4-compass-1280x800", 1280, 800, 96, true,
                    wizard(gt::OnboardingStep::MagCalibration, true, nullptr)});
    list.push_back({"wizard-4-compass-countdown-1280x800", 1280, 800, 96, true,
                    wizard(gt::OnboardingStep::MagCalibration, true, [](AppState& s) {
                        s.cal_in_wizard = true;
                        s.cal_earbuds = true;
                        s.cal_run.begin(gt::CalFlow::Mag, true, 5.0);
                        s.cal_run.consume_line(
                            "@@progress {\"flow\":\"mag\",\"step\":1,\"steps\":2,\"state\":\"waiting\","
                            "\"prompt\":\"Look straight ahead and hold completely still for 4 seconds.\","
                            "\"detail\":\"\",\"quality\":-1}",
                            s.now_s - 1.4);
                    })});
    list.push_back({"wizard-5-layout-1280x800", 1280, 800, 96, true,
                    wizard(gt::OnboardingStep::LayoutPreset, true, [](AppState& s) {
                        s.onboarding.preset_choice = static_cast<int>(gt::LayoutPreset::QuadArc);
                    })});
    list.push_back({"wizard-6-done-1280x800-light", 1280, 800, 96, false,
                    wizard(gt::OnboardingStep::Done, true, [](AppState& s) {
                        s.onboarding.head = gt::StepOutcome::Completed;
                    })});
    list.push_back({"wizard-narrow-820x640", 820, 640, 96, true,
                    wizard(gt::OnboardingStep::GlassesCheck, true, nullptr)});
    // Live calibration guide in the PC panel (Tracking page and setup guide).
    for (const GuideCase& guide : guide_cases()) {
        const auto setup = guide.setup;
        list.push_back({"tracking-guide-" + guide.name + "-1280x1700", 1280, 1700, 96, true, [setup](AppState& s) {
                            base_state(s, true);
                            s.page = AppPage::Tracking;
                            setup(s.cal_run, s.now_s);
                        }});
    }
    list.push_back({"tracking-guide-turn-off-axis-1280x1700-light", 1280, 1700, 96, false, [](AppState& s) {
                        base_state(s, true);
                        s.page = AppPage::Tracking;
                        guide_cases()[2].setup(s.cal_run, s.now_s);
                    }});
    list.push_back({"wizard-3-head-guide-nod-1280x800", 1280, 800, 96, true,
                    wizard(gt::OnboardingStep::HeadCalibration, true, [](AppState& s) {
                        s.cal_in_wizard = true;
                        guide_cases()[3].setup(s.cal_run, s.now_s);
                    })});
    list.push_back({"wizard-guide-tilt-narrow-820x640", 820, 640, 96, true,
                    wizard(gt::OnboardingStep::HeadCalibration, true, [](AppState& s) {
                        s.cal_in_wizard = true;
                        guide_cases()[4].setup(s.cal_run, s.now_s);
                    })});
    // A run that failed and exited: the sidebar card must say so on every
    // page (it used to fall back to a grey "Stopped" once the process was
    // gone, while only Home showed the failure).
    for (const AppPage failed_page : {AppPage::Home, AppPage::Layout}) {
        list.push_back({std::string("sidebar-failed-") + (failed_page == AppPage::Home ? "home" : "layout") +
                            "-1280x800",
                        1280, 800, 96, true,
                        [failed_page](AppState& s) {
                            base_state(s, true);
                            gt::EngineSnapshot snapshot;
                            snapshot.state = gt::EngineProcessState::Failed;
                            snapshot.has_exit_code = true;
                            snapshot.exit_code = 1;
                            snapshot.last_error = "engine exited with code 1";
                            s.engine.set_preview_snapshot(snapshot, false);
                            s.page = failed_page;
                        },
                        [](const std::string& name, const AppState& s, const uint32_t* bits, const auto&) {
                            check_status_dot(name, 1280, 800, 96, bits, gt::ui::current_theme(s).error, "the error colour");
                        }});
    }
    list.push_back({"sidebar-stopped-1280x800", 1280, 800, 96, true, page(AppPage::Settings, true, false),
                    [](const std::string& name, const AppState& s, const uint32_t* bits, const auto&) {
                        check_status_dot(name, 1280, 800, 96, bits, gt::ui::current_theme(s).text_faint, "faint");
                    }});
    // Slider knobs at both ends and the middle sit where a press maps.
    list.push_back({"comfort-slider-ends-1280x800", 1280, 800, 96, true,
                    [](AppState& s) {
                        base_state(s, true);
                        s.page = AppPage::Comfort;
                        s.config.comfort.dim_mode = gt::DimMode::Manual;
                        s.config.comfort.screen_brightness_pct[0] = gt::kComfortMinBrightnessPct;
                        s.config.comfort.screen_brightness_pct[1] = 100;
                        s.config.comfort.screen_brightness_pct[2] = (gt::kComfortMinBrightnessPct + 100) / 2;
                        s.config.comfort.night_tint = true;
                        s.config.comfort.night_tint_pct = 100;
                    },
                    [](const std::string& name, const AppState& s, const uint32_t* bits, const auto& hotspots) {
                        const int min = gt::kComfortMinBrightnessPct;
                        for (int i = 0; i < 3; ++i) {
                            const int pct = s.config.comfort.screen_brightness_pct[static_cast<size_t>(i)];
                            check_slider_knob(name, 1280, 96, bits, hotspots, gt::kUiBrightnessBase + i,
                                              static_cast<float>(pct - min) / static_cast<float>(100 - min));
                        }
                        check_slider_knob(name, 1280, 96, bits, hotspots, gt::kUiNightTintStrength, 1.0f);
                    }});
    // A broken layout file: the readiness row and the Layout card must not
    // both register the same go-to id (Tab focus would loop between them).
    list.push_back({"home-layout-problem-1280x800", 1280, 800, 96, true, [](AppState& s) {
                        base_state(s, true);
                        s.diagnostics.layout_ok = false;
                        s.diagnostics.layout_detail = L"config/layouts/default.json: screen 2 overlaps screen 1";
                        s.page = AppPage::Home;
                    },
                    // The engine exits with "layout load failed" on this file in
                    // either mode, so Home must not offer a start.
                    [](const std::string& name, const AppState& s, const uint32_t*, const auto& hotspots) {
                        for (const int id : {static_cast<int>(gt::kUiStartWorkspace),
                                             static_cast<int>(gt::kUiStartPreview)}) {
                            const auto spot = std::find_if(hotspots.begin(), hotspots.end(),
                                                           [id](const gt::ui::Hotspot& h) { return h.id == id; });
                            if (spot == hotspots.end() || spot->enabled) {
                                fail(name, "start button " + std::to_string(id) +
                                               " is offered although the layout file cannot load");
                            }
                        }
                        const gt::LaunchMode modes[] = {gt::LaunchMode::Workspace, gt::LaunchMode::Preview};
                        for (const gt::LaunchMode mode : modes) {
                            if (gt::start_block_reason(s, mode).empty()) {
                                fail(name, "start_block_reason is empty with a broken layout file");
                            }
                        }
                    }});
    // The smallest window the controller allows (760x560 outer, about
    // 744x521 client), every page, with six user presets: the Layout page
    // must still paint its editor and inspector.
    const std::vector<std::string> many_presets = {"triple",      "ultrawide",   "reading-desk",
                                                   "evening-sofa", "coding-wide", "travel-laptop"};
    static const char* const kPageNames[] = {"home", "layout", "comfort", "tracking", "diagnostics", "settings"};
    for (int p = 0; p < gt::kAppPageCount; ++p) {
        list.push_back({std::string("min-") + kPageNames[p] + "-744x521", 744, 521, 96, true,
                        [p, many_presets](AppState& s) {
                            base_state(s, true);
                            running_state(s);
                            s.preset_names = many_presets;
                            s.page = static_cast<AppPage>(p);
                            s.config.comfort.dim_mode = gt::DimMode::Manual;
                        }});
    }
    list.push_back({"min-layout-1488x1042-200-light", 1488, 1042, 192, false, [many_presets](AppState& s) {
                        base_state(s, true);
                        s.preset_names = many_presets;
                        s.page = AppPage::Layout;
                        s.layout_dirty = true;
                    }});
    return list;
}

// The glasses window's picture, rendered offscreen at the glasses' 1920x1080.
void render_glasses_guide(const GuideCase& guide, const std::filesystem::path& out_dir) {
    const int width = 1920;
    const int height = 1080;
    gt::AppState state;
    guide.setup(state.cal_run, state.now_s);
    const gt::CalGuideView view = gt::cal_guide_view(state.cal_run, state.now_s);
    const std::string name = "glasses-guide-" + guide.name;
    if (!view.visible || view.headline.empty()) {
        fail(name, "the guide view is not visible or has no headline");
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (dc == nullptr || bitmap == nullptr || bits == nullptr) {
        fail(name, "could not create the offscreen bitmap");
        return;
    }
    const HGDIOBJ previous = SelectObject(dc, bitmap);
    const RECT client{0, 0, width, height};
    gt::ui::GlassesGuideFonts fonts;
    fonts.create(height);
    {
        gt::ui::Canvas canvas(dc, client, gt::ui::dark_theme(), 96);
        gt::ui::paint_cal_guide(canvas, client, view, gt::ui::glasses_guide_style(fonts), 46.0f);
    }
    GdiFlush();
    // Checks: the background stays black (see-through on the optics), the
    // red guide line is drawn, and the ball sits where the geometry says.
    const auto pixel = [&](int x, int y) {
        const uint32_t value = static_cast<const uint32_t*>(bits)[static_cast<size_t>(y) * width + x];
        return std::array<int, 3>{static_cast<int>((value >> 16) & 0xff), static_cast<int>((value >> 8) & 0xff),
                                  static_cast<int>(value & 0xff)};
    };
    size_t red = 0;
    size_t lit = 0;
    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            const auto p = pixel(x, y);
            if (p[0] + p[1] + p[2] > 30) {
                ++lit;
            }
            if (p[0] > 200 && p[1] < 90 && p[2] < 90) {
                ++red;
            }
        }
    }
    const auto corner = pixel(width - 3, height - 3);
    if (corner[0] + corner[1] + corner[2] != 0) {
        fail(name, "the background is not black");
    }
    const size_t samples = static_cast<size_t>(width / 2) * static_cast<size_t>(height / 2);
    if (lit * 4 > samples) {
        fail(name, "more than a quarter of the glasses frame is lit");
    }
    if (view.live.kind != gt::CalGuideKind::Still && red < 200) {
        fail(name, "the red guide line is missing");
    }
    if (view.has_live) {
        const gt::CalGuidePoint ball =
            gt::ui::cal_guide_target_ball(client, view, gt::ui::glasses_guide_style(fonts), 46.0f);
        const auto p = pixel(std::clamp(static_cast<int>(ball.x), 0, width - 1),
                             std::clamp(static_cast<int>(ball.y), 0, height - 1));
        if (p[0] + p[1] + p[2] < 200) {
            fail(name, "no ball at the computed head position");
        }
    }
    std::printf("  [%s] %s: %dx%d glasses, %zu red / %zu lit samples\n", failures == 0 ? "ok" : "..", name.c_str(),
                width, height, red, lit);
    if (!out_dir.empty() && !save_png(bitmap, out_dir / (name + ".png"))) {
        fail(name, "could not write the PNG");
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path out_dir;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (arg == "--only" && i + 1 < argc) {
            only = argv[++i];
        }
    }
    if (!out_dir.empty()) {
        std::error_code error;
        std::filesystem::create_directories(out_dir, error);
    }
    gt::ui::GdiplusSession gdiplus;
    if (!gdiplus.ok()) {
        std::printf("ui_snapshot: GDI+ failed to start\n");
        return 1;
    }
    check_contrast();
    check_editor_labels();
    std::printf("ui_snapshot: offscreen renders of the controller UI\n");
    for (const Shot& shot : shots()) {
        if (!only.empty() && shot.name.find(only) == std::string::npos) {
            continue;
        }
        render(shot, out_dir);
    }
    std::printf("ui_snapshot: the calibration guide on the glasses (offscreen)\n");
    for (const GuideCase& guide : guide_cases()) {
        if (!only.empty() && ("glasses-guide-" + guide.name).find(only) == std::string::npos) {
            continue;
        }
        render_glasses_guide(guide, out_dir);
    }
    std::printf("ui_snapshot: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
