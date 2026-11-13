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
