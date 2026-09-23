#include "app_shell/app_state.h"

#include "app_shell/clock.h"
#include "app_shell/diagnostics.h"
#include "layout/layout.h"
#include "vdd/display_config.h"

#include <shellapi.h>

#include <cstdio>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <utility>

namespace gt {
namespace {

constexpr size_t kMaxEvents = 60;
constexpr double kTaskbarRestoreWindowS = 10.0;

void apply_taskbar_state(int state) {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    data.lParam = state & (ABS_AUTOHIDE | ABS_ALWAYSONTOP);
    SHAppBarMessage(ABM_SETSTATE, &data);
}

int masked_taskbar_state() {
    return gt::query_taskbar_state() & (ABS_AUTOHIDE | ABS_ALWAYSONTOP);
}

std::wstring mode_label(LaunchMode mode) {
    switch (mode) {
        case LaunchMode::Workspace:
            return L"full workspace (Parsec virtual desktops)";
        case LaunchMode::Preview:
            return L"renderer-only preview (labelled test screens, no virtual desktops)";
        case LaunchMode::None:
        default:
            return L"no mode";
    }
}

}  // namespace

void AppState::add_event(const std::wstring& text) {
    wchar_t stamp[32];
    std::swprintf(stamp, std::size(stamp), L"%6.1fs  ", now_s);
    events.push_back(std::wstring(stamp) + text);
    if (events.size() > kMaxEvents) {
        events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(events.size() - kMaxEvents));
    }
}

void AppState::set_banner(const std::wstring& text, Severity severity) {
    banner = text;
    banner_severity = severity;
}

void AppState::show_toast(const std::wstring& text) {
    toast = text;
    toast_expiry_s = now_s + 3.0;
}

void AppState::clear_banner() {
    banner.clear();
    banner_severity = Severity::Neutral;
}

void ensure_log_directory(const AppState& state) {
    std::error_code error;
    std::filesystem::create_directories(state.paths.log_dir, error);
}

std::wstring start_block_reason(const AppState& state, LaunchMode mode) {
    if (!state.paths.engine_found) {
        return L"spatial_desk.exe was not found next to this app; build the engine or run "
               L"scripts\\stage-portable.ps1";
    }
    const bool head_tracking =
        mode == LaunchMode::Workspace ||
        (mode == LaunchMode::Preview && !state.config.preview_without_head_tracking);
    if (mode == LaunchMode::Workspace && !state.diagnostics.vdd_ready) {
        return L"Parsec VDD is " + state.diagnostics.vdd_status_text +
               L"; full workspace needs the signed driver, Start preview does not";
    }
    if (head_tracking && !state.diagnostics.calibration_valid) {
        return L"orientation calibration is not usable: " + state.diagnostics.calibration_detail;
    }
    if (!state.diagnostics.glasses_found && state.config.monitor_index < 0) {
        return L"no RayNeo display is present; set the glasses to Extend mode (Win+P) first, or "
               L"pick a display index in config/app.json";
    }
    return std::wstring();
}

bool start_engine(AppState& state, LaunchMode mode, std::wstring& error) {
    if (mode != LaunchMode::Workspace && mode != LaunchMode::Preview) {
        error = L"choose full workspace or renderer-only preview";
        return false;
    }
    if (state.engine.busy()) {
        error = L"the engine is already running";
        return false;
    }
    if (state.engine.adopt_if_running()) {
        state.starting = false;
        state.clear_banner();
        state.add_event(L"found a running engine (launched by hand); adopted its window");
        state.show_toast(L"Adopted the running engine");
        return true;
    }
    if (!state.diagnostics.glasses_found && state.config.monitor_index < 0) {
        // Self-heal a Settings-disconnected display instead of blocking:
        // GDI re-attach first, CCD reactivation whenever that did not
        // recover (same rule as the engine's own sites).
        std::wstring detached;
        std::string ignored;
        bool recovered = false;
        if (gt::find_detached_glasses_display(detached)) {
            recovered = gt::reattach_detached_glasses(ignored);
            if (recovered) {
                state.add_event(L"re-attached the disconnected glasses display");
            }
        }
        if (!recovered && gt::reactivate_glasses_path(ignored)) {
            state.add_event(L"re-activated the glasses display path");
        }
        collect_display_and_vdd(state.diagnostics);
    }
    const std::wstring reason = start_block_reason(state, mode);
    if (!reason.empty()) {
        error = reason;
        return false;
    }
    ensure_log_directory(state);

    // spatial_desk.exe refuses to start without a layout file; create it from
    // the in-memory layout (triple arc by default) instead of failing.
    std::error_code exists_error;
    if (!std::filesystem::exists(state.paths.layout_path, exists_error)) {
        std::string save_error;
        std::error_code directory_error;
        std::filesystem::create_directories(state.paths.layout_path.parent_path(), directory_error);
        if (!gt::save_layout(narrow_utf8(state.paths.layout_path), state.layout, save_error)) {
            error = L"could not create the layout file: " + wide_from_utf8(save_error);
            return false;
        }
        state.add_event(L"created " + state.paths.layout_path.wstring() + L" from the editor layout");
    }

    // Rotate before the engine opens the files: it appends, so a long-lived CSV
    // would otherwise grow without bound across sessions.
    std::string rotate_error;
    if (!rotate_log_file(state.paths.engine_log, state.config.engine_log_rotation, rotate_error)) {
        state.add_event(L"engine log rotation skipped: " + wide_from_utf8(rotate_error));
    }
    if (!rotate_log_file(state.paths.telemetry_log, state.config.telemetry_rotation, rotate_error)) {
        state.add_event(L"telemetry rotation skipped: " + wide_from_utf8(rotate_error));
    }

    const bool head_tracking =
        mode == LaunchMode::Workspace ||
        (mode == LaunchMode::Preview && !state.config.preview_without_head_tracking);
    EngineLaunchCommand command;
    std::string build_error;
    if (!build_engine_launch_command(
            narrow_utf8(state.paths.engine_executable), narrow_utf8(state.paths.executable_dir),
            narrow_utf8(state.paths.layout_path), narrow_utf8(state.paths.calibration_path),
            narrow_utf8(state.paths.engine_status), narrow_utf8(state.paths.engine_log),
            narrow_utf8(state.paths.telemetry_log), mode, head_tracking, state.config.monitor_index,
            0.0f, command, build_error)) {
        error = wide_from_utf8(build_error);
        return false;
    }
    // A stale status file from a previous run must never be presented as
    // live state; the engine rewrites it within a second of starting.
    std::error_code status_remove_error;
    std::filesystem::remove(state.paths.engine_status, status_remove_error);
    std::string launch_error;
    if (!state.engine.launch(command, launch_error)) {
        error = wide_from_utf8(launch_error);
        return false;
    }
    // The engine takes over (and churns) the topology from here: snapshot
    // the taskbar preference so every exit path can hand it back.
    state.taskbar_state_at_engine_start = gt::query_taskbar_state();
    state.taskbar_restore_until_s = 0.0;
    state.config.last_mode = mode;
    state.config.last_engine_error.clear();
    state.starting = true;
    state.set_banner(L"starting " + mode_label(mode) +
                         (head_tracking ? L"..."
                                          : L" (fixed camera: no head tracking, "
                                            L"side screens cannot be viewed by turning)..."),
                     Severity::Neutral);
    state.add_event(L"started engine: " + mode_label(mode));
    if (state.config_valid) {
        if (!save_config_to_disk(state, error)) {
            state.add_event(L"preferences were not saved: " + error);
        }
    }
    error.clear();
    return true;
}

void restore_taskbar_after_churn(AppState& state) {
    if (state.taskbar_state_at_engine_start < 0) {
        return;
    }
    // Immediate set plus a sticky poll window: Explorer recreates the tray
    // seconds after churn, so the poll below re-asserts until it sticks.
    // Masked both sides: only the auto-hide/always-on-top bits are ours.
    const int want = state.taskbar_state_at_engine_start & (ABS_AUTOHIDE | ABS_ALWAYSONTOP);
    if (masked_taskbar_state() != want) {
        apply_taskbar_state(want);
        state.add_event(L"taskbar auto-hide restored to the pre-engine setting");
    }
    state.taskbar_restore_until_s = state.now_s + kTaskbarRestoreWindowS;
}

bool recover_display_topology(AppState& state) {
    const LONG result =
        SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
    restore_taskbar_after_churn(state);
    if (result == ERROR_SUCCESS) {
        state.add_event(L"display recovery applied (Extend topology)");
        return true;
    }
    state.add_event(L"display recovery failed; use Win+P to restore the laptop display");
    return false;
}

std::string layout_presets_dir(const AppState& state) {
    return narrow_utf8(state.paths.layout_path.parent_path() / "presets");
}

void refresh_layout_presets(AppState& state) {
    state.preset_names = list_layout_presets(layout_presets_dir(state));
    if (!state.loaded_preset.empty()) {
        bool still_there = false;
        for (const std::string& name : state.preset_names) {
            if (name == state.loaded_preset) {
                still_there = true;
                break;
            }
        }
        if (!still_there) {
            state.loaded_preset.clear();
        }
    }
}

void stop_engine_for_quit(AppState& state) {
    if (!state.engine.busy()) {
        return;
    }
    state.quitting = true;
    if (state.hwnd != nullptr) {
        ShowWindow(state.hwnd, SW_HIDE);
    }
    state.engine.request_stop();
    // Bounded wait: quitting must not orphan the engine (and its Parsec
    // desktops). The client terminates an owned engine after its 45 s grace
    // (a full display restore takes 13-35 s); a foreign engine that ignores
    // the quit request is reported and left alone. 60 s covers the grace
    // plus reap slack. The wait pumps messages so session teardown stays
    // answerable; nested commands are ignored while quitting
    // (see on_close/execute).
    const double deadline = steady_now_s() + 60.0;
    for (;;) {
        state.now_s = steady_now_s();
        poll_engine(state);
        if (!state.engine.busy()) {
            return;
        }
        const EngineSnapshot& live = state.engine.snapshot();
        if (live.state == EngineProcessState::Failed && !live.process_alive) {
            return;
        }
        if (steady_now_s() >= deadline) {
            state.add_event(L"quit: the engine is still running; leaving it in place");
            return;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void stop_engine(AppState& state) {
    state.engine.request_stop();
    state.add_event(L"asked the engine to stop (graceful quit message)");
}

bool save_layout_to_disk(AppState& state, std::wstring& error) {
    std::string validation_error;
    if (!gt::validate_layout(state.layout, validation_error)) {
        error = L"the layout is not valid: " + wide_from_utf8(validation_error);
        return false;
    }
    std::error_code directory_error;
    std::filesystem::create_directories(state.paths.layout_path.parent_path(), directory_error);
    if (!gt::save_layout(narrow_utf8(state.paths.layout_path), state.layout, validation_error)) {
        error = L"could not save the layout: " + wide_from_utf8(validation_error);
        return false;
    }
    state.layout_dirty = false;
    state.add_event(L"saved " + state.paths.layout_path.wstring() + L" (" +
                    std::to_wstring(state.layout.screens.size()) + L" screens)");
    if (state.engine.busy()) {
        std::string command_error;
        if (state.engine.send_command(kEngineMessageReloadLayout, command_error)) {
            state.add_event(L"layout saved; engine reload requested");
        } else {
            state.add_event(L"engine reload failed: " + wide_from_utf8(command_error));
        }
    }
    return true;
}

bool revert_layout_from_disk(AppState& state, std::wstring& error) {
    Layout loaded;
    std::string load_error;
    if (!gt::load_layout(narrow_utf8(state.paths.layout_path), loaded, load_error)) {
        error = L"could not reload the layout: " + wide_from_utf8(load_error);
        return false;
    }
    state.layout = std::move(loaded);
    state.layout_dirty = false;
    if (state.selected_screen >= state.layout.screens.size()) {
        state.selected_screen = state.layout.screens.empty() ? 0 : state.layout.screens.size() - 1;
    }
    state.add_event(L"reloaded the layout from disk");
    return true;
}

bool save_config_to_disk(AppState& state, std::wstring& error) {
    std::string save_error;
    if (!save_app_config(state.paths.app_config_path, state.config, save_error)) {
        error = L"could not save preferences: " + wide_from_utf8(save_error);
        return false;
    }
    // The file now holds exactly the running state, so later automatic saves
    // are safe again.
    state.config_valid = true;
    return true;
}

bool reload_app_config(AppState& state, std::wstring& error) {
    AppConfig reloaded;
    std::string load_error;
    if (!load_app_config(state.paths.app_config_path, reloaded, load_error)) {
        error = L"config/app.json is still invalid (" + wide_from_utf8(load_error) + L")";
        return false;
    }
    state.config = reloaded;
    state.config_valid = true;
    if (state.engine.busy()) {
        // The running engine was launched with the old file set: keep
        // watching its status/log/layout files and apply the re-pointed
        // paths on the next start instead of going blank mid-run.
        const std::filesystem::path keep_layout = state.paths.layout_path;
        const std::filesystem::path keep_log_dir = state.paths.log_dir;
        apply_config_paths(state.paths, state.config);
        state.paths.layout_path = keep_layout;
        state.paths.log_dir = keep_log_dir;
        state.paths.engine_log = keep_log_dir / "engine.log";
        state.paths.telemetry_log = keep_log_dir / "telemetry.csv";
        state.paths.engine_status = keep_log_dir / "engine-status.txt";
        state.add_event(L"reloaded config/app.json; layout/log paths apply on the next start");
    } else {
        apply_config_paths(state.paths, state.config);
        state.add_event(L"reloaded config/app.json");
    }
    return true;
}

void poll_engine(AppState& state) {
    const EngineProcessState previous = state.engine.snapshot().state;
    state.engine.poll(state.now_s);

    // While the engine runs steady, track the live taskbar state so a
    // mid-session user change becomes the value handed back (never during
    // Starting/Stopping churn, and never inside a sticky window, where the
    // re-assert below wins).
    if (state.engine.snapshot().state == EngineProcessState::Running &&
        state.taskbar_state_at_engine_start >= 0 &&
        state.now_s >= state.taskbar_restore_until_s) {
        state.taskbar_state_at_engine_start = gt::query_taskbar_state();
    }
    // Sticky taskbar window armed by restore_taskbar_after_churn: re-assert
    // while Explorer settles (its tray recreation lags churn by seconds).
    if (state.taskbar_state_at_engine_start >= 0 &&
        state.now_s < state.taskbar_restore_until_s &&
        masked_taskbar_state() !=
            (state.taskbar_state_at_engine_start & (ABS_AUTOHIDE | ABS_ALWAYSONTOP))) {
        apply_taskbar_state(state.taskbar_state_at_engine_start);
    }
    // Deferred display recovery: the engine failed while still alive
    // (foreign timeout); recover once the process is actually gone.
    if (state.display_recovery_pending && !state.engine.snapshot().process_alive) {
        state.display_recovery_pending = false;
        recover_display_topology(state);
    }

    // The status file is advisory and may be stale (previous run) or absent
    // (hand-launched engine without --status): only fresh content is live.
    if (!state.engine.busy()) {
        state.engine_status = EngineStatusFile{};
    } else {
        EngineStatusFile fresh;
        std::string tail;
        std::string read_error;
        std::string parse_error;
        const auto now_unix = static_cast<uint64_t>(std::time(nullptr));
        if (read_text_tail(state.paths.engine_status, 8u << 10, tail, read_error) &&
            parse_engine_status(tail, fresh, parse_error) &&
            engine_status_is_fresh(fresh, now_unix, kEngineStatusMaxAgeS)) {
            state.engine_status = fresh;
        } else {
            state.engine_status = EngineStatusFile{};
        }
    }

    if (state.engine.snapshot().state == EngineProcessState::Exited && state.engine_status.parsed &&
        state.engine_status.state == kStatusFailed) {
        // A clean exit code with a failed status marker is a fatality the
        // exit code missed: correct the snapshot so the banner, the tooltip
        // and the state label all report the failure.
        state.engine.note_status_failure(state.engine_status.error);
    }

    const EngineSnapshot& snapshot = state.engine.snapshot();
    if (snapshot.state != previous) {
        switch (snapshot.state) {
            case EngineProcessState::Running:
                state.starting = false;
                state.clear_banner();
                state.add_event(snapshot.query_answered
                                    ? L"engine running (" +
                                          std::to_wstring(snapshot.query_screens) + L" screens)"
                                    : L"engine running");
                break;
            case EngineProcessState::Failed: {
                state.starting = false;
                std::wstring reason = wide_from_utf8(snapshot.last_error);
                if (reason.empty() && state.engine_status.parsed &&
                    !state.engine_status.error.empty()) {
                    reason = wide_from_utf8(state.engine_status.error);
                }
                if (reason.empty()) {
                    reason = L"the engine stopped unexpectedly";
                }
                state.set_banner(L"engine failed: " + reason +
                                     L"  (recovery: open the engine log, fix the cause, then "
                                     L"Start workspace or Start preview again)",
                                 Severity::Error);
                state.add_event(L"engine failed: " + reason);
                // Safety net: an engine that died mid-session may have left the
                // laptop panel detached. Re-extend to bring every connected
                // display back; harmless when the topology is already healthy.
                // Only when the process is actually gone: a foreign engine
                // that timed out is still alive and churning, so re-extending
                // under it would fight it (defer until it exits instead).
                if (!snapshot.process_alive) {
                    recover_display_topology(state);
                } else {
                    state.display_recovery_pending = true;
                    state.add_event(L"display recovery deferred until the engine exits");
                }
                if (!state.quit_requested) {
                    state.config.last_engine_error = snapshot.last_error.empty()
                                                         ? state.engine_status.error
                                                         : snapshot.last_error;
                    if (state.config_valid) {
                        std::wstring ignored;
                        save_config_to_disk(state, ignored);
                    }
                }
                break;
            }
            case EngineProcessState::Stopped:
                state.starting = false;
                if (previous != EngineProcessState::Stopped) {
                    state.add_event(L"engine stopped");
                    state.clear_banner();
                    // The engine restores the taskbar itself on a clean
                    // exit, but a forced kill after the stop grace can
                    // still cut a slow restore short: the controller owns
                    // the last word.
                    restore_taskbar_after_churn(state);
                }
                break;
            case EngineProcessState::Starting:
                state.starting = true;
                break;
            case EngineProcessState::Stopping:
                state.starting = false;
                break;
            case EngineProcessState::Exited:
                state.starting = false;
                state.add_event(L"engine exited normally");
                state.clear_banner();
                break;
        }
    }
    if (state.engine_status.parsed && state.engine_status.state == kStatusFailed &&
        !state.engine_status.error.empty() && snapshot.process_alive) {
        state.set_banner(L"engine reported a problem: " + wide_from_utf8(state.engine_status.error),
                         Severity::Error);
    }
}

void run_expensive_diagnostics(AppState& state) {
    collect_display_and_vdd(state.diagnostics);
    collect_calibration(state.paths, state.diagnostics);
    collect_hid(state.diagnostics);
    collect_layout_file(state.paths, state.diagnostics);
    collect_telemetry(state.paths, state.diagnostics);
    collect_engine_log(state.paths, state.diagnostics);
    state.next_expensive_check_s = state.now_s + 2.0;
}

}  // namespace gt
