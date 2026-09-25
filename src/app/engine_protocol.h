#pragma once

// Private contract between the controller ("RayNeo Spatial.exe", src/app_shell)
// and the renderer engine (spatial_desk.exe, src/app/spatial_desk.cpp).
//
// The controller owns the engine as a child process but never shares a heap or
// a handle table with it. Everything therefore crosses one of two boundaries:
//
//   * window messages  - the controller finds the engine window by class name
//                        (kEngineWindowClass) and sends the kEngineMessage*
//                        values below. Only integer payloads cross, so the
//                        messages are safe to send across processes.
//   * a status file    - the engine atomically rewrites a small key=value file
//                        once per second (--status); the controller reads it at
//                        no more than 2 Hz for engine-state diagnostics and for
//                        the case where the engine was launched by hand.
//
// The values are load-bearing: the two executables are shipped together and may
// be rebuilt independently, so never renumber an existing message or status key.

#include <cstdint>
#include <string>

namespace gt {

// Windows defines WM_APP as 0x8000; repeat it here so this header stays free of
// windows.h and can be unit tested by the pure app-model selftest.
inline constexpr unsigned kEngineMessageBase = 0x8000u;

inline constexpr unsigned kEngineMessageQuery = kEngineMessageBase + 0x101u;
inline constexpr unsigned kEngineMessageQuit = kEngineMessageBase + 0x102u;
inline constexpr unsigned kEngineMessageRecenter = kEngineMessageBase + 0x103u;
inline constexpr unsigned kEngineMessageToggleYaw = kEngineMessageBase + 0x104u;
inline constexpr unsigned kEngineMessageTogglePitch = kEngineMessageBase + 0x105u;
inline constexpr unsigned kEngineMessageReloadLayout = kEngineMessageBase + 0x106u;
// View comfort (app/view_comfort.h), applied live. Integer payloads only.
//   SetStabilise        wParam = level 0..4
//   SetDimMode          wParam = DimMode, lParam = focus-dim brightness %
//   SetScreenBrightness wParam = screen index, lParam = brightness %
//   SetNightTint        wParam = strength %, 0 = off
//   CursorToCenter      no payload: cursor to the middle of the centre screen
inline constexpr unsigned kEngineMessageSetStabilise = kEngineMessageBase + 0x107u;
inline constexpr unsigned kEngineMessageSetDimMode = kEngineMessageBase + 0x108u;
inline constexpr unsigned kEngineMessageSetScreenBrightness = kEngineMessageBase + 0x109u;
inline constexpr unsigned kEngineMessageSetNightTint = kEngineMessageBase + 0x10Au;
inline constexpr unsigned kEngineMessageCursorToCenter = kEngineMessageBase + 0x10Bu;

// Reply bits returned by kEngineMessageQuery. The low bits are engine state
// flags, the next byte is the layout screen count.
inline constexpr unsigned kEngineFlagYawTracking = 1u << 0;
inline constexpr unsigned kEngineFlagPitchTracking = 1u << 1;
inline constexpr unsigned kEngineFlagVirtualDisplays = 1u << 2;
inline constexpr unsigned kEngineFlagHeadTracking = 1u << 3;
inline constexpr unsigned kEngineFlagTopologyTakeover = 1u << 4;
inline constexpr unsigned kEngineScreenCountShift = 8;
inline constexpr unsigned kEngineScreenCountMask = 0xFFu << kEngineScreenCountShift;
// Live reading-stabilisation level (so a Ctrl+Alt+S press in the engine is
// reflected, and remembered, by the controller). Stored as level + 1 so an
// older engine that leaves these bits at zero reads as "unknown".
inline constexpr unsigned kEngineStabiliseShift = 16;
inline constexpr unsigned kEngineStabiliseMask = 0xFu << kEngineStabiliseShift;

inline constexpr wchar_t kEngineWindowClass[] = L"RayNeoSpatialDesk";
inline constexpr wchar_t kEngineWindowTitle[] = L"RayNeo Spatial Desk";
inline constexpr wchar_t kAppWindowClass[] = L"RayNeoSpatialController";
inline constexpr wchar_t kEngineExecutableName[] = L"spatial_desk.exe";
inline constexpr wchar_t kAppExecutableName[] = L"RayNeo Spatial.exe";
inline constexpr wchar_t kCalibrationExecutableName[] = L"orientation_calibrate.exe";

// Stable identity used for the taskbar button and for Start Menu pinning. Set
// through SetCurrentProcessExplicitAppUserModelID before any UI exists; the
// staging scripts copy the same string into the shortcut helper.
inline constexpr wchar_t kAppUserModelId[] = L"RayNeo.Spatial.Desktop";

// Engine command line switches understood by spatial_desk.exe.
inline constexpr const char* kEngineArgLayout = "--layout";
inline constexpr const char* kEngineArgCalibration = "--calibration";
inline constexpr const char* kEngineArgLog = "--log";
inline constexpr const char* kEngineArgStatus = "--status";
inline constexpr const char* kEngineArgMonitor = "--monitor";
inline constexpr const char* kEngineArgFov = "--fov";
inline constexpr const char* kEngineArgNoVirtualDisplays = "--no-virtual-displays";
inline constexpr const char* kEngineArgNoImu = "--no-imu";

// Status-file keys and values (one "key=value" per line, LF terminated).
inline constexpr const char* kStatusKeyState = "state";
inline constexpr const char* kStatusKeyMode = "mode";
inline constexpr const char* kStatusKeyScreens = "screens";
inline constexpr const char* kStatusKeyFps = "fps";
inline constexpr const char* kStatusKeyMonitor = "monitor";
inline constexpr const char* kStatusKeyMonitorWidth = "monitor_width";
inline constexpr const char* kStatusKeyMonitorHeight = "monitor_height";
inline constexpr const char* kStatusKeyVdd = "vdd";
inline constexpr const char* kStatusKeyImu = "imu";
inline constexpr const char* kStatusKeyDetached = "detached";
inline constexpr const char* kStatusKeyElapsed = "elapsed_s";
inline constexpr const char* kStatusKeyUpdated = "updated_unix";
inline constexpr const char* kStatusKeyError = "error";

inline constexpr const char* kStatusStarting = "starting";
inline constexpr const char* kStatusReady = "ready";
inline constexpr const char* kStatusFailed = "failed";
inline constexpr const char* kStatusStopped = "stopped";

inline constexpr const char* kModeWorkspace = "workspace";
inline constexpr const char* kModePreview = "preview";

// Human-readable name for a kEngineMessage* value; "unknown" for anything else.
const char* engine_message_name(unsigned message);

// Stable AppUserModelID string (same as kAppUserModelId, exposed without the
// wide-character literal for tests and logging).
const wchar_t* app_user_model_id();

// Comma-free, newline-free single-line rendering of a value for the status file.
std::string engine_status_sanitize(const std::string& value);

}  // namespace gt
