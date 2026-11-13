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

}  // namespace gt
