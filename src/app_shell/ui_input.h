#pragma once

// Window-free decisions behind the controller's input and engine-start paths,
// kept here (header-only, no Win32) so app_selftest can cover them. AppWindow
// calls these with its own AppState; the tests call them with a stand-in that
// has the same members.

#include <string>

namespace gt {

// An in-app calibration owns the glasses' sensor: the tool streams the IMU
// and turns the stream off when it exits, which would kill a running
// engine's tracking. `calibration_active` is true from the tool's launch
// until its process is gone, so it also covers the post-cancel shutdown wait.
// Returns the refusal shown to the user, or an empty string when the engine
// may start.
inline std::wstring engine_start_calibration_block(bool calibration_active) {
    if (calibration_active) {
        return L"Finish or cancel the calibration first: it is using the glasses' sensor.";
    }
    return std::wstring();
}

// "Open in console" (head or compass calibration in its own console window).
// The console tool needs the sensor exactly like the in-app run: it must not
// start beside a running engine (its exit turns the IMU stream off under the
// engine) nor beside an in-app run, including one cancelled but still
// shutting down (two tools streaming and both writing the calibration file).
// The paint pass enables the button whenever no in-app run is shown, which
// covers both cases, so the click itself is gated. Returns the refusal, or an
// empty string when the console tool may start.
inline std::wstring console_calibration_block(bool engine_busy, bool calibration_active) {
    if (calibration_active) {
        return L"Finish or cancel the calibration first: it is using the glasses' sensor.";
    }
    if (engine_busy) {
        return L"Stop the engine before calibrating: the calibration needs the glasses' sensor to itself.";
    }
    return std::wstring();
}

// The setup guide never starts the engine, and its calibration steps need
// the sensor free. Its own pages offer no Start button, but the tray menu
// (and any other path into execute()) still can: refused while the guide is
// open. Returns the refusal, or an empty string when the engine may start.
inline std::wstring engine_start_guide_block(bool setup_guide_active) {
    if (setup_guide_active) {
        return L"Finish or skip the setup guide first, then start the workspace.";
    }
    return std::wstring();
}

// Start or Retry on an in-app calibration. `calibration_active` is true from
// the tool's launch until its process is gone; `run_running` is the panel's
// running phase. A cancelled run keeps the process for its shutdown grace
// (the tool turns the IMU stream off), so a click then has to wait: say so
// instead of ignoring it. Returns the message, or empty when a run may start.
inline std::wstring calibration_start_block(bool calibration_active, bool run_running) {
    if (run_running) {
        return L"A calibration is already running.";
    }
    if (calibration_active) {
        return L"The previous calibration is still closing; try again in a moment.";
    }
    return std::wstring();
}

// Graceful stop of an in-app calibration at logoff/shutdown and on window
// destroy: cancel() sends "q" and closes stdin, which the tool's --auto
// stdin watcher maps to its Ctrl+C path (stream off, exit 1; bug 27); only
// if it has not exited within `wait_ms` is it terminated. `Runner` has
// active(), cancel(), wait_for_exit(ms) and terminate() (CalibrationRunner).
// Returns true when the tool exited by itself (or none was running).
template <typename Runner>
bool stop_calibration_gracefully(Runner& runner, unsigned long wait_ms) {
    if (!runner.active()) {
        return true;
    }
    runner.cancel();
    if (runner.wait_for_exit(wait_ms)) {
        return true;
    }
    runner.terminate();
    return false;
}

// WM_ENDSESSION budget for that graceful stop. During logoff/shutdown
// Windows gives an application only a few seconds (the hung-app /
// WaitToKillAppTimeout defaults, about 5 s on current builds) before it
// shows the "this app is preventing shutdown" screen or ends it; the cancel
// path is documented to take a few hundred ms (not measured on hardware), so
// 2 s stays well under that and leaves room for the engine request after it.
inline constexpr unsigned long kEndSessionCalibrationWaitMs = 2000;

// "Run setup guide" (Settings). Opening the guide starts it afresh and clears
// the calibration panel; doing that under a live in-app calibration would
// orphan the run: its progress lines are ignored by the cleared (Idle)
// state, the Cancel button and the glasses guide disappear, and the engine
// start stays refused for a calibration the user can no longer see or
// cancel. So the guide does not open while the tool process exists
// (`calibration_active`, which includes the post-cancel shutdown wait) or
// the run is still marked running. Returns whether the guide opened; the
// caller then resets the guide's scroll and re-runs the health checks.
template <typename State>
bool open_setup_guide(State& state, bool calibration_active) {
    if (calibration_active || state.cal_run.running()) {
        return false;
    }
    state.onboarding.start();
    state.cal_run = decltype(state.cal_run){};
    return true;
}

// What the drag-end half of a button-up left to do.
struct PointerDragEnd {
    bool ended = false;          // a drag was active and is now over
    bool save_splitter = false;  // a splitter moved: persist the preferences
    bool save_comfort = false;   // a comfort slider moved: persist it
};

// The drag-end half of a left-button-up: every drag mode is closed and the
// pending click is dropped (a drag never also clicks). The caller releases
// capture and performs the saves the result asks for. No-op when nothing is
// being dragged.
template <typename State>
PointerDragEnd pointer_end_drag(State& state, int& pending_click_id) {
    PointerDragEnd result;
    if (!state.drag_active) {
        return result;
    }
    result.ended = true;
    result.save_splitter = state.drag_splitter >= 0;
    result.save_comfort = state.drag_comfort >= 0;
    state.drag_active = false;
    state.drag_comfort = -1;
    state.drag_field = -1;
    state.drag_screen = -1;
    state.orbiting = false;
    state.drag_scroll_panel = -1;
    state.drag_splitter = -1;
    pending_click_id = 0;
    return result;
}

// WM_CAPTURECHANGED: the window lost the mouse mid-gesture (Alt+Tab, another
// window took capture). The button-up will never arrive here, so end the drag
// exactly as a button-up would, but never execute a click. Our own
// ReleaseCapture in the button-up also lands here, after the drag has
// already ended, and is then a no-op.
template <typename State>
PointerDragEnd pointer_on_capture_lost(State& state, int& pending_click_id) {
    state.press_id = 0;
    return pointer_end_drag(state, pending_click_id);
}

// Page switch (Ctrl+1..6, sidebar arrows) while a captured drag is in
// progress: the dragged control is no longer painted, so the drag ends here
// exactly as a button-up would, with the normal save of a moved splitter or
// comfort slider and no click. The caller releases capture (whose
// WM_CAPTURECHANGED is then a no-op) and performs the saves.
template <typename State>
PointerDragEnd pointer_on_page_change(State& state, int& pending_click_id) {
    state.press_id = 0;
    return pointer_end_drag(state, pending_click_id);
}

// WM_MOUSELEAVE: hover and the pressed look end when the pointer leaves the
// window (a button released outside never reaches us). Both are kept during
// a captured drag. The click itself is still decided by the pending click id
// on button-up. Returns true when anything changed (repaint).
template <typename State>
bool pointer_on_mouse_leave(State& state) {
    if (state.drag_active) {
        return false;
    }
    const bool changed = state.hover_id != 0 || state.press_id != 0;
    state.hover_id = 0;
    state.press_id = 0;
    return changed;
}

}  // namespace gt
