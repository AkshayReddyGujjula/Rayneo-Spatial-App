#pragma once

// Hover help for the controller dashboard: one topic per setting, each naming
// what it does plus the effect of raising/lowering it where that applies.
// Header-only so the app and the selftest share the table with no build change.

#include "app_shell/app_layout.h"

namespace gt {

enum class HelpTopic {
    StatusEngine = 0,
    StatusDisplay,
    StatusCalibration,
    StatusHid,
    StatusVdd,
    StatusLayoutFile,
    StatusTelemetry,
    StatusEngineLog,
    EngineStartWorkspace,
    EngineStartPreview,
    EngineStop,
    EngineRecenter,
    EngineReloadLayout,
    EngineYawToggle,
    EnginePitchToggle,
    EngineNoImu,
    DiagCalibration,
    DiagEngineLog,
    DiagLogsFolder,
    DiagCloseToTray,
    DiagRecoverDisplays,
    LayoutFactory,
    LayoutUserPresets,
    LayoutPresetSave,
    LayoutPresetDelete,
    LayoutAddScreen,
    LayoutRemoveScreen,
    LayoutSave,
    LayoutRevert,
    EditorCanvas,
    FieldYaw,
    FieldPitch,
    FieldRoll,
    FieldDistance,
    FieldWidth,
    FieldHeight,
    FieldFov,
    FieldActiveFps,
    FieldMidFps,
    FieldIdleFps,
    FieldEnterDeg,
    FieldLeaveDeg,
    HeaderRefresh,
    Count,
};

inline const wchar_t* help_text(HelpTopic topic) {
    switch (topic) {
        case HelpTopic::StatusEngine:
            return L"Whether the render engine process is running and healthy. Start "
                   L"workspace (or preview) if it is stopped; open the engine log below "
                   L"when it reports an error.";
        case HelpTopic::StatusDisplay:
            return L"Whether the RayNeo glasses display was found as a Windows monitor. "
                   L"It must read 'present' in Extend mode before Start workspace; use "
                   L"Recover displays if the glasses vanish after sleep.";
        case HelpTopic::StatusCalibration:
            return L"Whether config/orientation.json holds a usable sensor-to-head "
                   L"calibration. Without it the app refuses head tracking; run "
                   L"calibration from Diagnostics and recovery.";
        case HelpTopic::StatusHid:
            return L"Whether the glasses IMU answered over USB HID. 'Not found' means the "
                   L"cable, the HID driver or the glasses' USB mode needs attention "
                   L"before tracking can start.";
        case HelpTopic::StatusVdd:
            return L"Whether the Parsec virtual-display driver is installed and ready. "
                   L"Workspace mode needs it to create the real Windows monitors; "
                   L"without it use Start preview.";
        case HelpTopic::StatusLayoutFile:
            return L"Whether the active layout file parsed and validated. A problem here "
                   L"blocks Start; Revert in the layout panel restores the last saved file.";
        case HelpTopic::StatusTelemetry:
            return L"Whether the engine is recording its per-frame diagnostics CSV. It "
                   L"lands in the logs folder and is the first thing to attach when "
                   L"reporting a tracking bug.";
        case HelpTopic::StatusEngineLog:
            return L"Size and path of the engine's own log file. Open it from Diagnostics "
                   L"and recovery when the engine misbehaves or exits unexpectedly.";
        case HelpTopic::EngineStartWorkspace:
            return L"Creates real Windows virtual monitors and renders them as "
                   L"world-locked screens on the glasses. Switches the laptop panel off "
                   L"until exit; quitting restores everything.";
        case HelpTopic::EngineStartPreview:
            return L"Renders labelled test screens without touching your displays or "
                   L"needing the Parsec driver. Use it to arrange screens and check "
                   L"tracking safely.";
        case HelpTopic::EngineStop:
            return L"Asks the engine to quit gracefully: virtual desktops are removed and "
                   L"your display layout is restored. The controller waits up to a "
                   L"minute for the restore.";
        case HelpTopic::EngineRecenter:
            return L"Re-zeros head tracking where you are looking now (same as R / "
                   L"Ctrl+Shift+R). Press it whenever the centre drifts or after putting "
                   L"the glasses on.";
        case HelpTopic::EngineReloadLayout:
            return L"Tells the running engine to re-read the layout file right now. The "
                   L"Save button in the layout panel already does this; use it after "
                   L"hand-editing JSON.";
        case HelpTopic::EngineYawToggle:
            return L"Enables panning: looking left/right moves the view. Turn it off to "
                   L"freeze yaw while keeping pitch; Ctrl+Alt+Y does the same from anywhere.";
        case HelpTopic::EnginePitchToggle:
            return L"Enables nodding: looking up/down moves the view. Turn it off to freeze "
                   L"pitch while keeping yaw; Ctrl+Alt+P does the same from anywhere.";
        case HelpTopic::EngineNoImu:
            return L"Preview ignores the glasses IMU and pins the camera still. Only for "
                   L"arranging screens at your desk without wearing the glasses.";
        case HelpTopic::DiagCalibration:
            return L"Launches the guided sensor-to-head calibration (nod, tilt and yaw on "
                   L"request). Redo it when nodding visibly tilts the screens: that means "
                   L"a stale file.";
        case HelpTopic::DiagEngineLog:
            return L"Opens the engine log in your text editor. It records per-second pose, "
                   L"display topology changes and shutdown restore steps.";
        case HelpTopic::DiagLogsFolder:
            return L"Opens the folder with engine logs, telemetry CSVs and status files. "
                   L"Zip it when asking for help with tracking or display bugs.";
        case HelpTopic::DiagCloseToTray:
            return L"ON: closing the window hides it in the notification area and the "
                   L"engine keeps running. OFF: closing quits everything.";
        case HelpTopic::DiagRecoverDisplays:
            return L"Re-runs display recovery: reattaches a detached glasses display and "
                   L"restores the taskbar. Try it before rebooting when the glasses vanish.";
        case HelpTopic::LayoutFactory:
            return L"One-click starter arcs: Single, Triple (default), Quad, Five and Wide. "
                   L"Clicking loads one into the editor unsaved; Save and reload writes it.";
        case HelpTopic::LayoutUserPresets:
            return L"Your saved presets from config/layouts/presets; a star marks the "
                   L"loaded one. Click a preset to load it, then Save and reload to push "
                   L"it live.";
        case HelpTopic::LayoutPresetSave:
            return L"Saves the current editor arrangement under a name you choose. Triple "
                   L"and ultrawide ship by default; add as many as you like.";
        case HelpTopic::LayoutPresetDelete:
            return L"Deletes the loaded (starred) preset file. The live layout is "
                   L"untouched; this cannot be undone, but the file is tiny JSON.";
        case HelpTopic::LayoutAddScreen:
            return L"Adds a screen (lowest free id and virtual-display slot) at the "
                   L"centre. Move it with the arc editor or the sliders below.";
        case HelpTopic::LayoutRemoveScreen:
            return L"Removes the selected screen from the editor, unsaved. Its "
                   L"virtual-display slot becomes free for the next added screen.";
        case HelpTopic::LayoutSave:
            return L"Validates, writes the layout file and tells the engine to reload it "
                   L"live. The glasses view updates within a second.";
        case HelpTopic::LayoutRevert:
            return L"Throws away unsaved editor changes and reloads the last saved file. "
                   L"Use it when an experiment goes sideways.";
        case HelpTopic::EditorCanvas:
            return L"Top-down view: you are the dot, screens are the coloured bars. Drag "
                   L"a bar left/right for yaw, up/down for pitch; Tab here for arrow-key "
                   L"control.";
        case HelpTopic::FieldYaw:
            return L"Sideways angle of the selected screen in degrees. Negative moves it "
                   L"left, positive right; -45/0/+45 is the classic triple arc.";
        case HelpTopic::FieldPitch:
            return L"Vertical angle of the screen. Positive floats it above eye level, "
                   L"negative sinks it below; keep 0 for a level arc.";
        case HelpTopic::FieldRoll:
            return L"In-plane tilt of the screen. Almost always stays 0; small values "
                   L"straighten a screen that looks rotated.";
        case HelpTopic::FieldDistance:
            return L"How far away the screen floats, in metres. Larger pushes it away "
                   L"(smaller on the glasses, more head-turn to see); smaller pulls it close.";
        case HelpTopic::FieldWidth:
            return L"Screen width in metres. Wider fills more of your view; shrink it if "
                   L"the edges feel out of sight when looking straight.";
        case HelpTopic::FieldHeight:
            return L"Screen height in metres. Taller shows more vertical content; keep the "
                   L"16:9 ratio with width unless you want a custom shape.";
        case HelpTopic::FieldFov:
            return L"Horizontal field of view of the glasses render in degrees. Match your "
                   L"glasses (46); larger shows more but shrinks everything.";
        case HelpTopic::FieldActiveFps:
            return L"Capture rate for the screen you are looking at. Higher is smoother "
                   L"(120 max); lower saves CPU/GPU when the image is mostly static.";
        case HelpTopic::FieldMidFps:
            return L"Capture rate for screens near your gaze but not centred. 30 is a good "
                   L"balance; raise it if side screens visibly lag while turning.";
        case HelpTopic::FieldIdleFps:
            return L"Capture rate for screens far from your gaze. 1 keeps them alive "
                   L"cheaply; they ramp up as you look over.";
        case HelpTopic::FieldEnterDeg:
            return L"How close to a screen's centre, in degrees, your gaze must get before "
                   L"it counts as active. Lower switches sooner; higher needs a direct look.";
        case HelpTopic::FieldLeaveDeg:
            return L"Gaze angle where an active screen drops back to the mid rate. Keep "
                   L"below Enter; the gap between them stops rapid flickering.";
        case HelpTopic::HeaderRefresh:
            return L"Re-runs all health checks and re-reads config files from disk. Press "
                   L"it after editing JSON by hand or reconnecting hardware.";
        case HelpTopic::Count:
            break;
    }
    return L"";
}

inline HelpTopic help_for_field(LayoutField field) {
    return static_cast<HelpTopic>(static_cast<int>(HelpTopic::FieldYaw) +
                                  static_cast<int>(field));
}

}  // namespace gt
