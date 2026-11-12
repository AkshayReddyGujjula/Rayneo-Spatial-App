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
#include "app_shell/ui_help.h"
#include "app_shell/ui_paint.h"
#include "app_shell/ui_widgets.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
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

struct Shot {
    std::string name;
    int width;
    int height;
    UINT dpi;
    bool dark;
    std::function<void(gt::AppState&)> setup;
};

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
                        s.cal_run.begin(gt::CalFlow::Orientation);
                        s.cal_run.consume_line(
                            "@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":5,\"state\":\"waiting\","
                            "\"prompt\":\"Look straight ahead and hold still\",\"detail\":\"Keep your torso "
                            "facing forward.\",\"quality\":0.82}");
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
                        s.cal_run.begin(gt::CalFlow::Orientation);
                        s.cal_run.consume_line(
                            "@@progress {\"flow\":\"orientation\",\"step\":3,\"steps\":5,\"state\":\"recording\","
                            "\"prompt\":\"Slowly nod down and back up\",\"detail\":\"Move only your head.\","
                            "\"quality\":0.64}");
                    })});
    list.push_back({"wizard-3-head-failed-1280x800-light", 1280, 800, 96, false,
                    wizard(gt::OnboardingStep::HeadCalibration, true, [](AppState& s) {
                        s.cal_in_wizard = true;
                        s.cal_run.begin(gt::CalFlow::Orientation);
                        s.cal_run.consume_line("unknown or incomplete option: --progress-json");
                        s.cal_run.process_exited(1);
                    })});
    list.push_back({"wizard-4-compass-1280x800", 1280, 800, 96, true,
                    wizard(gt::OnboardingStep::MagCalibration, true, nullptr)});
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
    return list;
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
    std::printf("ui_snapshot: offscreen renders of the controller UI\n");
    for (const Shot& shot : shots()) {
        if (!only.empty() && shot.name.find(only) == std::string::npos) {
            continue;
        }
        render(shot, out_dir);
    }
    std::printf("ui_snapshot: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
