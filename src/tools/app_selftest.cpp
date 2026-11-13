// app_selftest - pure app-model regression for the controller.
//
// Covers what the GUI cannot assert on its own: the engine command contract,
// the status-file protocol, preference validation and atomic persistence, layout
// presets, screen add/remove invariants, field normalisation, bounded log
// rotation and the telemetry tail reader. No window is created and no driver,
// display or HID device is touched.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "app/engine_protocol.h"
#include "app/view_comfort.h"
#include "app_shell/app_config.h"
#include "app_shell/app_layout.h"
#include "app_shell/calibration_guide.h"
#include "app_shell/calibration_progress.h"
#include "app_shell/engine_commands.h"
#include "app_shell/onboarding.h"
#include "app_shell/ui_anim.h"
#include "app_shell/ui_model.h"
#include "app_shell/session_logs.h"
#include "util/utf8_path.h"
#include "app_shell/telemetry.h"
#include "app_shell/ui_help.h"
#include "app_shell/ui_input.h"
#include "app_shell/ui_orbit.h"

#include <algorithm>
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
        gt::kEngineMessageSetStabilise, gt::kEngineMessageSetDimMode,
        gt::kEngineMessageSetScreenBrightness, gt::kEngineMessageSetNightTint,
        gt::kEngineMessageCursorToCenter,
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
    check((gt::kEngineStabiliseMask & gt::kEngineScreenCountMask) == 0 &&
              (gt::kEngineStabiliseMask & 0x1Fu) == 0 &&
              (gt::kEngineStabiliseMask >> gt::kEngineStabiliseShift) >=
                  static_cast<unsigned>(gt::kComfortStabiliseLevels),
          "stabilise level field fits beside the flags and screen count");

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
    // Normal pages get the sentence-style summary; the technical one above
    // stays on the Diagnostics page.
    check(gt::engine_status_display_summary(status_file) ==
              "Workspace  ·  3 screens  ·  120 fps  ·  virtual desktops  ·  laptop screen off",
          "the page status summary is plain words with middle dots");
    check(gt::engine_status_display_summary(failed_status) ==
              "Stopped with an error: virtual display startup failed",
          "a failed status reads as a sentence with its reason");
    gt::EngineStatusFile preview_status;
    check(gt::parse_engine_status("state=starting\nmode=preview\nscreens=1\nfps=0\nvdd=0\nimu=0\n",
                                  preview_status, error) &&
              gt::engine_status_display_summary(preview_status) ==
                  "Preview  ·  starting  ·  1 screen  ·  no head tracking",
          "preview, starting, one screen and no head tracking read plainly");
    check(gt::engine_status_display_summary(gt::EngineStatusFile{}).find('|') == std::string::npos,
          "no status yet is a sentence too");

    // Engine options (Tracking page) -> engine flags.
    const auto value_after = [](const std::vector<std::string>& arguments, const char* flag) {
        for (size_t index = 0; index + 1 < arguments.size(); ++index) {
            if (arguments[index] == flag) {
                return arguments[index + 1];
            }
        }
        return std::string();
    };
    const auto has_flag = [](const std::vector<std::string>& arguments, const char* flag) {
        for (const std::string& argument : arguments) {
            if (argument == flag) {
                return true;
            }
        }
        return false;
    };
    check(gt::build_engine_launch_command(engine, working, layout, calibration, status, engine_log, telemetry,
                                          gt::LaunchMode::Workspace, true, -1, 0.0f, command, error) &&
              value_after(command.arguments, gt::kEngineArgPredictMs) == "35" &&
              !has_flag(command.arguments, gt::kEngineArgMagTrackHardIron),
          "default engine options pass --predict-ms 35 and no hard-iron tracking");
    gt::EngineOptions options;
    options.predict_ms = 0;
    options.mag_track_hard_iron = true;
    check(gt::build_engine_launch_command(engine, working, layout, calibration, status, engine_log, telemetry,
                                          gt::LaunchMode::Preview, true, -1, 0.0f, command, error,
                                          gt::ViewComfort{}, options) &&
              value_after(command.arguments, gt::kEngineArgPredictMs) == "0" &&
              has_flag(command.arguments, gt::kEngineArgMagTrackHardIron) &&
              command.command_line.find(" --predict-ms 0") != std::string::npos,
          "prediction off passes --predict-ms 0; the earbud switch passes --mag-track-hard-iron");
    options.predict_ms = 50;
    check(gt::build_engine_launch_command(engine, working, layout, calibration, status, engine_log, telemetry,
                                          gt::LaunchMode::Preview, false, -1, 0.0f, command, error,
                                          gt::ViewComfort{}, options) &&
              !has_flag(command.arguments, gt::kEngineArgPredictMs) &&
              !has_flag(command.arguments, gt::kEngineArgMagTrackHardIron),
          "without head tracking no IMU option is passed");
    check(gt::build_engine_launch_command(engine, working, layout, calibration, status, engine_log, telemetry,
                                          gt::LaunchMode::Preview, true, -1, 0.0f, command, error,
                                          gt::ViewComfort{}, options) &&
              value_after(command.arguments, gt::kEngineArgPredictMs) == "50",
          "a 50 ms choice is forwarded");
    options.predict_ms = 101;
    check(!gt::build_engine_launch_command(engine, working, layout, calibration, status, engine_log, telemetry,
                                           gt::LaunchMode::Preview, true, -1, 0.0f, command, error,
                                           gt::ViewComfort{}, options),
          "a prediction horizon outside 0..100 ms is rejected");
    check(gt::predict_ms_choice_index(0) == 0 && gt::predict_ms_choice_index(35) == 2 &&
              gt::predict_ms_choice_index(50) == 3 && gt::predict_ms_choice_index(40) == -1,
          "prediction choices map to segment indices; other values select none");

    // Heading-lock status keys -> Tracking page badge state.
    const auto lock_of = [&](const std::string& text) {
        gt::EngineStatusFile parsed;
        std::string parse_error;
        if (!gt::parse_engine_status(text, parsed, parse_error)) {
            return gt::HeadingLockState::Unknown;
        }
        return gt::heading_lock_from_status(parsed);
    };
    gt::EngineStatusFile mag_status;
    check(gt::parse_engine_status("state=ready\nimu=1\nmag=locked\nmag_error_deg=0.62\nmag_offset_ut=1.25\n",
                                  mag_status, error) &&
              mag_status.mag == "locked" && std::fabs(mag_status.mag_error_deg - 0.62) < 1e-9 &&
              std::fabs(mag_status.mag_offset_ut - 1.25) < 1e-9,
          "mag, mag_error_deg and mag_offset_ut are read back");
    check(lock_of("state=ready\nimu=1\nmag=off\n") == gt::HeadingLockState::Off &&
              lock_of("state=ready\nimu=1\nmag=acquiring\n") == gt::HeadingLockState::Acquiring &&
              lock_of("state=ready\nimu=1\nmag=locked\n") == gt::HeadingLockState::Locked &&
              lock_of("state=ready\nimu=1\nmag=disturbed\n") == gt::HeadingLockState::Disturbed &&
              lock_of("state=ready\nimu=1\nmag=relearning\n") == gt::HeadingLockState::Relearning,
          "every engine mag state maps to its badge state");
    check(lock_of("state=ready\nimu=1\n") == gt::HeadingLockState::Unknown &&
              lock_of("state=ready\nimu=0\nmag=off\n") == gt::HeadingLockState::Unknown &&
              lock_of("state=ready\nimu=1\nmag=spinning\n") == gt::HeadingLockState::Unknown &&
              gt::heading_lock_from_status(gt::EngineStatusFile{}) == gt::HeadingLockState::Unknown,
          "an older engine, a fixed camera, an unknown value or no status reads as Not reported");
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
        const std::filesystem::path legacy = directory / "rayneo-legacy-app.json";
        {
            std::ofstream out(legacy, std::ios::binary | std::ios::trunc);
            out << "{\"version\": 1, \"layout_path\": \"a\", \"calibration_path\": \"b\", "
                   "\"log_dir\": \"c\", \"monitor_index\": -1, \"health_poll_ms\": 1000, "
                   "\"preview_without_head_tracking\": false, \"close_to_tray\": false, "
                   "\"engine_log\": {\"max_bytes\": 1048576, \"max_files\": 3}, "
                   "\"telemetry_log\": {\"max_bytes\": 4194304, \"max_files\": 3}}";
        }
        gt::AppConfig loaded;
        check(gt::load_app_config(legacy, loaded, error) && loaded.splits.column < 0.0f &&
                  loaded.splits.left[0] < 0.0f,
              "a legacy file without splits (and with the retired log-rotation keys) loads");
        check(gt::app_config_to_json_text(loaded).find("engine_log") == std::string::npos,
              "the retired log-rotation keys are dropped on the next save");
        std::error_code remove_error;
        std::filesystem::remove(legacy, remove_error);
    }
    {
        // Malformed splits fail the load instead of crashing the dashboard.
        const std::filesystem::path broken = directory / "rayneo-broken-app.json";
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
    const float two_metre_position =
        gt::layout_slider_fraction(gt::LayoutField::Distance, 2.0f);
    check(two_metre_position > 0.4f && two_metre_position < 0.55f &&
              std::fabs(gt::layout_slider_value(gt::LayoutField::Distance,
                                                two_metre_position) - 2.0f) < 1e-4f,
          "depth slider gives typical screen distances room for precise adjustment");
    check(std::fabs(gt::layout_slider_value(gt::LayoutField::Distance, 0.0f) - 0.25f) < 1e-4f &&
              std::fabs(gt::layout_slider_value(gt::LayoutField::Distance, 1.0f) - 20.0f) < 1e-3f,
          "depth slider keeps the full supported range");
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

    // User-facing angles read as degrees with a real minus sign (U+2212);
    // before 2026-11-13 the formatter printed "-45 deg" / "0 deg".
    check(gt::format_layout_field_value(gt::LayoutField::Yaw, -45.0f) == "−45°" &&
              gt::format_layout_field_value(gt::LayoutField::Fov, 46.0f) == "46°" &&
              gt::format_layout_field_value(gt::LayoutField::Pitch, -0.4f) == "0°" &&
              gt::format_layout_field_value(gt::LayoutField::LeaveDeg, 12.6f) == "13°",
          "angle fields format as whole degrees with the degree sign and no -0");
    check(gt::format_layout_field_value(gt::LayoutField::Distance, 2.0f) == "2.00 m" &&
              gt::format_layout_field_value(gt::LayoutField::ActiveFps, 60.0f) == "60 fps",
          "distance and frame-rate fields keep their units");
    gt::Layout triple;
    triple.fov_deg = 46.0f;
    for (const float yaw : {-45.0f, 0.0f, 45.0f}) {
        gt::ScreenLayout s;
        s.yaw_deg = yaw;
        triple.screens.push_back(s);
    }
    triple.capture_policy.active_fps = 120;
    triple.capture_policy.mid_fps = 30;
    triple.capture_policy.idle_fps = 1;
    check(gt::layout_display_summary(triple) ==
              "3 screens  ·  arc −45° to 45°  ·  46° field of view  ·  "
              "120/30/1 fps capture",
          "the page layout summary is plain words with degree signs");
    check(gt::layout_summary(triple) == "3 screens | arc -45..45 deg | 46 deg fov | 120/30/1 fps",
          "the Diagnostics layout summary keeps its technical form");
    gt::Layout single = triple;
    single.screens.resize(1);
    single.screens[0].yaw_deg = 0.0f;
    check(gt::layout_display_summary(single).rfind("1 screen  ·  at 0°  ·  ", 0) == 0 &&
              gt::layout_display_summary(gt::Layout{}) == "No screens",
          "one screen is singular and has a position, not an arc");
}

bool file_contains(const std::filesystem::path& path, const std::string& needle) {
    return read_text(path).find(needle) != std::string::npos;
}

void write_session_files(const std::filesystem::path& log_dir, const std::string& tag) {
    for (const std::string& name : gt::session_log_names()) {
        check(write_text(log_dir / name, tag + " " + name + "\n" + std::string(4096, 'x')),
              "session file written");
    }
}

std::vector<std::string> archive_listing(const std::filesystem::path& archive_dir) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(archive_dir, error)) {
        names.push_back(gt::utf8_from_path(entry.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

void test_session_logs(const std::filesystem::path& directory) {
    std::printf("app_selftest: per-session log archive\n");
    check(!gt::session_qualifies_for_archive(0.0) && !gt::session_qualifies_for_archive(599.0) &&
              !gt::session_qualifies_for_archive(600.0),
          "sessions of 10 minutes or less are not archived");
    check(gt::session_qualifies_for_archive(600.5) && gt::session_qualifies_for_archive(7200.0),
          "sessions over 10 minutes are archived");

    std::tm start{};
    start.tm_year = 2026 - 1900;
    start.tm_mon = 8;
    start.tm_mday = 25;
    start.tm_hour = 14;
    start.tm_min = 3;
    start.tm_sec = 7;
    const std::string stem = gt::session_archive_stem(start);
    check(stem == "session-2026-09-25_14-03-07", "the archive stem is the local start time");
    check(gt::is_session_archive_name(stem + ".zip"), "a published archive name is recognised");
    check(!gt::is_session_archive_name(stem + ".zip.partial") &&
              !gt::is_session_archive_name("." + stem + ".partial") &&
              !gt::is_session_archive_name(".pending-" + stem) &&
              !gt::is_session_archive_name("reference-2026-09-25.zip") &&
              !gt::is_session_archive_name("session-2026-9-25_14-03-07.zip") &&
              !gt::is_session_archive_name("session-2026-09-25_14-03-0x.zip"),
          "temporary, pending, reference and malformed names are never treated as archives");

    const std::vector<std::string> five = {
        "session-2026-09-27_10-00-00.zip", "notes.txt", "session-2026-09-25_10-00-00.zip",
        "session-2026-09-29_10-00-00.zip", "reference-2026-09-25.zip",
        "session-2026-09-26_10-00-00.zip", "session-2026-09-28_10-00-00.zip"};
    const std::vector<std::string> pruned = gt::session_archives_to_prune(five, 3);
    check(pruned.size() == 2 && pruned[0] == "session-2026-09-25_10-00-00.zip" &&
              pruned[1] == "session-2026-09-26_10-00-00.zip",
          "pruning removes only the oldest archives beyond the newest three");
    check(gt::session_archives_to_prune({"session-2026-09-25_10-00-00.zip"}, 3).empty(),
          "nothing is pruned while at most three archives exist");

    const std::filesystem::path log_dir = directory / "session-logs";
    std::error_code fs_error;
    std::filesystem::remove_all(log_dir, fs_error);
    std::filesystem::create_directories(log_dir, fs_error);
    const std::filesystem::path archive_dir = gt::session_archive_dir(log_dir);

    // A new session starts on empty files, including legacy rotated ones.
    write_session_files(log_dir, "old");
    check(write_text(log_dir / "engine.log.1", "legacy") &&
              write_text(log_dir / "telemetry.csv.3", "legacy"),
          "legacy rotated files written");
    gt::clear_session_logs(log_dir);
    bool any_left = false;
    for (const auto& entry : std::filesystem::directory_iterator(log_dir, fs_error)) {
        (void)entry;
        any_left = true;
    }
    check(!any_left, "clearing removes the live files and legacy rotations");

    std::string error;
    check(!gt::stage_session_logs(log_dir, stem, error) && error.empty() &&
              !gt::has_pending_session_logs(archive_dir),
          "a session without files stages nothing");

    // The user's reference archive sits beside the rolling ones and survives.
    std::filesystem::create_directories(archive_dir, fs_error);
    check(write_text(archive_dir / "reference-2026-09-25.zip", "reference"),
          "reference archive written");

    // Five sessions through the real helper body (tar.exe included).
    for (int day = 1; day <= 5; ++day) {
        start.tm_mday = day;
        const std::string session = gt::session_archive_stem(start);
        write_session_files(log_dir, "day" + std::to_string(day));
        check(gt::stage_session_logs(log_dir, session, error) && error.empty(),
              "the session's files move to the pending folder");
        check(!std::filesystem::exists(log_dir / "imu_raw.csv") &&
                  gt::has_pending_session_logs(archive_dir),
              "the log directory is free for the next session at once");
        check(gt::run_session_archiver(archive_dir) == 0, "the archiver drains the queue");
        check(!gt::has_pending_session_logs(archive_dir), "no pending folder is left");
    }
    const std::vector<std::string> listing = archive_listing(archive_dir);
    check(listing.size() == 4 && listing[0] == "reference-2026-09-25.zip" &&
              listing[1] == "session-2026-09-03_14-03-07.zip" &&
              listing[2] == "session-2026-09-04_14-03-07.zip" &&
              listing[3] == "session-2026-09-05_14-03-07.zip",
          "a rolling three: sessions 1 and 2 are pruned, the reference and no temporaries remain");
    const std::filesystem::path newest = archive_dir / "session-2026-09-05_14-03-07.zip";
    const std::string bytes = read_text(newest);
    check(bytes.size() > 4 && bytes.compare(0, 4, std::string("PK\x03\x04", 4)) == 0,
          "the archive is a real zip");
    check(file_contains(newest, "imu_raw.csv") && file_contains(newest, "engine.log") &&
              file_contains(newest, "telemetry.csv") && file_contains(newest, "engine-status.txt"),
          "the archive holds every session file");
    check(bytes.size() < 4u * 4096u, "the archive is compressed");

    check(gt::run_session_archiver(archive_dir) == 0, "an idle archiver exits cleanly");

    // A helper busy with ANOTHER log folder (a second copy of the app, or a
    // concurrent test run) must not turn this folder's helper away: the queue
    // lock is per directory. With one machine-wide name the drain below was a
    // silent no-op and the pending folder stayed behind.
    {
        const std::filesystem::path other_archive = directory / "other-logs" / "sessions";
        const std::wstring own_name = gt::session_archiver_mutex_name(archive_dir);
        const std::wstring other_name = gt::session_archiver_mutex_name(other_archive);
        const std::filesystem::path respelled =
            gt::path_from_utf8(gt::utf8_from_path(archive_dir) + "\\.\\");
        check(own_name != other_name && own_name == gt::session_archiver_mutex_name(respelled) &&
                  own_name.rfind(L"Local\\", 0) == 0 && own_name.find(L'\\', 6) == std::wstring::npos,
              "the archiver lock is named per directory (one folder, one name)");
        HANDLE foreign = CreateMutexW(nullptr, TRUE, other_name.c_str());
        start.tm_mday = 6;
        write_session_files(log_dir, "day6");
        check(gt::stage_session_logs(log_dir, gt::session_archive_stem(start), error) && error.empty(),
              "a sixth session is staged while another folder's helper runs");
        check(gt::run_session_archiver(archive_dir) == 0 && !gt::has_pending_session_logs(archive_dir),
              "another folder's busy helper does not block this folder's queue");
        if (foreign != nullptr) {
            ReleaseMutex(foreign);
            CloseHandle(foreign);
        }
    }
    std::filesystem::remove_all(log_dir, fs_error);

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
    const gt::OrbitPoint corner = gt::orbit_project(view, camera, quad.corners[2], rect);
    const int inner_x = centre.pixel.x + (corner.pixel.x - centre.pixel.x) * 3 / 4;
    const int inner_y = centre.pixel.y + (corner.pixel.y - centre.pixel.y) * 3 / 4;
    check(gt::orbit_hit_test(layout, view, camera, rect, inner_x, inner_y, 14) == 0,
          "tapping inside a screen away from its centre selects it");
    gt::ScreenLayout near_screen = layout.screens[0];
    near_screen.distance_m = 1.0f;
    near_screen.id = "near";
    layout.screens.insert(layout.screens.begin(), near_screen);
    check(gt::orbit_hit_test(layout, view, camera, rect, centre.pixel.x, centre.pixel.y, 14) == 0,
          "overlapping screens select the one nearest the editor camera");
    check(gt::orbit_hit_test(layout, view, camera, rect, 0, 0, 14) < 0,
          "tapping empty space selects nothing");
    gt::OrbitView angled_view;
    angled_view.yaw_deg = -30.0f;
    angled_view.pitch_deg = 18.0f;
    const gt::OrbitCamera angled_camera = gt::orbit_camera(angled_view);
    gt::ScreenLayout dragged = layout.screens[1];
    const gt::OrbitPoint before_drag = gt::orbit_project(
        angled_view, angled_camera, gt::orbit_screen_quad(dragged).center, rect);
    const gt::OrbitDragAngles dragged_angles = gt::orbit_drag_angles(
        dragged, angled_view, angled_camera, rect, 30, -20);
    dragged.yaw_deg = dragged_angles.yaw_deg;
    dragged.pitch_deg = dragged_angles.pitch_deg;
    dragged.distance_m = dragged_angles.distance_m;
    const gt::OrbitPoint after_drag = gt::orbit_project(
        angled_view, angled_camera, gt::orbit_screen_quad(dragged).center, rect);
    check(std::abs(after_drag.pixel.x - before_drag.pixel.x - 30) <= 3 &&
              std::abs(after_drag.pixel.y - before_drag.pixel.y + 20) <= 3,
          "dragging a screen in an angled 3D view follows the pointer");
    bool drag_consistent = true;
    gt::Layout drag_layout = gt::preset_layout(gt::LayoutPreset::TripleArc);
    near_screen.vdd_index = 7;
    drag_layout.screens.push_back(near_screen);
    for (const float camera_yaw : {-45.0f, 0.0f, 45.0f}) {
        for (const float camera_pitch : {0.0f, 18.0f, 40.0f}) {
            gt::OrbitView sample_view;
            sample_view.yaw_deg = camera_yaw;
            sample_view.pitch_deg = camera_pitch;
            const gt::OrbitCamera sample_camera = gt::orbit_camera(sample_view);
            for (const gt::ScreenLayout& original : drag_layout.screens) {
                const gt::OrbitPoint start = gt::orbit_project(
                    sample_view, sample_camera, gt::orbit_screen_quad(original).center, rect);
                if (start.behind) continue;
                for (const int dx : {-15, 15}) {
                    for (const int dy : {-10, 10}) {
                        gt::ScreenLayout moved = original;
                        const gt::OrbitDragAngles moved_angles = gt::orbit_drag_angles(
                            original, sample_view, sample_camera, rect, dx, dy);
                        moved.yaw_deg = moved_angles.yaw_deg;
                        moved.pitch_deg = moved_angles.pitch_deg;
                        moved.distance_m = moved_angles.distance_m;
                        const gt::OrbitPoint finish = gt::orbit_project(
                            sample_view, sample_camera, gt::orbit_screen_quad(moved).center, rect);
                        const bool follows = !finish.behind &&
                            std::abs(finish.pixel.x - start.pixel.x - dx) <= 4 &&
                            std::abs(finish.pixel.y - start.pixel.y - dy) <= 4;
                        drag_consistent &= follows;
                    }
                }
            }
        }
    }
    check(drag_consistent, "small 3D drags stay under the pointer across orbit views");
}

}  // namespace

void test_view_comfort(const std::filesystem::path& directory) {
    std::printf("app_selftest: view comfort (stabilisation, dimming, night tint)\n");
    std::string error;
    const gt::ViewComfort defaults;
    check(gt::validate_view_comfort(defaults, error), "default view comfort validates");
    check(defaults.stabilise_level == 2 && defaults.dim_mode == gt::DimMode::Off &&
              !defaults.night_tint,
          "defaults: medium stabilisation, dimming and night tint off");
    std::vector<std::string> args;
    gt::append_view_comfort_args(defaults, args);
    check(args.empty(), "default comfort adds no engine switches (historical command line)");
    check(std::string(gt::stabilise_level_name(4)) == "ultra" &&
              std::string(gt::stabilise_level_name(0)) == "off",
          "stabilisation names cover off..ultra");

    gt::ViewComfort bad = defaults;
    bad.stabilise_level = 5;
    check(!gt::validate_view_comfort(bad, error), "stabilisation beyond ultra is rejected");
    bad = defaults;
    bad.screen_brightness_pct[3] = 5;
    check(!gt::validate_view_comfort(bad, error), "brightness below the 10 % floor is rejected");
    bad = defaults;
    bad.night_tint_pct = 101;
    check(!gt::validate_view_comfort(bad, error), "night tint above 100 % is rejected");

    // Every non-default setting survives the engine command line.
    auto round_trip = [&](const gt::ViewComfort& in, gt::ViewComfort& out) {
        std::vector<std::string> list;
        gt::append_view_comfort_args(in, list);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("spatial_desk"));
        for (std::string& item : list) {
            argv.push_back(item.data());
        }
        out = gt::ViewComfort{};
        for (int i = 1; i < static_cast<int>(argv.size()); ++i) {
            bool handled = false;
            if (!gt::parse_view_comfort_arg(static_cast<int>(argv.size()), argv.data(), i, out,
                                            handled, error) ||
                !handled) {
                return false;
            }
        }
        return true;
    };
    gt::ViewComfort manual;
    manual.stabilise_level = 4;
    manual.dim_mode = gt::DimMode::Manual;
    manual.screen_brightness_pct = {60, 100, 35, 100, 100, 100, 100, 10};
    manual.night_tint = true;
    manual.night_tint_pct = 70;
    gt::ViewComfort parsed;
    check(round_trip(manual, parsed) && parsed == manual,
          "ultra + manual dimming + night tint round-trip through the engine switches");
    gt::ViewComfort focus;
    focus.stabilise_level = 3;
    focus.dim_mode = gt::DimMode::Focus;
    focus.focus_dim_pct = 25;
    check(round_trip(focus, parsed) && parsed.dim_mode == gt::DimMode::Focus &&
              parsed.focus_dim_pct == 25 && parsed.stabilise_level == 3 && !parsed.night_tint,
          "focus dimming round-trips");
    {
        char name[] = "--screen-brightness";
        char value[] = "50,abc";
        char* argv[] = {name, value};
        int index = 0;
        bool handled = false;
        gt::ViewComfort target;
        check(!gt::parse_view_comfort_arg(2, argv, index, target, handled, error) && handled,
              "malformed brightness list is rejected");
    }

    // Maths the renderer uses.
    const gt::Rgb neutral = gt::night_tint_rgb(0);
    const gt::Rgb warm = gt::night_tint_rgb(100);
    check(neutral.r == 1.0f && neutral.g == 1.0f && neutral.b == 1.0f, "0 % tint is neutral");
    check(warm.r == 1.0f && warm.g < 1.0f && warm.b < warm.g && warm.b > 0.2f,
          "full tint keeps red, cuts green a little and blue most");
    check(gt::comfort_tint(defaults).b == 1.0f, "tint off is neutral whatever its strength");
    const std::vector<std::pair<float, float>> triple = {{-45.0f, 0.0f}, {0.0f, 0.0f}, {45.0f, 0.0f}};
    check(gt::comfort_focused_screen(triple, 40.0f, 5.0f) == 2 &&
              gt::comfort_focused_screen(triple, -3.0f, -10.0f) == 1 &&
              gt::comfort_focused_screen(triple, -30.0f, 0.0f) == 0 &&
              gt::comfort_focused_screen({}, 0.0f, 0.0f) == -1,
          "focused screen is the nearest centre");
    check(gt::comfort_target_brightness(focus, 0, 1) == 0.25f &&
              gt::comfort_target_brightness(focus, 1, 1) == 1.0f &&
              gt::comfort_target_brightness(focus, 0, -1) == 1.0f,
          "focus dimming keeps the looked-at screen bright");
    check(gt::comfort_target_brightness(manual, 2, 1) == 0.35f &&
              gt::comfort_target_brightness(defaults, 2, 1) == 1.0f,
          "manual brightness per screen; off is full brightness");
    auto fade_for = [](int hz) {
        float value = 1.0f;
        for (int i = 0; i < hz / 2; ++i) {
            value = gt::comfort_fade(value, 0.2f, 1.0f / static_cast<float>(hz));
        }
        return value;
    };
    // Two time constants (0.5 s at tau 0.25 s): 0.2 + 0.8 * e^-2, at any frame rate.
    const float expected_fade = 0.2f + 0.8f * std::exp(-2.0f);
    check(std::fabs(fade_for(30) - expected_fade) < 1e-3f &&
              std::fabs(fade_for(120) - expected_fade) < 1e-3f,
          "focus fade follows its time constant at any frame rate");

    // Persistence: survives app.json, and a file without the block loads defaults.
    gt::AppConfig config;
    config.comfort = manual;
    const std::filesystem::path path = directory / "comfort-app.json";
    gt::AppConfig loaded;
    check(gt::save_app_config(path, config, error) && gt::load_app_config(path, loaded, error) &&
              loaded.comfort == manual,
          "view comfort persists in app.json");
    {
        std::ofstream legacy(directory / "comfort-legacy.json");
        legacy << "{\"version\": 1}\n";
    }
    check(gt::load_app_config(directory / "comfort-legacy.json", loaded, error) &&
              loaded.comfort == gt::ViewComfort{},
          "an app.json written before view comfort loads the defaults");
    {
        std::ofstream broken(directory / "comfort-broken.json");
        broken << "{\"version\": 1, \"view_comfort\": {\"stabilise\": \"max\"}}\n";
    }
    check(!gt::load_app_config(directory / "comfort-broken.json", loaded, error),
          "an unknown stabilisation name is rejected");

    // The launch command carries non-default comfort, validated first.
    gt::EngineLaunchCommand command;
    check(gt::build_engine_launch_command("C:\\s\\spatial_desk.exe", "C:\\s", "l.json", "o.json",
                                          "", "", "", gt::LaunchMode::Workspace, true, -1, 0.0f,
                                          command, error, focus) &&
              std::find(command.arguments.begin(), command.arguments.end(), "--dim-mode") !=
                  command.arguments.end(),
          "launch command carries the comfort switches");
    check(!gt::build_engine_launch_command("C:\\s\\spatial_desk.exe", "C:\\s", "l.json", "o.json",
                                           "", "", "", gt::LaunchMode::Workspace, true, -1, 0.0f,
                                           command, error, bad),
          "launch refuses invalid comfort settings");
}

void test_ui_model(const std::filesystem::path& directory) {
    std::printf("app_selftest: pages, theme and engine-option preferences\n");
    bool pages_round_trip = true;
    std::set<std::string> keys;
    for (int i = 0; i < gt::kAppPageCount; ++i) {
        const auto page = static_cast<gt::AppPage>(i);
        gt::AppPage parsed = gt::AppPage::Count;
        pages_round_trip = pages_round_trip && gt::parse_app_page(gt::app_page_key(page), parsed) &&
                           parsed == page && std::wcslen(gt::app_page_label(page)) > 0 &&
                           std::wcslen(gt::app_page_title(page)) > 0 &&
                           std::wcslen(gt::app_page_subtitle(page)) > 20;
        keys.insert(gt::app_page_key(page));
    }
    check(pages_round_trip, "every page has a key, label, title and description that round-trip");
    check(keys.size() == static_cast<size_t>(gt::kAppPageCount), "page keys are distinct");
    gt::AppPage page = gt::AppPage::Layout;
    check(!gt::parse_app_page("nonsense", page) && page == gt::AppPage::Layout,
          "an unknown page key is refused and leaves the value alone");

    gt::ThemeMode mode = gt::ThemeMode::Dark;
    check(gt::parse_theme_mode("Light", mode) && mode == gt::ThemeMode::Light &&
              gt::parse_theme_mode("system", mode) && mode == gt::ThemeMode::System &&
              !gt::parse_theme_mode("sepia", mode),
          "theme names parse case-insensitively and reject unknown names");
    check(gt::theme_is_dark(gt::ThemeMode::Dark, true) &&
              !gt::theme_is_dark(gt::ThemeMode::Light, false) &&
              gt::theme_is_dark(gt::ThemeMode::System, false) &&
              !gt::theme_is_dark(gt::ThemeMode::System, true),
          "System follows the Windows app theme; Dark and Light are fixed");

    gt::HeadingLockState lock = gt::HeadingLockState::Unknown;
    check(gt::parse_heading_lock_state("locked", lock) && lock == gt::HeadingLockState::Locked &&
              gt::parse_heading_lock_state("Disturbed", lock) &&
              lock == gt::HeadingLockState::Disturbed &&
              gt::parse_heading_lock_state("relearning", lock) &&
              lock == gt::HeadingLockState::Relearning &&
              gt::parse_heading_lock_state("off", lock) && lock == gt::HeadingLockState::Off &&
              gt::parse_heading_lock_state("acquiring", lock) && lock == gt::HeadingLockState::Acquiring &&
              !gt::parse_heading_lock_state("spinning", lock),
          "heading-lock states parse; unknown words are refused");
    check(std::wcslen(gt::heading_lock_detail(gt::HeadingLockState::Locked)) > 30 &&
              std::wstring(gt::heading_lock_label(gt::HeadingLockState::Unknown)) == L"Not reported",
          "heading-lock states have user-facing wording");

    // Persistence: defaults, round trip, legacy files and bad values.
    gt::AppConfig defaults;
    check(!defaults.onboarding_complete && defaults.theme == gt::ThemeMode::Dark &&
              defaults.last_page == gt::AppPage::Home && defaults.engine.predict_ms == 35 &&
              !defaults.engine.mag_track_hard_iron,
          "new UI preferences default to: guide pending, dark theme, Home page, prediction 35 ms, "
          "hard-iron tracking off");
    gt::AppConfig custom;
    custom.onboarding_complete = true;
    custom.theme = gt::ThemeMode::System;
    custom.last_page = gt::AppPage::Diagnostics;
    custom.engine.predict_ms = 20;
    custom.engine.mag_track_hard_iron = true;
    std::string error;
    gt::AppConfig loaded;
    check(gt::save_app_config(directory / "ui-prefs.json", custom, error) &&
              gt::load_app_config(directory / "ui-prefs.json", loaded, error) &&
              loaded.onboarding_complete && loaded.theme == gt::ThemeMode::System &&
              loaded.last_page == gt::AppPage::Diagnostics && loaded.engine.predict_ms == 20 &&
              loaded.engine.mag_track_hard_iron,
          "guide completion, theme, last page and engine options round-trip through app.json");
    write_text(directory / "ui-legacy.json", "{\"version\": 1, \"close_to_tray\": false}\n");
    check(gt::load_app_config(directory / "ui-legacy.json", loaded, error) &&
              !loaded.onboarding_complete && loaded.theme == gt::ThemeMode::Dark &&
              loaded.last_page == gt::AppPage::Home && !loaded.close_to_tray,
          "an app.json written before the redesign loads with the guide pending");
    write_text(directory / "ui-badpage.json", "{\"version\": 1, \"last_page\": \"future\"}\n");
    check(gt::load_app_config(directory / "ui-badpage.json", loaded, error) &&
              loaded.last_page == gt::AppPage::Home,
          "an unknown last_page falls back to Home instead of rejecting the file");
    write_text(directory / "ui-badtheme.json", "{\"version\": 1, \"theme\": \"neon\"}\n");
    check(!gt::load_app_config(directory / "ui-badtheme.json", loaded, error) &&
              error.find("theme") != std::string::npos,
          "an unknown theme is rejected with a reason");
    write_text(directory / "ui-badopts.json",
               "{\"version\": 1, \"engine_options\": {\"motion_prediction\": 1}}\n");
    check(!gt::load_app_config(directory / "ui-badopts.json", loaded, error),
          "a non-boolean engine option is rejected");
    check(gt::load_app_config(directory / "ui-legacy.json", loaded, error) && loaded.engine.predict_ms == 35 &&
              !loaded.engine.mag_track_hard_iron,
          "an app.json without engine options loads with prediction 35 ms and tracking off");
    write_text(directory / "ui-oldopts.json",
               "{\"version\": 1, \"engine_options\": {\"motion_prediction\": false}}\n");
    check(gt::load_app_config(directory / "ui-oldopts.json", loaded, error) && loaded.engine.predict_ms == 35,
          "the never-wired motion_prediction switch is ignored: old files get the 35 ms default");
    write_text(directory / "ui-predict.json",
               "{\"version\": 1, \"engine_options\": {\"predict_ms\": 250, \"mag_track_hard_iron\": true}}\n");
    check(gt::load_app_config(directory / "ui-predict.json", loaded, error) && loaded.engine.predict_ms == 100 &&
              loaded.engine.mag_track_hard_iron,
          "a hand-edited prediction above the engine range is clamped to 100 ms");
    write_text(directory / "ui-predict-bad.json",
               "{\"version\": 1, \"engine_options\": {\"predict_ms\": \"fast\"}}\n");
    check(!gt::load_app_config(directory / "ui-predict-bad.json", loaded, error),
          "a non-integer prediction horizon is rejected");
}

void test_calibration_progress() {
    std::printf("app_selftest: calibration progress protocol\n");
    gt::CalProgressEvent event;
    std::string error;
    check(gt::parse_cal_progress_line(
              "@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":5,\"state\":\"recording\","
              "\"prompt\":\"Look left\",\"detail\":\"hold still\",\"quality\":0.75}\r\n",
              event, error) == gt::CalLineKind::Progress &&
              event.flow == gt::CalFlow::Orientation && event.step == 2 && event.steps == 5 &&
              event.state == gt::CalStepState::Recording && event.prompt == "Look left" &&
              event.detail == "hold still" && std::fabs(event.quality - 0.75f) < 1e-6f,
          "a full progress line parses every field (CRLF tolerated)");
    check(gt::parse_cal_progress_line("@@progress {\"flow\":\"mag\",\"state\":\"waiting\"}", event,
                                      error) == gt::CalLineKind::Progress &&
              event.flow == gt::CalFlow::Mag && event.step == 0 && event.steps == 0 &&
              event.prompt.empty() && event.quality < 0.0f,
          "missing optional fields take their defaults");
    check(gt::parse_cal_progress_line("@@progress {\"flow\":\"mag\",\"state\":\"ok\","
                                      "\"quality\":-1}",
                                      event, error) == gt::CalLineKind::Progress &&
              event.quality < 0.0f,
          "quality -1 means no figure");
    check(gt::parse_cal_progress_line("@@progress {\"flow\":\"mag\",\"state\":\"ok\","
                                      "\"quality\":3.5,\"step\":9,\"steps\":4}",
                                      event, error) == gt::CalLineKind::Progress &&
              event.quality < 0.0f && event.step == 4,
          "out-of-range quality reads as none and step is clamped to steps");
    check(gt::parse_cal_progress_line("Press ENTER when ready", event, error) ==
                  gt::CalLineKind::NotProgress &&
              gt::parse_cal_progress_line("", event, error) == gt::CalLineKind::NotProgress &&
              gt::parse_cal_progress_line("@@progressive scan", event, error) ==
                  gt::CalLineKind::NotProgress,
          "ordinary console lines are not progress");
    const char* const malformed[] = {
        "@@progress",
        "@@progress {not json",
        "@@progress [1,2]",
        "@@progress {\"state\":\"ok\"}",
        "@@progress {\"flow\":\"orientation\"}",
        "@@progress {\"flow\":\"compass\",\"state\":\"ok\"}",
        "@@progress {\"flow\":\"mag\",\"state\":\"thinking\"}",
        "@@progress {\"flow\":\"mag\",\"state\":\"ok\",\"step\":\"2\"}",
        "@@progress {\"flow\":\"mag\",\"state\":\"ok\",\"prompt\":5}",
        "@@progress {\"flow\":\"mag\",\"state\":\"ok\",\"quality\":\"high\"}",
    };
    bool all_malformed = true;
    for (const char* line : malformed) {
        const bool rejected =
            gt::parse_cal_progress_line(line, event, error) == gt::CalLineKind::Malformed &&
            !error.empty();
        if (!rejected) {
            std::printf("    not rejected: %s\n", line);
        }
        all_malformed = all_malformed && rejected;
    }
    check(all_malformed, "malformed progress lines (bad JSON, missing/invalid flow or state, "
                         "wrong field types) are rejected with a reason");

    gt::CalRunState run;
    run.begin(gt::CalFlow::Orientation);
    check(run.running() && !run.awaiting_user() && run.fraction() == 0.0f,
          "a started run is running with no progress");
    run.consume_line("RayNeo GT orientation calibration");
    run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":1,\"steps\":4,"
                     "\"state\":\"waiting\",\"prompt\":\"Sit upright\"}");
    check(run.awaiting_user() && run.prompt == "Sit upright" && run.recent_output.size() == 1,
          "a waiting event asks the wearer to continue; console text is kept aside");
    run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":1,\"steps\":4,"
                     "\"state\":\"recording\"}");
    check(!run.awaiting_user() && run.prompt == "Sit upright" &&
              std::fabs(run.fraction() - 0.125f) < 1e-6f,
          "recording keeps the last prompt and counts half a step");
    run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":3,\"steps\":4,"
                     "\"state\":\"ok\",\"quality\":0.9}");
    check(std::fabs(run.fraction() - 0.75f) < 1e-6f && run.quality > 0.89f,
          "an ok step completes that step");
    run.consume_line("@@progress {broken");
    check(run.malformed_lines == 1 && run.running(), "a malformed line is counted, not fatal");
    run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":4,\"steps\":4,"
                     "\"state\":\"done\",\"prompt\":\"Saved\"}");
    run.process_exited(0);
    check(run.phase == gt::CalRunState::Phase::Succeeded && run.fraction() == 1.0f,
          "done finishes the run; the later exit does not overturn it");

    gt::CalRunState failed;
    failed.begin(gt::CalFlow::Mag);
    failed.consume_line("@@progress {\"flow\":\"mag\",\"step\":2,\"steps\":3,\"state\":\"failed\","
                        "\"detail\":\"Field too weak\"}");
    check(failed.phase == gt::CalRunState::Phase::Failed && failed.failure == "Field too weak",
          "a failed event fails the run with the tool's reason");

    gt::CalRunState silent;
    silent.begin(gt::CalFlow::Orientation);
    silent.consume_line("unknown or incomplete option: --progress-json");
    silent.process_exited(1);
    check(silent.phase == gt::CalRunState::Phase::Failed &&
              silent.failure.find("exit code 1") != std::string::npos &&
              silent.failure.find("--progress-json") != std::string::npos &&
              !silent.recent_output.empty(),
          "an exit without done/failed is a clear failure naming the exit code");

    gt::CalRunState partial;
    partial.begin(gt::CalFlow::Orientation);
    partial.consume_line("@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,"
                         "\"state\":\"recording\"}");
    partial.process_exited(3);
    check(partial.phase == gt::CalRunState::Phase::Failed &&
              partial.failure.find("--progress-json") == std::string::npos,
          "a mid-run exit fails without blaming the protocol");

    gt::CalRunState cancelled;
    cancelled.begin(gt::CalFlow::Orientation);
    cancelled.cancel();
    cancelled.process_exited(1);
    check(cancelled.phase == gt::CalRunState::Phase::Cancelled,
          "a cancelled run stays cancelled when the tool then exits");

    gt::CalRunState noisy;
    noisy.begin(gt::CalFlow::Orientation);
    for (int i = 0; i < 40; ++i) {
        noisy.consume_line("line " + std::to_string(i));
    }
    check(noisy.recent_output.size() == gt::CalRunState::kRecentLines &&
              noisy.recent_output.back() == "line 39",
          "recent console output is bounded to the newest lines");

    // In-app runs use the tool's --auto mode (orientation_calibrate.cpp).
    const std::vector<std::string> head_args =
        gt::calibration_tool_arguments(gt::CalFlow::Orientation, "C:\\c\\orientation.json", true);
    check(head_args == std::vector<std::string>{"--progress-json", "--auto", "--auto-delay", "5", "--repeats", "3",
                                                "--output", "C:\\c\\orientation.json"},
          "head calibration runs --progress-json --auto --auto-delay 5 --repeats 3 --output FILE (no --earbuds)");
    check(gt::calibration_tool_arguments(gt::CalFlow::Mag, "o.json", false) ==
                  std::vector<std::string>{"--progress-json", "--auto", "--auto-delay", "5", "--mag", "--output",
                                           "o.json"} &&
              gt::calibration_tool_arguments(gt::CalFlow::Mag, "o.json", true) ==
                  std::vector<std::string>{"--progress-json", "--auto", "--auto-delay", "5", "--mag", "--earbuds",
                                           "--output", "o.json"},
          "compass calibration adds --mag, and --earbuds only when Earbuds in is on");

    gt::CalRunState timed;
    timed.begin(gt::CalFlow::Orientation, true, 5.0);
    check(timed.countdown_s(0.0) < 0.0, "no countdown before the tool reports a waiting step");
    timed.consume_line(
        "@@progress {\"flow\":\"orientation\",\"step\":1,\"steps\":4,\"state\":\"waiting\",\"prompt\":\"Still\"}",
        10.0);
    check(std::fabs(timed.countdown_s(12.0) - 3.0) < 1e-9 && timed.countdown_s(20.0) == 0.0,
          "a waiting step counts down the --auto delay from when it arrived");
    timed.consume_line(
        "@@progress {\"flow\":\"orientation\",\"step\":1,\"steps\":4,\"state\":\"recording\",\"prompt\":\"Still\"}",
        15.0);
    check(timed.countdown_s(16.0) < 0.0, "recording has no countdown");
    timed.consume_line("@@progress {\"flow\":\"orientation\",\"step\":1,\"steps\":4,\"state\":\"retry\","
                       "\"prompt\":\"Still\",\"detail\":\"HOLD_STILL: 1.80 deg/s of movement (limit 1.5)\",\"quality\":0}",
                       19.0);
    timed.consume_line(
        "@@progress {\"flow\":\"orientation\",\"step\":1,\"steps\":4,\"state\":\"waiting\",\"prompt\":\"Still\"}",
        19.01);
    // Shown text uses the degree sign (was "1.80 deg/s ..." before the 2026-11-13 wording pass).
    check(timed.retry_reason == "1.80°/s of movement (limit 1.5)" &&std::fabs(timed.countdown_s(20.01) - 4.0) < 1e-6,
          "a retry reason survives the next waiting line, and the countdown restarts");
    timed.consume_line("@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"waiting\",\"prompt\":\"Turn\"}",
                       30.0);
    check(timed.retry_reason.empty(), "the retry reason belongs to its step only");
    gt::CalRunState keyed;
    keyed.begin(gt::CalFlow::Orientation);
    keyed.consume_line("@@progress {\"flow\":\"orientation\",\"step\":1,\"steps\":4,\"state\":\"waiting\"}", 1.0);
    check(keyed.awaiting_user() && keyed.countdown_s(2.0) < 0.0, "keyboard mode shows no countdown");

    gt::CalRunState no_device;
    no_device.begin(gt::CalFlow::Mag, true, 5.0);
    no_device.consume_line("@@progress {\"flow\":\"mag\",\"step\":1,\"steps\":2,\"state\":\"failed\",\"prompt\":\"x\","
                           "\"detail\":\"NO_DEVICE: no RayNeo GT HID device found\",\"quality\":-1}");
    no_device.process_exited(1);
    check(no_device.phase == gt::CalRunState::Phase::Failed &&
              no_device.failure.find("glasses were not found") != std::string::npos,
          "a failure before step 1 is reported in plain words");
    gt::CalRunState tool_cancel;
    tool_cancel.begin(gt::CalFlow::Orientation, true, 5.0);
    tool_cancel.consume_line("@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"failed\","
                             "\"prompt\":\"x\",\"detail\":\"cancelled\"}");
    tool_cancel.process_exited(1);
    check(tool_cancel.phase == gt::CalRunState::Phase::Cancelled, "the tool's own cancelled line reads as cancelled");
    gt::CalRunState old_tool;
    old_tool.begin(gt::CalFlow::Orientation, true, 5.0);
    old_tool.consume_line("unknown or incomplete option: --auto");
    old_tool.process_exited(2);
    check(old_tool.phase == gt::CalRunState::Phase::Failed &&
              old_tool.failure.find("does not understand") != std::string::npos,
          "exit code 2 (bad arguments) names an outdated tool");
    {
        // "Run setup guide" while a calibration runs must not orphan it: the
        // tool keeps the glasses, and only the running state offers Cancel,
        // drives the glasses guide and receives the result (R6-B).
        gt::CalRunState live_run;
        live_run.begin(gt::CalFlow::Orientation, true, 5.0);
        live_run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"recording\"}");
        live_run.clear_unless_running();
        live_run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":4,\"steps\":4,\"state\":\"done\","
                              "\"quality\":0.9}");
        check(live_run.phase == gt::CalRunState::Phase::Succeeded && live_run.quality > 0.8f,
              "clearing the panel keeps a running calibration and its result");
        live_run.clear_unless_running();
        gt::CalRunState failed_run;
        failed_run.start_failed("not found");
        failed_run.clear_unless_running();
        check(live_run.phase == gt::CalRunState::Phase::Idle && !live_run.has_event &&
                  failed_run.phase == gt::CalRunState::Phase::Idle && failed_run.failure.empty(),
              "clearing the panel resets a finished run");
    }
    {
        // R6-B: the tool now replaces ill-formed UTF-8 with U+FFFD (pinned in
        // orientation_calibration_selftest). The parser is strict, so that
        // is what keeps a localized SAVE_FAILED line from being dropped.
        gt::CalProgressEvent final_event;
        std::string final_error;
        check(gt::parse_cal_progress_line("@@progress {\"flow\":\"orientation\",\"step\":4,\"steps\":4,"
                                          "\"state\":\"failed\",\"prompt\":\"Calibration was not saved.\","
                                          "\"detail\":\"SAVE_FAILED: Acc\xEF\xBF\xBDs refus\xEF\xBF\xBD.\","
                                          "\"quality\":-1}",
                                          final_event, final_error) == gt::CalLineKind::Progress &&
                  final_event.state == gt::CalStepState::Failed &&
                  final_event.detail.rfind("SAVE_FAILED: ", 0) == 0,
              "a final failed line with U+FFFD in its detail parses");
        check(gt::parse_cal_progress_line("@@progress {\"flow\":\"orientation\",\"step\":4,\"steps\":4,"
                                          "\"state\":\"failed\",\"detail\":\"SAVE_FAILED: Acc\xE8s\"}",
                                          final_event, final_error) == gt::CalLineKind::Malformed,
              "raw ANSI bytes would make that line malformed (why the tool sanitises)");
    }
    check(gt::cal_reason_text("SMALL_EXCURSION: turn further") == "Turn further" &&
              gt::cal_reason_text("plain words") == "plain words" &&
              gt::cal_reason_text("STREAM_FAILED: timeout").find("motion data") != std::string::npos,
          "reason codes become plain sentences");
    check(gt::cal_user_units("head movement 0.80 deg/s (limit 1.5)") == "head movement 0.80°/s (limit 1.5)" &&
              gt::cal_user_units("3 of 3 gestures used, spread 1.2 deg, excursion 30.0 deg") ==
                  "3 of 3 gestures used, spread 1.2°, excursion 30.0°" &&
              gt::cal_user_units("turn at least 90 degrees") == "turn at least 90 degrees" &&
              gt::cal_user_units("the deg word") == "the deg word",
          "tool text shown in the app uses the degree sign, words are left alone");
    check(gt::cal_reason_text("AXES_DISAGREE: the steps disagree (up_vs_yaw 6.10 deg, nod_level 5.40 deg, "
                              "nod_tilt 12.80 deg, handedness 9.95 deg)") == "The steps disagree" &&
              gt::cal_reason_text("SMALL_EXCURSION: excursion 8.0 deg") == "Excursion 8.0°",
          "failure text drops the raw axis metrics and uses degree signs");

    // The calibration-saved line: plain words by quality band, raw metrics never shown.
    const std::string metrics = "up_vs_yaw 6.10 deg, nod_level 5.40 deg, nod_tilt 12.80 deg, handedness 9.95 deg";
    const std::string good = gt::calibration_saved_text(gt::CalFlow::Orientation, 0.85f, metrics);
    const std::string fair = gt::calibration_saved_text(gt::CalFlow::Orientation, 0.5f, metrics);
    const std::string retry = gt::calibration_saved_text(gt::CalFlow::Orientation, 0.1f, metrics);
    const std::string unknown = gt::calibration_saved_text(gt::CalFlow::Orientation, -1.0f, {});
    check(good.find("agreed closely") != std::string::npos && fair.find("reasonably well") != std::string::npos &&
              retry.find("disagreed") != std::string::npos &&
              unknown == "The new calibration is in use from the next start.",
          "the saved line follows the Good / Fair / Retry bands");
    check(good.find("up_vs_yaw") == std::string::npos && fair.find("deg") == std::string::npos &&
              retry.find("handedness") == std::string::npos,
          "the saved line never shows the raw metric names");
    check(gt::calibration_saved_text(gt::CalFlow::Orientation, 0.85f,
                                     metrics + "; next: calibrate the magnetometer (--mag) for drift-free heading")
                  .find("Next, calibrate the compass") != std::string::npos &&
              good.find("compass") == std::string::npos,
          "the compass follow-up appears only when the tool asks for it");
    const std::string mag_ok = gt::calibration_saved_text(
        gt::CalFlow::Mag, 0.9f, "field 51.5 uT, residual 0.80 uT, turn 360 deg, tilt 40 deg, observability 0.120");
    const std::string mag_thin = gt::calibration_saved_text(
        gt::CalFlow::Mag, 0.5f,
        "field 51.5 uT, residual 0.80 uT, turn 200 deg, tilt 10 deg, observability 0.020; for a better result "
        "repeat and nod up and down and tilt ear to shoulder more");
    check(mag_ok.find("heading lock is on") != std::string::npos && mag_ok.find("uT") == std::string::npos &&
              mag_thin.find("run it again and nod up and down and tilt ear to shoulder more.") != std::string::npos &&
              mag_thin.find("observability") == std::string::npos,
          "the compass saved line is plain words and keeps the tool's movement advice");
}

void test_onboarding(const std::filesystem::path& directory) {
    std::printf("app_selftest: first-run setup guide\n");
    gt::AppConfig config;
    check(gt::onboarding_should_show(config), "a fresh install shows the setup guide");
    gt::OnboardingFlow flow;
    flow.start();
    check(flow.active && flow.step == gt::OnboardingStep::Welcome && !flow.can_go_back() &&
              !flow.step_skippable(),
          "the guide opens on Welcome with no Back and no Skip");
    flow.back();
    check(flow.step == gt::OnboardingStep::Welcome, "Back on the first step stays put");
    flow.next();
    check(flow.step == gt::OnboardingStep::GlassesCheck && flow.can_go_back(),
          "Next moves to the glasses check");
    flow.next();
    check(flow.step == gt::OnboardingStep::HeadCalibration && flow.step_skippable(),
          "the head calibration step is skippable");
    flow.mark_head_calibrated();
    flow.next();
    check(flow.head == gt::StepOutcome::Completed && flow.step == gt::OnboardingStep::MagCalibration,
          "a completed calibration stays completed when moving on");
    flow.skip_step();
    check(flow.mag == gt::StepOutcome::Skipped && flow.step == gt::OnboardingStep::LayoutPreset,
          "Skip marks the compass step skipped and advances");
    flow.back();
    flow.back();
    check(flow.step == gt::OnboardingStep::HeadCalibration && flow.head == gt::StepOutcome::Completed,
          "Back keeps earlier outcomes");
    flow.next();
    flow.next();
    flow.preset_choice = static_cast<int>(gt::LayoutPreset::QuadArc);
    flow.next();
    check(flow.on_last_step() && flow.preset_choice == static_cast<int>(gt::LayoutPreset::QuadArc),
          "the layout choice survives to the final step");
    flow.next();
    check(flow.step == gt::OnboardingStep::Done, "Next on Done does not run past the end");

    gt::OnboardingFlow passed;
    passed.start();
    for (int i = 0; i < 4; ++i) {
        passed.next();
    }
    check(passed.head == gt::StepOutcome::Skipped && passed.mag == gt::StepOutcome::Skipped,
          "pressing Next through pending calibrations records them as skipped");
    bool labelled = true;
    for (int i = 0; i < gt::kOnboardingStepCount; ++i) {
        const auto step = static_cast<gt::OnboardingStep>(i);
        labelled = labelled && std::wcslen(gt::onboarding_step_label(step)) > 0 &&
                   std::wcslen(gt::onboarding_step_title(step)) > 5;
    }
    check(labelled, "every step has a stepper label and a title");

    {
        // An app.json that failed to load: the controller runs on defaults
        // (onboarding_complete false, so the guide opens) while the user's
        // file stays on disk for them to fix. Finishing or skipping the guide
        // must not write the defaults over it.
        gt::OnboardingFlow guide;
        guide.start();
        gt::AppConfig defaults_in_use;
        check(!gt::onboarding_close(guide, defaults_in_use, false) && !guide.active &&
                  defaults_in_use.onboarding_complete,
              "closing the guide over an app.json that failed to load does not write the file");
    }
    check(gt::onboarding_close(flow, config, true) && !flow.active && config.onboarding_complete &&
              !gt::onboarding_should_show(config),
          "finishing (or skipping) closes the guide, marks it complete and saves a valid app.json");
    std::string error;
    gt::AppConfig reloaded;
    check(gt::save_app_config(directory / "onboarding.json", config, error) &&
              gt::load_app_config(directory / "onboarding.json", reloaded, error) &&
              !gt::onboarding_should_show(reloaded),
          "completion persists, so the guide does not reappear on the next launch");
    reloaded.onboarding_complete = false;
    check(gt::onboarding_should_show(reloaded),
          "clearing the flag (Run setup guide again) shows it again");
}

void test_animation() {
    std::printf("app_selftest: animation maths\n");
    check(gt::anim::ease_out_cubic(0.0f) == 0.0f && gt::anim::ease_out_cubic(1.0f) == 1.0f &&
              gt::anim::ease_out_cubic(0.5f) > 0.5f && gt::anim::ease_out_cubic(-3.0f) == 0.0f &&
              gt::anim::ease_out_cubic(9.0f) == 1.0f,
          "ease-out starts at 0, ends at 1, front-loads motion and clamps");
    check(std::fabs(gt::anim::ease_in_out_cubic(0.5f) - 0.5f) < 1e-6f &&
              gt::anim::ease_in_out_cubic(0.25f) < 0.25f &&
              gt::anim::ease_in_out_cubic(0.75f) > 0.75f,
          "ease-in-out is symmetric about the midpoint");
    bool monotonic = true;
    float previous = 0.0f;
    for (int i = 1; i <= 100; ++i) {
        const float value = gt::anim::ease_in_out_cubic(static_cast<float>(i) / 100.0f);
        monotonic = monotonic && value >= previous;
        previous = value;
    }
    check(monotonic, "ease-in-out never moves backwards");
    check(gt::anim::ease_out_back(1.0f) > 0.999f && gt::anim::ease_out_back(0.7f) > 1.0f,
          "ease-out-back overshoots slightly then lands on 1");

    // Exponential approach: frame-rate independent and exact at the end.
    float coarse = 0.0f;
    float fine = 0.0f;
    coarse = gt::anim::approach(coarse, 1.0f, 0.1f, 0.1f, 0.0f);
    for (int i = 0; i < 10; ++i) {
        fine = gt::anim::approach(fine, 1.0f, 0.01f, 0.1f, 0.0f);
    }
    check(std::fabs(coarse - fine) < 1e-4f && std::fabs(coarse - (1.0f - std::exp(-1.0f))) < 1e-4f,
          "approach is frame-rate independent (one 100 ms step == ten 10 ms steps)");
    check(gt::anim::approach(0.999f, 1.0f, 0.016f, 0.1f) == 1.0f,
          "approach snaps onto the target inside epsilon");

    gt::anim::Transition transition;
    check(transition.progress(5.0) == 1.0f && !transition.active(5.0),
          "an unstarted transition reads as finished");
    transition.begin(10.0, 0.2);
    check(transition.active(10.1) && std::fabs(transition.progress(10.1) - 0.5f) < 1e-5f &&
              transition.progress(11.0) == 1.0f && !transition.active(11.0),
          "a transition runs linearly over its duration");

    gt::anim::Animator animator;
    const uint64_t hover = gt::anim::key(gt::anim::kHover, 42);
    animator.begin_frame();
    check(animator.track(hover, 0.0f) == 0.0f, "a new value starts at its target (no pop-in)");
    animator.end_frame();
    check(!animator.animating(), "a settled animator wants no timer");
    animator.begin_frame();
    animator.track(hover, 1.0f);
    animator.end_frame();
    check(animator.animating(), "a new target starts an animation");
    int frames = 0;
    while (animator.step(0.016) && frames < 200) {
        ++frames;
    }
    animator.begin_frame();
    const float settled = animator.track(hover, 1.0f);
    animator.end_frame();
    check(settled == 1.0f && frames > 3 && frames < 60,
          "a hover fade settles exactly within a second, then the timer can stop");
    animator.begin_frame();
    animator.end_frame();
    check(animator.size() == 0, "values not painted this frame are dropped");

    animator.set_reduce_motion(true);
    animator.begin_frame();
    animator.track(hover, 0.0f);
    animator.track(hover, 1.0f);
    const float reduced = animator.track(hover, 1.0f);
    animator.end_frame();
    check(reduced == 1.0f && !animator.animating(),
          "with Windows animations off every value snaps");
    animator.begin_frame();
    animator.request_frames();
    check(!animator.wants_frames(), "pulses request no frames when animations are off");
    animator.set_reduce_motion(false);
    animator.request_frames();
    check(animator.wants_frames(), "a pulse requests frames while animations are on");
    animator.begin_frame();
    check(!animator.wants_frames(), "the frame request lasts one paint");
    animator.snap(hover, 0.25f);
    check(animator.track(hover, 0.25f) == 0.25f, "snap jumps a value (slider drag)");

    // The animation timer. A frame request is cleared only by the next paint,
    // and a hidden (tray) or minimized window gets no WM_PAINT: closing the
    // window to the tray while the status card pulsed (engine starting) used
    // to leave the 15 ms timer ticking until the window was shown again.
    gt::anim::AnimationDemand demand;
    check(!gt::anim::animation_timer_wanted(demand), "an idle window runs no animation timer");
    demand.wants_frames = true;
    check(gt::anim::animation_timer_wanted(demand), "a visible window with a frame request animates");
    demand.window_paints = false;
    check(!gt::anim::animation_timer_wanted(demand),
          "a hidden or minimized window runs no animation timer, even with a stale frame request");
    demand.wants_frames = false;
    demand.starting = true;
    demand.toast = true;
    demand.animating = true;
    demand.transition = true;
    demand.fade_pending = true;
    check(!gt::anim::animation_timer_wanted(demand), "nothing keeps the timer alive while the window cannot paint");
    demand.window_paints = true;
    demand.toast = false;
    demand.animating = false;
    demand.transition = false;
    demand.fade_pending = false;
    check(gt::anim::animation_timer_wanted(demand), "an engine start still animates while the window is shown");
}

// Stand-in for the AppState members the pointer helpers touch.
struct PointerFixture {
    int hover_id = 0;
    int press_id = 0;
    bool drag_active = false;
    int drag_comfort = -1;
    int drag_field = -1;
    int drag_screen = -1;
    bool orbiting = false;
    int drag_scroll_panel = -1;
    int drag_splitter = -1;
};

// Stand-in for CalibrationRunner in stop_calibration_gracefully().
struct FakeRunner {
    bool active_ = false;
    unsigned long exits_within_ms = 0;
    unsigned long waited_ms = 0;
    int cancels = 0;
    int terminates = 0;
    bool active() const { return active_; }
    void cancel() { ++cancels; }
    bool wait_for_exit(unsigned long ms) {
        waited_ms = ms;
        if (cancels > 0 && exits_within_ms <= ms) {
            active_ = false;
            return true;
        }
        return false;
    }
    void terminate() { ++terminates; }
};

// Stand-in for the AppState members open_setup_guide() touches.
struct GuideFixture {
    gt::OnboardingFlow onboarding;
    gt::CalRunState cal_run;
};

// Input and engine-start decisions AppWindow delegates to ui_input.h.
void test_ui_input() {
    std::printf("app_selftest: engine-start gate and pointer input\n");
    // An in-app calibration (including its post-cancel shutdown wait) owns
    // the glasses' sensor: an engine start must be refused, not launched.
    const std::wstring blocked = gt::engine_start_calibration_block(true);
    check(!blocked.empty(), "an engine start is refused while a calibration is running");
    check(blocked.find(L"calibration") != std::wstring::npos,
          "the refusal tells the user the calibration is the reason");
    check(gt::engine_start_calibration_block(false).empty(),
          "an engine start is allowed when no calibration is running");
    // "Open in console" needs the sensor too: refused beside a running
    // engine and beside an in-app run (also one still shutting down).
    check(!gt::console_calibration_block(true, false).empty(),
          "a console calibration is refused while the engine runs");
    check(!gt::console_calibration_block(false, true).empty(),
          "a console calibration is refused while an in-app calibration is running");
    check(gt::console_calibration_block(false, false).empty(),
          "a console calibration may start when the sensor is free");
    // The tray can still reach execute() while the modal setup guide is up.
    check(!gt::engine_start_guide_block(true).empty(),
          "an engine start (tray) is refused while the setup guide is open");
    check(gt::engine_start_guide_block(false).empty(), "an engine start is allowed with the guide closed");
    // Start/Retry during a cancelled run's shutdown grace used to be ignored
    // silently; it now explains itself.
    const std::wstring closing = gt::calibration_start_block(true, false);
    check(closing.find(L"still closing") != std::wstring::npos,
          "Start/Retry during a cancelled run's shutdown grace says the old run is still closing");
    check(!gt::calibration_start_block(true, true).empty(), "a second start during a running calibration is refused");
    check(gt::calibration_start_block(false, false).empty(), "a calibration may start when none is active");

    // Logoff/shutdown (and window destroy) stop an in-app calibration through
    // its graceful cancel, bounded, terminating only if it does not exit.
    FakeRunner graceful;
    graceful.active_ = true;
    graceful.exits_within_ms = 300;
    check(gt::stop_calibration_gracefully(graceful, gt::kEndSessionCalibrationWaitMs) && graceful.cancels == 1 &&
              graceful.terminates == 0 && graceful.waited_ms == gt::kEndSessionCalibrationWaitMs,
          "end session cancels the calibration gracefully and does not kill a tool that exits");
    FakeRunner stuck;
    stuck.active_ = true;
    stuck.exits_within_ms = 100000;
    check(!gt::stop_calibration_gracefully(stuck, gt::kEndSessionCalibrationWaitMs) && stuck.cancels == 1 &&
              stuck.terminates == 1,
          "a tool that ignores the cancel is terminated after the bounded wait");
    FakeRunner none;
    check(gt::stop_calibration_gracefully(none, gt::kEndSessionCalibrationWaitMs) && none.cancels == 0 &&
              none.terminates == 0,
          "with no calibration running the end-session stop does nothing");
    check(gt::kEndSessionCalibrationWaitMs > 0 && gt::kEndSessionCalibrationWaitMs <= 2500,
          "the end-session wait stays well under the ~5 s Windows allows");

    // Losing mouse capture mid-drag (Alt+Tab) must end the drag exactly as a
    // button-up would, or the next plain mouse move keeps dragging.
    PointerFixture slider;
    slider.press_id = 42;
    slider.drag_active = true;
    slider.drag_comfort = 42;
    int pending = 7;
    gt::PointerDragEnd lost = gt::pointer_on_capture_lost(slider, pending);
    check(lost.ended && !slider.drag_active, "capture lost mid-drag ends the drag");
    check(slider.drag_comfort == -1 && slider.press_id == 0 && pending == 0,
          "capture lost clears the dragged slider, the pressed look and the pending click");
    check(lost.save_comfort && !lost.save_splitter,
          "capture lost still saves a moved comfort slider, like a button-up");

    PointerFixture splitter;
    splitter.drag_active = true;
    splitter.drag_splitter = 2;
    splitter.drag_scroll_panel = 1;
    splitter.drag_field = 3;
    splitter.drag_screen = 1;
    splitter.orbiting = true;
    pending = 0;
    lost = gt::pointer_on_capture_lost(splitter, pending);
    check(lost.ended && lost.save_splitter && !lost.save_comfort,
          "capture lost on a splitter drag persists the splitter");
    check(splitter.drag_splitter == -1 && splitter.drag_scroll_panel == -1 &&
              splitter.drag_field == -1 && splitter.drag_screen == -1 && !splitter.orbiting,
          "capture lost closes every drag mode");

    // Our own ReleaseCapture on button-up also sends WM_CAPTURECHANGED,
    // after the drag ended: it must not save twice or touch a pending click.
    PointerFixture idle;
    pending = 9;
    lost = gt::pointer_on_capture_lost(idle, pending);
    check(!lost.ended && !lost.save_splitter && !lost.save_comfort && pending == 9,
          "capture changed with no drag is a no-op");
    PointerFixture button_up;
    button_up.drag_active = true;
    button_up.drag_comfort = 5;
    pending = 0;
    const gt::PointerDragEnd up = gt::pointer_end_drag(button_up, pending);
    lost = gt::pointer_on_capture_lost(button_up, pending);
    check(up.ended && up.save_comfort && !lost.ended && !lost.save_comfort,
          "the button-up's own ReleaseCapture does not end (or save) the drag twice");

    // Press a button, drag out and release outside: the button-up never
    // arrives, so leaving the window must drop the pressed look.
    PointerFixture pressed;
    pressed.press_id = 42;
    pressed.hover_id = 42;
    check(gt::pointer_on_mouse_leave(pressed) && pressed.press_id == 0 && pressed.hover_id == 0,
          "leaving the window clears the pressed look and the hover");
    PointerFixture press_only;
    press_only.press_id = 42;
    check(gt::pointer_on_mouse_leave(press_only) && press_only.press_id == 0,
          "leaving the window clears a press even with no hover (repaint requested)");
    check(!gt::pointer_on_mouse_leave(press_only), "a second leave changes nothing");
    PointerFixture dragging;
    dragging.press_id = 42;
    dragging.hover_id = 42;
    dragging.drag_active = true;
    check(!gt::pointer_on_mouse_leave(dragging) && dragging.press_id == 42 &&
              dragging.hover_id == 42,
          "a captured drag keeps its pressed look and hover when the pointer leaves");

    // Ctrl+1..6 with the button held on a splitter: the page changes under
    // the drag, so it ends there with the normal save and no click.
    PointerFixture page_drag;
    page_drag.press_id = 1203;
    page_drag.drag_active = true;
    page_drag.drag_splitter = 3;
    pending = 1203;
    const gt::PointerDragEnd paged = gt::pointer_on_page_change(page_drag, pending);
    check(paged.ended && paged.save_splitter && !page_drag.drag_active && page_drag.drag_splitter == -1 &&
              page_drag.press_id == 0 && pending == 0,
          "a page switch mid-drag ends the drag with the normal splitter save and no click");
    lost = gt::pointer_on_capture_lost(page_drag, pending);
    check(!lost.ended && !lost.save_splitter,
          "the ReleaseCapture after a page switch does not end (or save) the drag twice");
    PointerFixture page_idle;
    pending = 5;
    check(!gt::pointer_on_page_change(page_idle, pending).ended && pending == 5,
          "a page switch with no drag ends nothing");

    // "Run setup guide" under a live in-app calibration (started on the
    // Tracking page, then Settings): opening the guide used to clear the run
    // state, orphaning the tool - its progress was then ignored, Cancel and
    // the glasses guide vanished, and engine starts stayed refused for a
    // calibration nobody could see. The guide must not open until it ends.
    GuideFixture live;
    live.cal_run.begin(gt::CalFlow::Orientation, true, 5.0);
    check(!gt::open_setup_guide(live, true), "the setup guide does not open during a calibration");
    check(live.cal_run.running() && live.cal_run.flow == gt::CalFlow::Orientation && !live.onboarding.active,
          "a refused guide leaves the live calibration run (and its Cancel) intact");
    live.cal_run.process_exited(1);
    check(live.cal_run.phase == gt::CalRunState::Phase::Failed,
          "the run still sees its own exit after the refused guide");
    GuideFixture cancelling;
    cancelling.cal_run.begin(gt::CalFlow::Mag, true, 5.0);
    cancelling.cal_run.cancel();
    check(!gt::open_setup_guide(cancelling, true),
          "the guide also waits out a cancelled tool that has not exited yet");
    GuideFixture marked;
    marked.cal_run.begin(gt::CalFlow::Orientation, true, 5.0);
    check(!gt::open_setup_guide(marked, false), "a run still marked running blocks the guide");
    GuideFixture idle_guide;
    idle_guide.cal_run.start_failed("old failure");
    idle_guide.onboarding.step = gt::OnboardingStep::Done;
    check(gt::open_setup_guide(idle_guide, false) && idle_guide.onboarding.active &&
              idle_guide.onboarding.step == gt::OnboardingStep::Welcome &&
              idle_guide.cal_run.phase == gt::CalRunState::Phase::Idle,
          "with no calibration the guide opens at Welcome and clears a finished run's panel");
}

// Startup phase keys (status file "phase" / "phase_progress") and the Home
// steps they map to. Older engines without the keys, and unknown values from
// newer engines, must keep the generic view.
void test_startup_phase() {
    std::printf("app_selftest: startup phase status keys and Home steps\n");
    std::string error;
    gt::EngineStatusFile status;
    check(gt::parse_engine_status("state=ready\nimu=1\nphase=hold_still\nphase_progress=0.42\n", status, error) &&
              status.phase == "hold_still" && std::fabs(status.phase_progress - 0.42) < 1e-9,
          "phase and phase_progress parse");
    check(std::string(gt::kStatusKeyPhase) == "phase" && std::string(gt::kStatusKeyPhaseProgress) == "phase_progress",
          "the phase keys keep their names");
    check(gt::parse_engine_status("state=ready\nimu=1\nfps=60.00\n", status, error) && status.phase.empty() &&
              status.phase_progress < 0.0,
          "an older engine without the keys reports no phase");
    const gt::StartupView old_engine =
        gt::startup_view(gt::parse_engine_phase(status.phase), status.phase_progress, status.imu);
    check(!old_engine.visible, "no phase key keeps the generic Starting/Running view");
    check(gt::parse_engine_status("state=ready\nphase=calibrating_v2\nphase_progress=nan\n", status, error) &&
              gt::parse_engine_phase(status.phase) == gt::EnginePhase::Unknown && status.phase_progress < 0.0,
          "an unknown phase value and a non-numeric progress are ignored");
    check(!gt::startup_view(gt::EnginePhase::Unknown, 0.5, true).visible, "an unknown phase shows no steps");
    check(gt::parse_engine_status("state=ready\nphase=imu_warmup\nphase_progress=1.7\n", status, error) &&
              status.phase_progress == 1.0,
          "a progress above 1 is clamped");
    check(gt::parse_engine_status("state=ready\nphase=imu_warmup\nphase_progress=-0.2\n", status, error) &&
              status.phase_progress < 0.0,
          "a negative progress counts as not reported");

    // Every engine phase value round-trips to its own enum.
    const struct {
        const char* name;
        gt::EnginePhase phase;
    } names[] = {{gt::kPhasePreparing, gt::EnginePhase::Preparing},
                 {gt::kPhaseTakeover, gt::EnginePhase::Takeover},
                 {gt::kPhaseWaitingDisplay, gt::EnginePhase::WaitingDisplay},
                 {gt::kPhaseConnecting, gt::EnginePhase::Connecting},
                 {gt::kPhaseImuWarmup, gt::EnginePhase::ImuWarmup},
                 {gt::kPhaseHoldStill, gt::EnginePhase::HoldStill},
                 {gt::kPhaseTracking, gt::EnginePhase::Tracking},
                 {gt::kPhaseRunning, gt::EnginePhase::Running},
                 {gt::kPhaseRestoring, gt::EnginePhase::Restoring}};
    bool all_named = true;
    for (const auto& entry : names) {
        all_named = all_named && gt::parse_engine_phase(entry.name) == entry.phase;
    }
    check(all_named, "every engine phase value maps to its own phase");

    // Steps with head tracking: screens, glasses sensor, warm-up, hold still.
    const auto step_of = [](gt::EnginePhase phase) { return gt::startup_view(phase, -1.0, true).active_step; };
    check(step_of(gt::EnginePhase::Preparing) == 0 && step_of(gt::EnginePhase::Takeover) == 0 &&
              step_of(gt::EnginePhase::WaitingDisplay) == 0 && step_of(gt::EnginePhase::Connecting) == 1 &&
              step_of(gt::EnginePhase::ImuWarmup) == 2 && step_of(gt::EnginePhase::HoldStill) == 3,
          "each startup phase activates its step");
    check(!gt::startup_view(gt::EnginePhase::Tracking, 1.0, true).visible &&
              !gt::startup_view(gt::EnginePhase::Running, 1.0, false).visible &&
              !gt::startup_view(gt::EnginePhase::Restoring, -1.0, true).visible,
          "tracking, running and restoring hide the steps");
    const gt::StartupView takeover = gt::startup_view(gt::EnginePhase::Takeover, -1.0, true);
    check(takeover.visible && takeover.step_count == gt::kStartupStepCount && takeover.progress < 0.0 &&
              !takeover.hold_still,
          "the display takeover shows indeterminate steps");
    const gt::StartupView early = gt::startup_view(gt::EnginePhase::ImuWarmup, 0.3, true);
    const gt::StartupView late = gt::startup_view(gt::EnginePhase::ImuWarmup, 0.75, true);
    check(early.visible && !early.get_ready && std::fabs(early.progress - 0.3) < 1e-9 && late.get_ready &&
              !late.hold_still && std::wstring(late.headline).find(L"hold still") != std::wstring::npos,
          "the end of the warm-up asks the wearer to get ready to hold still");
    const gt::StartupView waiting = gt::startup_view(gt::EnginePhase::HoldStill, 0.0, true);
    const gt::StartupView measuring = gt::startup_view(gt::EnginePhase::HoldStill, 0.5, true);
    const gt::StartupView unreported = gt::startup_view(gt::EnginePhase::HoldStill, -1.0, true);
    check(waiting.hold_still && measuring.hold_still && waiting.progress == 0.0 && measuring.progress == 0.5 &&
              unreported.progress == 0.0 && std::wstring(waiting.detail) != measuring.detail &&
              std::wstring(measuring.headline) == L"Hold your head still",
          "hold still shows the prompt with a filling bar");
    const gt::StartupView no_imu_takeover = gt::startup_view(gt::EnginePhase::Takeover, -1.0, false);
    check(no_imu_takeover.visible && no_imu_takeover.step_count == 0 &&
              !gt::startup_view(gt::EnginePhase::HoldStill, 0.5, false).visible,
          "without head tracking only the screen setup is shown, with no sensor steps");
    check(std::wstring(gt::kStartStillHint).find(L"still") != std::wstring::npos,
          "the pre-start hint asks the wearer to keep still");
}

std::string live_line(const char* kind, const char* state, float yaw, float pitch, float roll, float along, float off,
                      int repeats, const char* hint, float remaining = 4.0f) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "@@live {\"step\":2,\"kind\":\"%s\",\"state\":\"%s\",\"elapsed\":1.00,\"remaining\":%.2f,"
                  "\"frame\":\"file\",\"yaw\":%.2f,\"pitch\":%.2f,\"roll\":%.2f,\"along\":%.2f,\"off\":%.2f,"
                  "\"speed\":20.0,\"still_rms\":0.50,\"still_gate\":1.5,\"gate\":12,\"target\":25,\"peak\":%.2f,"
                  "\"repeats\":%d,\"repeats_required\":3,\"hint\":\"%s\"}",
                  kind, state, static_cast<double>(remaining), static_cast<double>(yaw), static_cast<double>(pitch),
                  static_cast<double>(roll), static_cast<double>(along), static_cast<double>(off),
                  static_cast<double>(std::max(0.0f, along)), repeats, hint);
    return buffer;
}

bool guide_near(float a, float b, float tolerance = 0.01f) {
    return std::fabs(a - b) <= tolerance;
}

void test_calibration_guide() {
    std::printf("app_selftest: live calibration guide (@@live)\n");
    gt::CalLiveSample sample;
    std::string error;
    // The tool's own pinned line (orientation_calibration_selftest).
    check(gt::parse_cal_live_line(
              "@@live {\"step\":2,\"kind\":\"turn\",\"state\":\"recording\",\"elapsed\":1.25,\"remaining\":11.75,"
              "\"frame\":\"file\",\"yaw\":12.50,\"pitch\":0.00,\"roll\":0.25,\"along\":12.50,\"off\":0.25,"
              "\"speed\":30.0,\"still_rms\":0.50,\"still_gate\":1.5,\"gate\":12,\"target\":25,\"peak\":12.50,"
              "\"repeats\":1,\"repeats_required\":3,\"hint\":\"FURTHER\"}\r\n",
              sample, error) == gt::CalLiveLineKind::Live &&
              sample.step == 2 && sample.kind == gt::CalGuideKind::Turn && !sample.waiting &&
              guide_near(sample.remaining_s, 11.75f) && sample.frame == "file" && guide_near(sample.yaw_deg, 12.5f) &&
              guide_near(sample.roll_deg, 0.25f) && guide_near(sample.off_deg, 0.25f) && guide_near(sample.speed_degs, 30.0f) &&
              guide_near(sample.gate_deg, 12.0f) && guide_near(sample.target_deg, 25.0f) && sample.repeats == 1 &&
              sample.repeats_required == 3 && sample.hint == "FURTHER",
          "a tool @@live line parses field for field");
    check(gt::parse_cal_live_line("captured 2856 samples (476.0 Hz)", sample, error) == gt::CalLiveLineKind::NotLive &&
              gt::parse_cal_live_line("@@lively chatter", sample, error) == gt::CalLiveLineKind::NotLive &&
              gt::parse_cal_live_line("@@progress {\"flow\":\"orientation\",\"state\":\"ok\"}", sample, error) ==
                  gt::CalLiveLineKind::NotLive,
          "console text and progress lines are not live lines");
    check(gt::parse_cal_live_line("@@live {not json", sample, error) == gt::CalLiveLineKind::Malformed &&
              gt::parse_cal_live_line("@@live {\"kind\":\"spin\",\"state\":\"recording\"}", sample, error) ==
                  gt::CalLiveLineKind::Malformed &&
              gt::parse_cal_live_line("@@live {\"kind\":\"nod\",\"state\":\"done\"}", sample, error) ==
                  gt::CalLiveLineKind::Malformed,
          "bad JSON, kind or state is malformed");
    check(gt::parse_cal_live_line("@@live {\"kind\":\"tilt\",\"state\":\"waiting\",\"yaw\":1e9,\"gate\":-3,"
                                  "\"target\":2,\"hint\":\"bad<hint>\",\"repeats\":-4}",
                                  sample, error) == gt::CalLiveLineKind::Live &&
              sample.waiting && guide_near(sample.yaw_deg, 360.0f) && guide_near(sample.gate_deg, 12.0f) &&
              guide_near(sample.target_deg, 12.0f) && sample.hint.empty() && sample.repeats == 0 &&
              guide_near(sample.remaining_s, -1.0f),
          "out-of-range values are clamped, a bad hint is dropped, missing fields default");

    // Run model: live lines update the guide, never the failure report.
    gt::CalRunState run;
    run.begin(gt::CalFlow::Orientation, true, 5.0);
    run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"waiting\","
                     "\"prompt\":\"Turn left\",\"detail\":\"\",\"quality\":-1}",
                     10.0);
    check(!gt::cal_guide_view(run, 10.0).visible, "an older tool (no @@live) keeps the text-only view");
    run.consume_line(live_line("turn", "waiting", 0.2f, 0.1f, 0.0f, 0.2f, 0.1f, 0, "GET_READY", 3.2f), 10.5);
    gt::CalGuideView view = gt::cal_guide_view(run, 10.5);
    check(run.tool_has_live && run.has_live && run.recent_output.empty() && run.malformed_lines == 0,
          "live lines feed the guide and stay out of the console tail");
    check(view.visible && view.has_live && view.countdown == 4 && view.headline == "Turn left and back" &&
              view.subline.find("3 times") != std::string::npos && view.tone == gt::CalGuideTone::Neutral,
          "waiting: step name, how-to with the repeat count, countdown from the tool");
    check(!gt::cal_guide_view(run, 12.0).has_live && run.live_fresh(11.4) && !run.live_fresh(11.6),
          "a live sample older than a second is not drawn");
    run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"recording\","
                     "\"prompt\":\"Turn left\",\"detail\":\"\",\"quality\":-1}",
                     11.0);
    run.consume_line(live_line("turn", "recording", 26.0f, 0.5f, 0.3f, 26.0f, 0.6f, 1, "BACK_TO_CENTRE"), 11.1);
    view = gt::cal_guide_view(run, 11.1);
    check(view.headline == "Back to centre" && view.subline == "1 of 3" && view.tone == gt::CalGuideTone::Good &&
              view.countdown == -1,
          "recording: the live hint is the headline, repeats counted");
    for (int i = 0; i < 12; ++i) {
        run.consume_line(live_line("turn", "recording", 20.0f, 6.0f, 1.0f, 20.0f, 6.1f, 1, "OFF_AXIS"), 11.2);
    }
    view = gt::cal_guide_view(run, 11.2);
    check(view.headline == "Keep your head level" && view.tone == gt::CalGuideTone::Warn,
          "off-axis turn: 'Keep your head level' in the warning colour");
    run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"retry\","
                     "\"prompt\":\"Turn left\",\"detail\":\"LOW_DOMINANCE: the turn mixed axes\",\"quality\":0}",
                     12.0);
    view = gt::cal_guide_view(run, 12.0);
    check(!run.has_live && view.headline == "Let's try that again" &&
              run.retry_tip.find("chin level") != std::string::npos && view.tip == run.retry_tip,
          "retry after an off-axis attempt: the guide names the correction");
    run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"waiting\","
                     "\"prompt\":\"Turn left\",\"detail\":\"\",\"quality\":-1}",
                     12.1);
    check(!run.retry_tip.empty(), "the correction stays for the retried attempt");
    run.consume_line("@@progress {\"flow\":\"orientation\",\"step\":2,\"steps\":4,\"state\":\"ok\","
                     "\"prompt\":\"Turn left\",\"detail\":\"\",\"quality\":0.9}",
                     20.0);
    check(run.retry_tip.empty() && !run.has_live && gt::cal_guide_view(run, 20.0).headline == "Step done",
          "a passed step clears the guide and the tip");

    gt::CalLiveSample last;
    last.kind = gt::CalGuideKind::Nod;
    last.repeats = 1;
    last.repeats_required = 3;
    check(gt::cal_retry_tip(last, 0, 0, 0).find("further") != std::string::npos &&
              gt::cal_retry_tip(last, 0, 0, 12).find("DOWN") != std::string::npos &&
              gt::cal_retry_tip(last, 3, 0, 0).find("further") != std::string::npos,
          "retry tips: too few repeats -> further; wrong way names the direction; brief hints ignored");
    last.repeats = 3;
    check(gt::cal_retry_tip(last, 0, 12, 0) == "Move more slowly and smoothly." && gt::cal_retry_tip(last, 0, 0, 0).empty(),
          "retry tips: too fast -> slower; nothing seen -> no tip");

    // Geometry: degrees -> pixels on a 1920x1080 glasses display, 46 deg FOV.
    const gt::CalGuideScale turn = gt::cal_guide_scale(gt::CalGuideKind::Turn, 1920.0f, 1080.0f, 46.0f, 25.0f);
    check(guide_near(turn.px_per_deg, 0.9f * 960.0f / 30.0f, 0.01f),
          "turn scale fits target + 5 deg in 90% of the half width (28.8 px/deg)");
    const gt::CalGuideScale wide = gt::cal_guide_scale(gt::CalGuideKind::Turn, 1920.0f, 1080.0f, 46.0f, 5.0f);
    check(guide_near(wide.px_per_deg, 1920.0f / 46.0f, 0.01f), "a small target keeps the true FOV scale (41.7 px/deg)");
    const gt::CalGuideScale nod = gt::cal_guide_scale(gt::CalGuideKind::Nod, 1920.0f, 1080.0f, 46.0f, 20.0f);
    check(guide_near(nod.px_per_deg, 0.9f * 540.0f / 25.0f, 0.01f), "nod scale uses the half height (19.4 px/deg)");
    check(guide_near(gt::cal_guide_scale(gt::CalGuideKind::Turn, 1920.0f, 1080.0f, 0.0f, 5.0f).px_per_deg, 1920.0f / 46.0f),
          "an invalid FOV falls back to 46 deg");

    gt::CalLiveSample head;
    head.kind = gt::CalGuideKind::Turn;
    head.yaw_deg = 10.0f;
    head.pitch_deg = -2.0f;
    gt::CalGuidePoint ball = gt::cal_guide_ball(head, turn, 960.0f, 540.0f, 1920.0f, 1080.0f);
    check(guide_near(ball.x, 960.0f - 288.0f, 0.1f) && guide_near(ball.y, 540.0f + 57.6f, 0.1f),
          "turn left moves the ball left; nodding down moves it down (off-axis to scale)");
    head.yaw_deg = 90.0f;
    ball = gt::cal_guide_ball(head, turn, 960.0f, 540.0f, 1920.0f, 1080.0f);
    check(ball.x >= 0.0f && ball.x < 60.0f, "the ball is clamped inside the display");
    gt::CalLiveSample tilt;
    tilt.kind = gt::CalGuideKind::Tilt;
    tilt.roll_deg = 15.0f;
    gt::CalGuideScale arc = gt::cal_guide_scale(gt::CalGuideKind::Tilt, 1920.0f, 1080.0f, 46.0f, 20.0f);
    arc.arc_radius = 400.0f;
    ball = gt::cal_guide_ball(tilt, arc, 960.0f, 700.0f, 1920.0f, 1080.0f);
    check(guide_near(ball.x, 960.0f + 200.0f, 0.1f) && guide_near(ball.y, 700.0f - 346.41f, 0.1f),
          "tilt right moves the ball clockwise along the arc (15 deg -> 30 deg drawn)");
    const gt::CalGuidePoint target = gt::cal_guide_axis_point(gt::CalGuideKind::Nod, 20.0f, nod, 960.0f, 200.0f);
    check(guide_near(target.x, 960.0f) && guide_near(target.y, 200.0f + 20.0f * nod.px_per_deg, 0.1f),
          "nod target mark sits below the centre");
    check(guide_near(gt::cal_guide_ring_radius(0.0f, 1.5f, 10.0f, 100.0f), 10.0f) &&
              guide_near(gt::cal_guide_ring_radius(0.75f, 1.5f, 10.0f, 100.0f), 55.0f) &&
              guide_near(gt::cal_guide_ring_radius(3.0f, 1.5f, 10.0f, 100.0f), 100.0f) &&
              guide_near(gt::cal_guide_ring_radius(std::nanf(""), 1.5f, 10.0f, 100.0f), 100.0f),
          "steadiness ring tightens with stillness, saturates at the 1.5 deg/s gate");

    gt::CalLiveSample tone;
    tone.kind = gt::CalGuideKind::Turn;
    tone.hint = "FURTHER";
    const bool good = gt::cal_guide_tone(tone) == gt::CalGuideTone::Good;
    tone.hint = "OFF_AXIS";
    const bool off = gt::cal_guide_tone(tone) == gt::CalGuideTone::Warn;
    tone.waiting = true;
    const bool neutral = gt::cal_guide_tone(tone) == gt::CalGuideTone::Neutral;
    tone.waiting = false;
    tone.kind = gt::CalGuideKind::Still;
    tone.still_rms_degs = 1.2f;
    const bool shaky = gt::cal_guide_tone(tone) == gt::CalGuideTone::Warn;
    tone.still_rms_degs = 0.4f;
    const bool steady = gt::cal_guide_tone(tone) == gt::CalGuideTone::Good;
    check(good && off && neutral && shaky && steady, "colour state: on-axis/steady good, off-axis/moving warn");
    check(gt::cal_guide_hint_text(gt::CalGuideKind::Turn, "OFF_AXIS") == "Keep your head level" &&
              gt::cal_guide_hint_text(gt::CalGuideKind::Nod, "FURTHER") == "Nod further" &&
              gt::cal_guide_hint_text(gt::CalGuideKind::Tilt, "WRONG_WAY") == "Other way: tilt right" &&
              gt::cal_guide_hint_text(gt::CalGuideKind::Still, "HOLD") == "Hold still" &&
              gt::cal_guide_hint_text(gt::CalGuideKind::Turn, "BACK_TO_CENTRE") == "Back to centre" &&
              gt::cal_guide_hint_text(gt::CalGuideKind::Turn, "SOMETHING_NEW").empty(),
          "hint text per step");
}

int main() {
    std::printf("app_selftest: controller app model\n");
    std::filesystem::path directory;
    std::error_code directory_error;
    for (int attempt = 0; attempt < 100 && directory.empty(); ++attempt) {
        const std::filesystem::path candidate = std::filesystem::temp_directory_path() /
                                                ("rayneo-app-selftest-" +
                                                 std::to_string(GetCurrentProcessId()) + "-" +
                                                 std::to_string(attempt));
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
    test_startup_phase();
    test_app_config(directory);
    test_presets_and_screens();
    test_user_presets(directory);
    test_help_topics();
    test_orbit();
    test_field_normalisation();
    test_session_logs(directory);
    test_telemetry();
    test_view_comfort(directory);
    test_ui_model(directory);
    test_calibration_progress();
    test_calibration_guide();
    test_onboarding(directory);
    test_animation();
    test_ui_input();

    std::filesystem::remove_all(directory, directory_error);
    std::printf("app_selftest: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
