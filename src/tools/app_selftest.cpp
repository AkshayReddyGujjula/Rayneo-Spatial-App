// app_selftest - pure app-model regression for the controller.
//
// Covers what the GUI cannot assert on its own: the engine command contract,
// the status-file protocol, preference validation and atomic persistence, layout
// presets, screen add/remove invariants, field normalisation, bounded log
// rotation and the telemetry tail reader. No window is created and no driver,
// display or HID device is touched.

#include "app/engine_protocol.h"
#include "app_shell/app_config.h"
#include "app_shell/app_layout.h"
#include "app_shell/engine_commands.h"
#include "util/utf8_path.h"
#include "app_shell/telemetry.h"
#include "app_shell/ui_help.h"
#include "app_shell/ui_orbit.h"

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& label) {
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", label.c_str());
    if (!condition) {
        ++failures;
    }
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!output) {
        return false;
    }
    output << text;
    return output.good();
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) {
        return std::string();
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool unique_screens(const gt::Layout& layout, std::string& error) {
    std::set<std::string> ids;
    std::set<int> indices;
    for (const gt::ScreenLayout& screen : layout.screens) {
        if (!ids.insert(screen.id).second) {
            error = "duplicate id " + screen.id;
            return false;
        }
        if (!indices.insert(screen.vdd_index).second) {
            error = "duplicate vdd index";
            return false;
        }
    }
    return true;
}

void test_engine_protocol() {
    std::printf("app_selftest: engine command constants\n");
    const unsigned messages[] = {
        gt::kEngineMessageQuery,      gt::kEngineMessageQuit,
        gt::kEngineMessageRecenter,   gt::kEngineMessageToggleYaw,
        gt::kEngineMessageTogglePitch, gt::kEngineMessageReloadLayout,
    };
    std::set<unsigned> seen;
    bool all_private = true;
    bool all_named = true;
    for (const unsigned message : messages) {
        all_private = all_private && message >= 0x8000u && message <= 0xBFFFu;
        all_named = all_named && std::string(gt::engine_message_name(message)) != "unknown";
        seen.insert(message);
    }
    check(all_private, "private messages stay inside the WM_APP range");
    check(seen.size() == std::size(messages), "private messages are distinct");
    check(all_named, "every private message has a stable name");
    check(std::string(gt::engine_message_name(0x1234u)) == "unknown",
          "unknown message names are reported as unknown");
    check(gt::kEngineScreenCountShift == 8 &&
              (gt::kEngineFlagYawTracking | gt::kEngineFlagPitchTracking |
               gt::kEngineFlagVirtualDisplays | gt::kEngineFlagHeadTracking |
               gt::kEngineFlagTopologyTakeover) == 0x1Fu,
          "query reply bit layout is stable");

    const std::wstring aumid(gt::app_user_model_id());
    check(!aumid.empty() && aumid.size() <= 128 && aumid.find(L' ') == std::wstring::npos,
          "AppUserModelID is a single stable token");
    check(aumid == std::wstring(gt::kAppUserModelId), "AppUserModelID accessor matches the constant");

    const std::string status_keys[] = {
        gt::kStatusKeyState,         gt::kStatusKeyMode,    gt::kStatusKeyScreens,
        gt::kStatusKeyFps,           gt::kStatusKeyMonitor, gt::kStatusKeyMonitorWidth,
        gt::kStatusKeyMonitorHeight, gt::kStatusKeyVdd,     gt::kStatusKeyImu,
        gt::kStatusKeyDetached,      gt::kStatusKeyElapsed, gt::kStatusKeyUpdated,
        gt::kStatusKeyError,
    };
    std::set<std::string> unique_keys;
    for (const std::string& key : status_keys) {
        unique_keys.insert(key);
    }
    check(unique_keys.size() == std::size(status_keys), "status-file keys are distinct");

    check(gt::engine_status_sanitize("a=b\nc") == "a b c",
          "status values are stripped of separators and newlines");
}

void test_status_freshness() {
    std::printf("app_selftest: status freshness gate\n");
    gt::EngineStatusFile fresh;
    fresh.parsed = true;
    fresh.updated_unix = 1000;
    check(gt::engine_status_is_fresh(fresh, 1000, gt::kEngineStatusMaxAgeS),
          "current status is fresh");
    check(gt::engine_status_is_fresh(fresh, 1003, gt::kEngineStatusMaxAgeS),
          "3 s old status is fresh");
    check(!gt::engine_status_is_fresh(fresh, 1004, gt::kEngineStatusMaxAgeS),
          "4 s old status is stale");
    check(!gt::engine_status_is_fresh(fresh, 999, gt::kEngineStatusMaxAgeS),
          "future status is rejected");
    fresh.updated_unix = 0;
    check(!gt::engine_status_is_fresh(fresh, 1000, gt::kEngineStatusMaxAgeS),
          "missing stamp is rejected");
    fresh.parsed = false;
    fresh.updated_unix = 1000;
    check(!gt::engine_status_is_fresh(fresh, 1000, gt::kEngineStatusMaxAgeS),
          "unparsed status is rejected");
}

void test_engine_commands() {
    std::printf("app_selftest: engine command construction\n");
    gt::EngineLaunchCommand command;
    std::string error;
    const std::string engine = "C:\\stage\\spatial_desk.exe";
    const std::string working = "C:\\stage";
    const std::string layout = "C:\\stage\\config\\layouts\\default.json";
    const std::string calibration = "C:\\stage\\config\\orientation.json";
    const std::string status = "C:\\stage\\logs\\engine-status.txt";
    const std::string engine_log = "C:\\stage\\logs\\engine.log";
    const std::string telemetry = "C:\\stage\\logs\\telemetry.csv";

    const bool workspace = gt::build_engine_launch_command(
        engine, working, layout, calibration, status, engine_log, telemetry,
        gt::LaunchMode::Workspace, true, -1, 0.0f, command, error);
    check(workspace, "workspace command builds");
    bool has_no_vdd = false;
    bool has_calibration = false;
    bool has_status = false;
    bool has_log = false;
    for (const std::string& argument : command.arguments) {
        has_no_vdd = has_no_vdd || argument == gt::kEngineArgNoVirtualDisplays;
        has_calibration = has_calibration || argument == gt::kEngineArgCalibration;
        has_status = has_status || argument == gt::kEngineArgStatus;
        has_log = has_log || argument == gt::kEngineArgLog;
    }
    check(!has_no_vdd, "workspace does not pass --no-virtual-displays");
    check(has_calibration, "workspace passes the calibration file");
    check(has_status && has_log, "the status file and telemetry CSV are always passed");
    check(command.command_line.rfind(gt::quote_command_line_argument(engine), 0) == 0,
          "the command line starts with the quoted executable");

    const bool preview = gt::build_engine_launch_command(
        engine, working, layout, calibration, status, engine_log, telemetry,
        gt::LaunchMode::Preview, true, 2, 52.0f, command, error);
    check(preview, "preview command builds");
    has_no_vdd = false;
    bool has_monitor = false;
    bool has_fov = false;
    for (size_t index = 0; index < command.arguments.size(); ++index) {
        has_no_vdd = has_no_vdd || command.arguments[index] == gt::kEngineArgNoVirtualDisplays;
        if (command.arguments[index] == gt::kEngineArgMonitor && index + 1 < command.arguments.size()) {
            has_monitor = command.arguments[index + 1] == "2";
        }
        if (command.arguments[index] == gt::kEngineArgFov && index + 1 < command.arguments.size()) {
            has_fov = command.arguments[index + 1] == "52";
        }
    }
    check(has_no_vdd, "preview is explicitly renderer-only");
    check(has_monitor, "an explicit monitor index is forwarded");
    check(has_fov, "a field-of-view override is forwarded");

    const bool no_imu = gt::build_engine_launch_command(
        engine, working, layout, calibration, status, engine_log, telemetry,
        gt::LaunchMode::Preview, false, -1, 0.0f, command, error);
    check(no_imu, "preview without head tracking builds");
    bool has_no_imu_flag = false;
    has_calibration = false;
    for (const std::string& argument : command.arguments) {
        has_no_imu_flag = has_no_imu_flag || argument == gt::kEngineArgNoImu;
        has_calibration = has_calibration || argument == gt::kEngineArgCalibration;
    }
    check(has_no_imu_flag, "--no-imu is passed when head tracking is off");
    check(!has_calibration, "no calibration file is passed without head tracking");

    check(!gt::build_engine_launch_command(engine, working, layout, calibration, status, engine_log,
                                           telemetry, gt::LaunchMode::None, true, -1, 0.0f, command,
                                           error),
          "a missing mode is rejected");
    check(!gt::build_engine_launch_command("", working, layout, calibration, status, engine_log,
                                           telemetry, gt::LaunchMode::Preview, true, -1, 0.0f,
                                           command, error),
          "a missing engine executable is rejected");
    check(!gt::build_engine_launch_command(engine, working, layout, calibration, status, engine_log,
                                           telemetry, gt::LaunchMode::Preview, true, 99, 0.0f,
                                           command, error),
          "an out-of-range monitor index is rejected");

    check(gt::quote_command_line_argument("plain") == "plain", "plain arguments stay unquoted");
    check(gt::quote_command_line_argument("C:\\Program Files\\app.exe") ==
              "\"C:\\Program Files\\app.exe\"",
          "paths with spaces are quoted");
    check(gt::quote_command_line_argument("a\"b") == "\"a\\\"b\"",
          "embedded quotes are escaped with a backslash");
    check(gt::quote_command_line_argument("C:\\dir with space\\") == "\"C:\\dir with space\\\\\"",
          "a trailing backslash is doubled before the closing quote");

    gt::EngineStatusFile status_file;
    const std::string status_text =
        "state=ready\nmode=workspace\nscreens=3\nfps=119.8\nmonitor=1\nmonitor_width=1920\n"
        "monitor_height=1080\nvdd=1\nimu=1\ndetached=1\nelapsed_s=12.5\nupdated_unix=42\nerror=\n";
    check(gt::parse_engine_status(status_text, status_file, error), "status file parses");
    check(status_file.state == "ready" && status_file.mode == "workspace" &&
              status_file.screens == 3 && status_file.vdd && status_file.imu &&
              status_file.detached && std::fabs(status_file.fps - 119.8) < 0.05,
          "status fields are read back");
    check(gt::engine_status_summary(status_file).find("3 screens") != std::string::npos,
          "status summary reports the screen count");
    check(gt::engine_status_summary(status_file).find("laptop display detached") !=
                  std::string::npos,
          "status summary reports the detached panel");
    check(!gt::parse_engine_status("mode=workspace\n", status_file, error),
          "a status file without a state key is rejected");
    gt::EngineStatusFile failed_status;
    check(gt::parse_engine_status("state=failed\nerror=virtual display startup failed\n",
                                  failed_status, error) &&
              gt::engine_status_summary(failed_status).find("virtual display startup failed") !=
                  std::string::npos,
          "a failed status carries its reason into the summary");
}

void test_app_config(const std::filesystem::path& directory) {
    std::printf("app_selftest: preference validation and atomic persistence\n");
    gt::AppConfig defaults;
    std::string error;
    check(gt::validate_app_config(defaults, error), "default preferences validate");
    check(defaults.health_poll_ms >= gt::kMinHealthPollMs,
          "default polling is never faster than twice per second");

    gt::AppConfig bad = defaults;
    bad.health_poll_ms = 100;
    check(!gt::validate_app_config(bad, error) && !error.empty(),
          "polling faster than 2 Hz is rejected");
    bad = defaults;
    bad.version = 2;
    check(!gt::validate_app_config(bad, error), "unknown preference versions are rejected");
    bad = defaults;
    bad.engine_log_rotation.max_files = 0;
    check(!gt::validate_app_config(bad, error), "log rotation without generations is rejected");
    bad = defaults;
    bad.telemetry_rotation.max_bytes = 1024;
    check(!gt::validate_app_config(bad, error), "an undersized log cap is rejected");
    bad = defaults;
    bad.layout_path.clear();
    check(!gt::validate_app_config(bad, error), "an empty layout path is rejected");
    bad = defaults;
    bad.monitor_index = 42;
    check(!gt::validate_app_config(bad, error), "an out-of-range monitor index is rejected");
    bad = defaults;
    bad.splits.column = 2.0f;
    check(!gt::validate_app_config(bad, error), "an out-of-range split fraction is rejected");
    bad = defaults;
    bad.splits.left[0] = 0.0f;
    bad.splits.left[1] = 0.0f;
    bad.splits.left[2] = 1.0f;
    check(gt::validate_app_config(bad, error), "degenerate split shares validate (paint clamps)");
    {
        // A pre-splitter preference file (no "splits" block) loads as automatic.
        const std::filesystem::path legacy =
            std::filesystem::temp_directory_path() / "rayneo-legacy-app.json";
        {
            std::ofstream out(legacy, std::ios::binary | std::ios::trunc);
            out << "{\"version\": 1, \"layout_path\": \"a\", \"calibration_path\": \"b\", "
                   "\"log_dir\": \"c\", \"monitor_index\": -1, \"health_poll_ms\": 1000, "
                   "\"preview_without_head_tracking\": false, \"close_to_tray\": false}";
        }
        gt::AppConfig loaded;
        check(gt::load_app_config(legacy, loaded, error) && loaded.splits.column < 0.0f &&
                  loaded.splits.left[0] < 0.0f,
              "a legacy file without splits loads as automatic");
        std::error_code remove_error;
        std::filesystem::remove(legacy, remove_error);
    }
    {
        // Malformed splits fail the load instead of crashing the dashboard.
        const std::filesystem::path broken =
            std::filesystem::temp_directory_path() / "rayneo-broken-app.json";
        {
            std::ofstream out(broken, std::ios::binary | std::ios::trunc);
            out << "{\"version\": 1, \"layout_path\": \"a\", \"calibration_path\": \"b\", "
                   "\"log_dir\": \"c\", \"monitor_index\": -1, \"health_poll_ms\": 1000, "
                   "\"preview_without_head_tracking\": false, \"close_to_tray\": false, "
                   "\"splits\": {\"column\": \"wide\"}}";
        }
        gt::AppConfig loaded;
        check(!gt::load_app_config(broken, loaded, error), "malformed splits fail the load");
        std::error_code remove_error;
        std::filesystem::remove(broken, remove_error);
    }

    const std::filesystem::path path = directory / "app.json";
    gt::AppConfig config = defaults;
    config.layout_path = "config/layouts/wide.json";
    config.calibration_path = "config/orientation.json";
    config.log_dir = "logs";
    config.monitor_index = 3;
    config.last_mode = gt::LaunchMode::Preview;
    config.preview_without_head_tracking = true;
    config.close_to_tray = false;
    config.health_poll_ms = 750;
    config.engine_log_rotation = {128u << 10, 2};
    config.telemetry_rotation = {256u << 10, 4};
    config.last_engine_error = "engine exited with code 1";
    config.window = {120, 80, 1440, 900, true};
    config.splits.column = 0.33f;
    config.splits.left[0] = 0.4f;
    config.splits.left[1] = 0.3f;
    config.splits.left[2] = 0.3f;
    config.splits.right[0] = 0.25f;
    config.splits.right[1] = 0.35f;
    config.splits.right[2] = 0.4f;
    check(gt::save_app_config(path, config, error), "preferences save");
    check(!std::filesystem::exists(std::filesystem::path(path.string() + ".tmp")),
          "the temporary file is gone after a successful save");

    gt::AppConfig loaded;
    check(gt::load_app_config(path, loaded, error), "preferences load");
    check(loaded.layout_path == config.layout_path &&
              loaded.calibration_path == config.calibration_path &&
              loaded.log_dir == config.log_dir && loaded.monitor_index == config.monitor_index &&
              loaded.last_mode == gt::LaunchMode::Preview &&
              loaded.preview_without_head_tracking && !loaded.close_to_tray &&
              loaded.health_poll_ms == 750 &&
              loaded.engine_log_rotation.max_bytes == config.engine_log_rotation.max_bytes &&
              loaded.engine_log_rotation.max_files == 2 &&
              loaded.telemetry_rotation.max_files == 4 &&
              loaded.last_engine_error == config.last_engine_error &&
              loaded.window.x == 120 && loaded.window.width == 1440 && loaded.window.maximized &&
              std::fabs(loaded.splits.column - 0.33f) < 1e-6f &&
              std::fabs(loaded.splits.left[1] - 0.3f) < 1e-6f &&
              std::fabs(loaded.splits.right[2] - 0.4f) < 1e-6f,
          "every preference round-trips");

    const std::string before = read_text(path);
    gt::AppConfig invalid = config;
    invalid.health_poll_ms = 10;
    check(!gt::save_app_config(path, invalid, error), "an invalid preference set is not written");
    check(read_text(path) == before, "the existing file is left untouched by a failed save");
    check(!std::filesystem::exists(std::filesystem::path(path.string() + ".tmp")),
          "a failed save removes its temporary file");

    const std::filesystem::path missing = directory / "absent.json";
    gt::AppConfig fresh;
    fresh.health_poll_ms = 4242;
    check(gt::load_app_config(missing, fresh, error) && fresh.health_poll_ms == 1000,
          "a missing file loads as defaults");

    const std::filesystem::path malformed = directory / "malformed.json";
    check(write_text(malformed, "{ not json"), "malformed fixture written");
    gt::AppConfig unused;
    check(!gt::load_app_config(malformed, unused, error), "malformed JSON is reported, not guessed");
    check(write_text(malformed, "{\"version\": 1, \"health_poll_ms\": 50}"), "invalid fixture written");
    check(!gt::load_app_config(malformed, unused, error),
          "out-of-range values in a file are reported");

    check(gt::launch_mode_text(gt::LaunchMode::Workspace) == std::string("workspace") &&
              gt::launch_mode_text(gt::LaunchMode::Preview) == std::string("preview"),
          "launch modes have stable file names");
    gt::LaunchMode mode = gt::LaunchMode::Workspace;
    check(gt::parse_launch_mode("preview", mode) && mode == gt::LaunchMode::Preview &&
              !gt::parse_launch_mode("elsewhere", mode),
          "launch mode parsing accepts only known modes");
}

void test_presets_and_screens() {
    std::printf("app_selftest: presets and screen add/remove invariants\n");
    std::string error;
    bool all_valid = true;
    bool all_unique = true;
    bool all_in_range = true;
    for (const gt::LayoutPreset preset : gt::layout_presets()) {
        const gt::Layout layout = gt::preset_layout(preset);
        all_valid = all_valid && gt::validate_layout(layout, error);
        all_unique = all_unique && unique_screens(layout, error);
        all_in_range = all_in_range && !layout.screens.empty() && layout.screens.size() <= 8;
    }
    check(all_valid, "every preset is a valid layout");
    check(all_unique, "preset ids and vdd indices are unique");
    check(all_in_range, "presets stay inside the 1..8 screen contract");
    check(gt::layout_presets().size() >= 4, "at least four presets are offered");

    const gt::Layout triple = gt::preset_layout(gt::LayoutPreset::TripleArc);
    check(triple.screens.size() == 3 && std::fabs(triple.screens[0].yaw_deg + 45.0f) < 1e-4f &&
              std::fabs(triple.screens[1].yaw_deg) < 1e-4f &&
              std::fabs(triple.screens[2].yaw_deg - 45.0f) < 1e-4f,
          "the triple arc is the default -45/0/+45 layout");
    const gt::Layout defaulted = gt::default_layout();
    check(triple.screens.size() == defaulted.screens.size() &&
              triple.screens[1].id == defaulted.screens[1].id &&
              triple.capture_policy.active_fps == defaulted.capture_policy.active_fps,
          "the triple-arc preset matches gt::default_layout");
    check(gt::preset_layout(gt::LayoutPreset::Single).screens.size() == 1,
          "the single-screen preset has one screen");

    gt::Layout layout = gt::preset_layout(gt::LayoutPreset::Single);
    check(gt::next_free_vdd_index(layout) == 0 && gt::next_screen_id(layout) == "screen-1",
          "free id and vdd slot selection start at the lowest available value");
    int added = 0;
    while (layout.screens.size() < 8) {
        if (!gt::add_screen(layout, error)) {
            break;
        }
        ++added;
    }
    check(layout.screens.size() == 8 && added == 7, "screens can be added up to the maximum of 8");
    check(!gt::add_screen(layout, error), "a ninth screen is refused");
    check(unique_screens(layout, error) && gt::validate_layout(layout, error),
          "the layout stays valid and unique at the maximum");

    const std::string removed_id = layout.screens[3].id;
    const int removed_index = layout.screens[3].vdd_index;
    check(gt::remove_screen(layout, 3, error), "a screen can be removed");
    check(layout.screens.size() == 7 && unique_screens(layout, error),
          "removing keeps the remaining ids and vdd indices unique");
    check(gt::next_free_vdd_index(layout) == removed_index,
          "a removed vdd index becomes available again");
    check(gt::next_screen_id(layout) == removed_id,
          "a removed screen id becomes available again");
    check(gt::add_screen(layout, error) && layout.screens.size() == 8 &&
              layout.screens.back().vdd_index == removed_index &&
              layout.screens.back().id == removed_id && unique_screens(layout, error),
          "the next screen reuses the freed id and vdd index");
    check(!gt::remove_screen(layout, 99, error), "an out-of-range removal is refused");

    while (layout.screens.size() > 1) {
        check(gt::remove_screen(layout, 0, error), "screens can be removed down to one");
    }
    check(!gt::remove_screen(layout, 0, error), "the last screen cannot be removed");
    check(gt::validate_layout(layout, error), "a one-screen layout is still valid");
}

void test_field_normalisation() {
    std::printf("app_selftest: field normalisation\n");
    gt::Layout layout = gt::preset_layout(gt::LayoutPreset::TripleArc);
    gt::ScreenLayout& screen = layout.screens[1];
    std::string error;
    check(gt::set_layout_field(layout, screen, gt::LayoutField::Yaw, 190.0f, error) &&
              std::fabs(screen.yaw_deg + 170.0f) < 1e-4f,
          "yaw wraps into -180..180");
    check(gt::set_layout_field(layout, screen, gt::LayoutField::Pitch, 120.0f, error) &&
              std::fabs(screen.pitch_deg - 89.0f) < 1e-4f,
          "pitch clamps to the validation limit");
    check(gt::set_layout_field(layout, screen, gt::LayoutField::Distance, 0.01f, error) &&
              std::fabs(screen.distance_m - 0.25f) < 1e-4f,
          "distance clamps to the validation limit");
    check(gt::set_layout_field(layout, screen, gt::LayoutField::ActiveFps, 5.0f, error) &&
              layout.capture_policy.active_fps == 5 && layout.capture_policy.mid_fps == 5 &&
              layout.capture_policy.idle_fps == 1 && gt::validate_layout(layout, error),
          "a low active FPS pulls mid down and keeps the ordering valid");
    check(gt::set_layout_field(layout, screen, gt::LayoutField::IdleFps, 240.0f, error) &&
              layout.capture_policy.idle_fps == 240 && layout.capture_policy.mid_fps == 240 &&
              layout.capture_policy.active_fps == 240,
          "a high idle FPS lifts mid and active");
    check(gt::set_layout_field(layout, screen, gt::LayoutField::EnterDeg, 10.0f, error) &&
              layout.capture_policy.enter_deg > layout.capture_policy.leave_deg &&
              gt::validate_layout(layout, error),
          "enter is kept strictly above leave");
    check(gt::set_layout_field(layout, screen, gt::LayoutField::LeaveDeg, 178.0f, error) &&
              layout.capture_policy.enter_deg > layout.capture_policy.leave_deg &&
              gt::validate_layout(layout, error),
          "leave cannot cross enter");
    check(!gt::set_layout_field(layout, screen, gt::LayoutField::Count, 1.0f, error),
          "an unknown field is refused");
    check(gt::layout_summary(layout).find("screens") != std::string::npos,
          "the layout summary names the screen count");
}

void test_log_rotation(const std::filesystem::path& directory) {
    std::printf("app_selftest: bounded log rotation\n");
    const gt::LogRotationPolicy policy{64, 2};
    check(!gt::decide_rotation(0, policy).rotate, "an empty log is not rotated");
    check(!gt::decide_rotation(63, policy).rotate, "a log under the cap is not rotated");
    check(gt::decide_rotation(64, policy).rotate, "a log at the cap is rotated");
    const gt::RotationDecision decision = gt::decide_rotation(1024, policy);
    check(decision.rotate && decision.generations == 2 && !decision.reason.empty(),
          "the rotation decision carries the generation count and a reason");
    check(gt::decide_rotation(1024, gt::LogRotationPolicy{64, 0}).generations == 1,
          "a policy without generations is clamped to one");

    const std::filesystem::path path = directory / "rotate.log";
    std::string error;
    check(gt::rotate_log_file(path, policy, error), "rotating a missing file is a no-op");
    check(!std::filesystem::exists(path), "no file is created by a no-op rotation");

    check(write_text(path, std::string(100, 'a')), "first log written");
    check(gt::rotate_log_file(path, policy, error), "the first rotation succeeds");
    check(!std::filesystem::exists(path) && std::filesystem::exists(gt::rotated_log_path(path, 1)),
          "the active log moves to generation 1");
    check(read_text(gt::rotated_log_path(path, 1)) == std::string(100, 'a'),
          "generation 1 keeps the original bytes");

    check(write_text(path, std::string(100, 'b')), "second log written");
    check(gt::rotate_log_file(path, policy, error), "the second rotation succeeds");
    check(read_text(gt::rotated_log_path(path, 1)) == std::string(100, 'b') &&
              read_text(gt::rotated_log_path(path, 2)) == std::string(100, 'a'),
          "generations shift oldest-first");

    check(write_text(path, std::string(100, 'c')), "third log written");
    check(gt::rotate_log_file(path, policy, error), "the third rotation succeeds");
    check(read_text(gt::rotated_log_path(path, 1)) == std::string(100, 'c') &&
              read_text(gt::rotated_log_path(path, 2)) == std::string(100, 'b'),
          "rotation stays bounded to the configured generations");

    check(gt::format_bytes(0) == "0 B" && gt::format_bytes(2048) == "2.0 KiB" &&
              gt::format_bytes(3u << 20) == "3.0 MiB",
          "byte formatting is human readable");
}

void test_telemetry() {
    std::printf("app_selftest: telemetry tail reader\n");
    gt::TelemetrySample sample;
    std::string error;
    const std::string header = gt::telemetry_csv_header();
    const std::string row =
        "12.345,123456,0.100,-0.200,0.300,0.010,0.020,0.030,-45.500,3.250,-1.750,0,1,1,0.420,"
        "1.500,0.060,2";
    check(gt::parse_telemetry_row(row, sample, error), "a canonical row parses");
    check(std::fabs(sample.elapsed_s - 12.345) < 1e-6 && sample.tick_100us == 123456u &&
              std::fabs(sample.yaw_deg + 45.5f) < 1e-4f && sample.rest && !sample.still &&
              sample.adapt_state == 1 && sample.escape_rollbacks == 2,
          "row fields land in the right members");
    check(!gt::parse_telemetry_row(header, sample, error), "the header is not data");
    check(!gt::parse_telemetry_row("1,2,3", sample, error), "a truncated row is rejected");

    const std::string tail = header + "\n" + row + "\n" +
                             "13.000,124000,0.100,-0.200,0.300,0.010,0.020,0.030,1.000,2.000,"
                             "3.000,0,1,3,0.100,0.900,0.050,0\n";
    check(gt::parse_telemetry_tail(tail, sample, error), "the tail parses");
    check(std::fabs(sample.elapsed_s - 13.0) < 1e-6 && sample.adapt_state == 3,
          "the newest row wins");
    check(gt::parse_telemetry_tail(row + "\n", sample, error),
          "a header-less file still parses positionally");
    const std::string reordered = "still,view_yaw_deg,elapsed_s\n1,10.0,5.5\n";
    check(gt::parse_telemetry_tail(reordered, sample, error) &&
              std::fabs(sample.yaw_deg - 10.0f) < 1e-4f &&
              std::fabs(sample.elapsed_s - 5.5) < 1e-6,
          "columns are mapped by header name when they move");
    check(!gt::parse_telemetry_tail("", sample, error), "an empty file reports no rows");
    check(!gt::parse_telemetry_tail(header + "\n", sample, error),
          "a header without rows reports no rows");
    check(std::wstring(gt::adapt_state_label(2)) == L"escape" &&
              std::wstring(gt::adapt_state_label(9)) == L"unknown",
          "adaptation states have stable labels");
}

void test_user_presets(const std::filesystem::path& directory) {
    std::printf("app_selftest: named user presets\n");
    std::string error;
    check(gt::layout_preset_name_valid("main setup", error), "spaces are allowed in preset names");
    check(gt::layout_preset_name_valid("triple", error), "the default active preset is valid");
    check(!gt::layout_preset_name_valid("", error), "an empty preset name is rejected");
    check(!gt::layout_preset_name_valid("../escape", error), "slashes are rejected");
    check(!gt::layout_preset_name_valid("a..b", error), "dot-dot is rejected");
    check(!gt::layout_preset_name_valid(".hidden", error), "a leading dot is rejected");
    check(!gt::layout_preset_name_valid("bad:name", error), "colons are rejected");
    check(!gt::layout_preset_name_valid(std::string(65, 'x'), error),
          "a 65-character name is rejected");

    const std::string presets = gt::utf8_from_path(directory / "presets");
    check(gt::list_layout_presets(presets).empty(), "a missing presets directory lists nothing");
    gt::Layout triple = gt::preset_layout(gt::LayoutPreset::TripleArc);
    check(gt::save_layout_preset(presets, "main setup", triple, error), "a preset saves");
    gt::Layout single = gt::preset_layout(gt::LayoutPreset::Single);
    check(gt::save_layout_preset(presets, "solo", single, error), "a second preset saves");
    const std::vector<std::string> names = gt::list_layout_presets(presets);
    check(names.size() == 2 && names[0] == "main setup" && names[1] == "solo",
          "presets list back sorted");
    gt::Layout loaded;
    check(gt::load_layout_preset(presets, "main setup", loaded, error) &&
              loaded.screens.size() == 3,
          "a preset round-trips");
    check(!gt::load_layout_preset(presets, "missing", loaded, error),
          "a missing preset fails to load");
    check(!gt::save_layout_preset(presets, "../escape", triple, error),
          "an evil name cannot escape the presets directory");
    check(!std::filesystem::exists(directory / "escape.json"), "no file escaped the directory");
    gt::Layout invalid;
    check(!gt::save_layout_preset(presets, "broken", invalid, error),
          "an invalid layout is not saved as a preset");
    check(gt::delete_layout_preset(presets, "solo", error), "a preset deletes");
    check(!gt::delete_layout_preset(presets, "solo", error), "deleting twice fails");
    check(gt::list_layout_presets(presets).size() == 1, "the deleted preset is gone");

    gt::AppConfig config;
    check(config.active_preset == "triple", "the default active preset is triple");
    check(gt::validate_app_config(config, error), "the default config still validates");
    config.active_preset = "../escape";
    check(!gt::validate_app_config(config, error), "an evil active preset fails validation");
}

void test_help_topics() {
    std::printf("app_selftest: help topics\n");
    bool all_long = true;
    for (int i = 0; i < static_cast<int>(gt::HelpTopic::Count); ++i) {
        const wchar_t* text = gt::help_text(static_cast<gt::HelpTopic>(i));
        all_long = all_long && text != nullptr && std::wcslen(text) >= 40;
    }
    check(all_long, "every help topic resolves to a substantive text");
    check(std::wstring(gt::help_text(gt::HelpTopic::FieldYaw)).find(L"Negative") !=
              std::wstring::npos,
          "the yaw slider help names both directions");
    check(std::wstring(gt::help_text(gt::HelpTopic::EngineYawToggle)).find(L"Ctrl+Alt+Y") !=
              std::wstring::npos,
          "the yaw toggle help names its hotkey");
    check(gt::help_for_field(gt::LayoutField::LeaveDeg) == gt::HelpTopic::FieldLeaveDeg,
          "field topics track the layout field order");
}

void test_orbit() {
    std::printf("app_selftest: orbit projection\n");
    gt::OrbitView view;
    view.yaw_deg = 0.0f;
    view.pitch_deg = 0.0f;
    view.distance_m = 6.0f;
    view.target_x = 0.0f;
    view.target_y = 1.0f;
    view.target_z = 0.0f;
    const gt::OrbitCamera camera = gt::orbit_camera(view);
    RECT rect{0, 0, 400, 300};
    const gt::OrbitPoint origin = gt::orbit_project(view, camera, gt::OrbitVec3{0.0f, 0.0f, 0.0f},
                                                   rect);
    check(!origin.behind && std::abs(origin.pixel.x - 200) <= 1 &&
              std::abs(origin.pixel.y - 150) <= 1,
          "the head projects to the viewport centre from behind");
    check(std::fabs(origin.depth_m - 5.0f) < 1e-3f, "head depth is the camera distance minus 1m");
    const gt::OrbitPoint behind =
        gt::orbit_project(view, camera, gt::OrbitVec3{0.0f, -6.0f, 0.0f}, rect);
    check(behind.behind, "points behind the camera are flagged");
    gt::Layout layout = gt::preset_layout(gt::LayoutPreset::Single);
    const gt::OrbitQuad quad = gt::orbit_screen_quad(layout.screens[0]);
    check(std::fabs(quad.center.x) < 1e-4f && std::fabs(quad.center.y - 2.0f) < 1e-4f &&
              std::fabs(quad.center.z) < 1e-4f,
          "the single screen centre sits 2m straight ahead");
    const gt::OrbitVec3 edge_x{quad.corners[1].x - quad.corners[0].x,
                               quad.corners[1].y - quad.corners[0].y,
                               quad.corners[1].z - quad.corners[0].z};
    const float width = std::sqrt(edge_x.x * edge_x.x + edge_x.y * edge_x.y + edge_x.z * edge_x.z);
    check(std::fabs(width - 1.6f) < 1e-3f && edge_x.x / width > 0.999f,
          "the quad right edge spans the width along +X");
    const gt::OrbitVec3 edge_z{quad.corners[3].x - quad.corners[0].x,
                               quad.corners[3].y - quad.corners[0].y,
                               quad.corners[3].z - quad.corners[0].z};
    const float height =
        std::sqrt(edge_z.x * edge_z.x + edge_z.y * edge_z.y + edge_z.z * edge_z.z);
    check(std::fabs(height - 0.9f) < 1e-3f && edge_z.z / height > 0.999f,
          "the quad up edge spans the height along +Z");
    const gt::OrbitPoint centre = gt::orbit_project(view, camera, quad.center, rect);
    check(gt::orbit_hit_test(layout, view, camera, rect, centre.pixel.x, centre.pixel.y, 14) == 0,
          "tapping a projected screen selects it");
    check(gt::orbit_hit_test(layout, view, camera, rect, 0, 0, 14) < 0,
          "tapping empty space selects nothing");
}

}  // namespace

int main() {
    std::printf("app_selftest: controller app model\n");
    std::filesystem::path directory;
    std::error_code directory_error;
    for (int attempt = 0; attempt < 100 && directory.empty(); ++attempt) {
        const std::filesystem::path candidate = std::filesystem::temp_directory_path() /
                                                ("rayneo-app-selftest-" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate, directory_error) && !directory_error) {
            directory = candidate;
        }
    }
    if (directory.empty()) {
        std::printf("  [FAIL] could not create the temporary directory: %s\n",
                    directory_error.message().c_str());
        return 1;
    }

    test_engine_protocol();
    test_engine_commands();
    test_status_freshness();
    test_app_config(directory);
    test_presets_and_screens();
    test_user_presets(directory);
    test_help_topics();
    test_orbit();
    test_field_normalisation();
    test_log_rotation(directory);
    test_telemetry();

    std::filesystem::remove_all(directory, directory_error);
    std::printf("app_selftest: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
