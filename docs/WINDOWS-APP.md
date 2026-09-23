# RayNeo Spatial — Windows controller app

`RayNeo Spatial.exe` is the native controller for the RayNeo GT spatial workspace. It does not
render anything itself: it launches, watches and steers `spatial_desk.exe`, which stays the
renderer engine (D3D11 swapchain on the glasses, Parsec virtual displays, Desktop Duplication
capture, IMU fusion). Pose fusion, camera-frame handling, calibration maths and drift behaviour
are untouched by the controller.

```
RayNeo Spatial.exe  (this app, WIN32 subsystem, taskbar identity RayNeo.Spatial.Desktop)
        |
        |  CreateProcessW            "spatial_desk.exe --layout ... --calibration ... --log ... --status ..."
        |  private window messages   WM_APP+0x101..0x106  (query, quit, recenter, yaw, pitch, reload)
        |  status file              logs\engine-status.txt  (key=value, rewritten 1/s, read at <= 2 Hz)
        v
spatial_desk.exe  (renderer engine: D3D11, VDD desktops, capture, IMU)
```

## 1. Requirements

| Item | Detail |
|---|---|
| Windows | Windows 10 1809+ or Windows 11, x64 |
| Compiler | Visual Studio 2022 Build Tools (MSVC 14.4x) + Ninja + CMake 3.25+ |
| Dependencies | `hidapi` and `nlohmann-json` through the vcpkg manifest; no new runtime or package dependency was added for the controller |
| Glasses | RayNeo GT connected by USB, Windows display mode **Extend** (`Win+P` → Extend) |
| Virtual displays | The signed **Parsec VDD** driver, installed by you. It is not bundled and the app never installs it. Without it use **Start preview**. |
| Calibration | `config/orientation.json` produced by `orientation_calibrate.exe` while wearing the glasses |

## 2. Build and run

Always build in a private Ninja directory (never the shared `build/`), inside a Developer Command Prompt for VS 2022. Set `VCPKG_ROOT` to your local vcpkg checkout:

```cmd
set "VCPKG_ROOT=C:\path\to\vcpkg"
cmake -S . -B build\agent-app -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
cmake --build build\agent-app
ctest --test-dir build\agent-app --output-on-failure
```

Targets added by the controller:

| Target | Output | Purpose |
|---|---|---|
| `rayneo_app_model` | static library | preference file, layout editing rules, engine command contract, telemetry tail reader (no windows) |
| `rayneo_app_ui` | static library | Win32 window, tray, engine process owner, read-only health checks |
| `rayneo_spatial_app` | `RayNeo Spatial.exe` | the GUI controller (WIN32 subsystem) |
| `app_selftest` | test binary | pure app-model regression (config, presets, add/remove, rotation, commands, telemetry) |

`spatial_desk.exe`, `orientation_calibrate.exe` and the existing test binaries keep their names and
behaviour. `spatial_desk.exe` gained exactly two things: the private controller messages and the
optional `--status FILE` switch (both additive; the existing keys, hotkeys and CSV columns are
unchanged).

## 3. Using the dashboard

The window has three regions: **Status** (what the machine reports), **Engine control** (the two
explicit launch actions plus live commands) and the **Layout editor** (presets, screen chips, the
arc editor and the sliders).

### Start workspace vs Start preview

The two actions are deliberately separate and neither silently degrades into the other:

| Action | Command line | Creates virtual desktops? |
|---|---|---|
| **Start workspace** | `spatial_desk.exe --layout … --calibration … --log … --status …` | yes, one Parsec VDD desktop per layout screen |
| **Start preview** | the same plus `--no-virtual-displays` | no; the engine renders labelled test screens only |
| **Start preview** with *Preview without head tracking* ticked | the same plus `--no-imu` | no; fixed camera, no calibration needed |

A blocked action is disabled and explains itself in the panel, for example:

* `Parsec VDD is Not installed; full workspace needs the signed driver, Start preview does not`
* `orientation calibration is not usable: missing; run orientation_calibrate.exe`
* `no RayNeo display is present; set the glasses to Extend mode (Win+P) first`
* `spatial_desk.exe was not found next to this app`

The status rows always show the truth for **Parsec VDD**, **Orientation calibration**,
**RayNeo HID** and **RayNeo display**, so the app never claims VDD-backed desktops when the driver
is unavailable.

### Live engine commands

While the engine runs: **Recenter**, **Yaw tracking** and **Pitch tracking** (state is read back
from the engine, not guessed), **Reload layout**, and **Stop** (a graceful quit message; a forced
terminate only happens if the engine ignores it for 8 s and is reported in the status panel).
The existing global hotkeys keep working: `Ctrl+Shift+R` recenter, `Ctrl+Alt+Y` yaw,
`Ctrl+Alt+P` pitch, `Ctrl+Alt+Q` quit.

### Keyboard

| Key | Effect |
|---|---|
| `Tab` / `Shift+Tab` | move focus through every control (focus ring is always visible) |
| arrows | adjust the focused slider; `PgUp`/`PgDn` ×10, `Home`/`End` to the limit. On a focused button they scroll its panel instead |
| `Space` / `Enter` | activate the focused button, chip or toggle |
| `Esc` | hide to the tray (or quit when *Close to tray* is off) |
| `Ctrl+S` | save the layout and reload the engine |
| `R` | recenter |
| mouse wheel | scroll the panel under the cursor (scrollbars appear wherever content overflows; drag the thumb or click the track to page). Over a slider in a panel with nothing to scroll, the wheel adjusts the slider instead |

## 4. Layout editor

* **Presets** — factory layouts include single centred, triple arc (default, −45°/0°/+45°),
  quad arc, five arc and a wide 3 × 4 m arc. Named layouts can also be saved and selected; the
  shipped names include `triple` and `ultrawide`. A preset is loaded into the editor and only
  written when you press **Save and reload**.
* **Screens** — 1 to 8. **Add screen** allocates the lowest free `screen-N` id and the lowest free
  `vdd_index` (0..15); **Remove** frees both for reuse. Ids and indices are always unique, which is
  what `gt::validate_layout` enforces.
* **Arc editor** — drag a screen left/right to change its yaw, up/down to change its pitch; the
  crosshair is the wearer, the rings are 1..4 m and the ticks are yaw.
* **3D editor** — switch to 3D and optionally fullscreen. Click anywhere on a screen to select
  it; drag that screen to move it under the pointer, or drag empty space to orbit the view.
  Mouse wheel over the view zooms the editor camera. The fullscreen view keeps yaw, pitch,
  depth from your eyes, roll, width and height controls for the selected screen beside Save and
  Revert. A 3D drag may also change depth to keep the screen under the pointer at steep orbit
  angles; use the selected screen's depth slider to fine-tune it. The slider uses a geometric
  scale for finer control near normal viewing distances while retaining the full 0.25–20 m range.
* **Sliders** — yaw, pitch, roll, distance, width, height for the selected screen, plus field of
  view and the capture policy (active/mid/idle FPS, enter/leave hysteresis). Coupled values are
  normalised as you drag: `1 <= idle <= mid <= active` and `enter > leave` always hold.
* **Save and reload** validates, writes `config/layouts/default.json` atomically through
  `gt::save_layout` (temporary file + `MoveFileEx`), and then tells the engine to reload it
  immediately. **Revert** re-reads the file from disk and discards unsaved edits.

`config/layouts/default.json` keeps the exact schema the engine has always read, so an edited file
stays compatible with `spatial_desk.exe --layout`.

## 5. Preferences: `config/app.json`

Validated on load and on save; writes are atomic (temporary file + `MoveFileEx` with
`MOVEFILE_WRITE_THROUGH`), and an invalid value is rejected instead of guessed. Unknown keys are
ignored within version 1, while an unsupported version is rejected explicitly.

```json
{
  "version": 1,
  "layout_path": "config/layouts/default.json",
  "calibration_path": "config/orientation.json",
  "log_dir": "logs",
  "monitor_index": -1,
  "last_mode": "none",
  "preview_without_head_tracking": false,
  "close_to_tray": true,
  "health_poll_ms": 1000,
  "engine_log":   { "max_bytes": 1048576, "max_files": 3 },
  "telemetry_log": { "max_bytes": 4194304, "max_files": 3 },
  "last_engine_error": "",
  "window": { "x": 120, "y": 90, "width": 1180, "height": 860, "maximized": false }
}
```

| Field | Accepted range | Meaning |
|---|---|---|
| `layout_path`, `calibration_path`, `log_dir` | 1..512 printable chars, relative to the app folder or absolute | where the engine reads and writes |
| `monitor_index` | `-1` (auto: the RayNeo display) or `0..15` | forwarded to the engine as `--monitor` |
| `last_mode` | `none`, `workspace`, `preview` | remembered for the header chip and diagnostics |
| `health_poll_ms` | `500..10000` | dashboard polling; 500 ms is the hard 2 Hz floor |
| `engine_log.max_bytes` / `telemetry_log.max_bytes` | 64 KiB .. 64 MiB | rotation threshold |
| `engine_log.max_files` / `telemetry_log.max_files` | 1..9 | rotated generations kept (`engine.log.1` …) |
| `window` | width ≥ 320, height ≥ 240 | restored on start and clamped to the work area |

The controller writes this file at runtime (window placement, last mode, last engine error), so a
dirty working tree after running the app is expected.

## 6. Diagnostics, logs and recovery

| Row | Source | Notes |
|---|---|---|
| Engine | process state + `logs/engine-status.txt` + window query | pid, uptime, screens, monitor, mode, last error |
| RayNeo display | `EnumDisplayMonitors` | shows the device name and resolution, or tells you to use Extend mode |
| Orientation calibration | `gt::load_orientation_calibration` | validity, file age and the mounting basis rows |
| RayNeo HID | `hid_enumerate` (VID 3941 / PID AF50) | interface count, product, usage page/usage |
| Parsec VDD | `VddClient::driver_status()` (read-only registry query) | `Ready`, `Not installed`, `Disabled`, … |
| Layout file | `gt::load_layout` | validity, screen count, arc, FOV, capture policy, file age |
| Telemetry CSV | newest row of `logs/telemetry.csv` | yaw/pitch/roll, bias, rest/still, adaptation state, deviation |
| Engine log | `logs/engine.log` | console output captured from the engine process |

* **Log rotation** is bounded and happens *before* a launch, while the engine does not hold the
  files: the previous run is shifted to `.1`, `.2`, … and the oldest generation is deleted. The
  telemetry CSV is read back at no more than 2 Hz and only its tail is parsed.
* **Polling**: the health timer is the only recurring timer, it can never be faster than 500 ms
  (twice per second), and HID/VDD/calibration checks run at most every 2 s. A second timer exists
  only while a transition is on screen (engine starting, toast fading) and is killed immediately
  afterwards; the idle UI is fully event-driven.
* **Recovery states** are explicit: the banner names the failure, `Open engine log` shows the
  engine's own output, `Run calibration` starts the guided tool in its own console, and the status
  rows keep the VDD/calibration/display state visible. A failed engine never leaves a stale "ready"
  claim: `logs/engine-status.txt` carries `state=failed` plus the reason.

## 7. Tray lifecycle and pinning

* Closing the window hides it to the notification area (configurable with *Close to tray*); the
  tray menu offers **Show window**, **Start workspace**, **Start preview**, **Stop engine**,
  **Recenter**, **Reload layout** and **Quit**. The icon is re-added automatically if Explorer
  restarts. If the icon could not be created, closing quits instead of stranding the window.
* **Quitting stops the engine first** (graceful quit message, then a bounded wait that covers
  the 8 s forced-terminate grace), so the controller never orphans `spatial_desk.exe` or its
  Parsec desktops. System shutdown/logoff sends the same quit request without the wait.
* **Starting while an engine was launched by hand adopts it** instead of launching a second
  copy: the dashboard attaches to the running window and reports the adoption in the event log.
* The engine panel only shows *fresh* status: `logs/engine-status.txt` content older than 3 s
  (or missing, as with a hand-launched engine without `--status`) is shown as "no status yet",
  never as a stale "ready".
* The process calls `SetCurrentProcessExplicitAppUserModelID(L"RayNeo.Spatial.Desktop")` before any
  window exists, so the taskbar button, the tray icon and the Start Menu shortcut share one stable
  identity.
* **Pinning is manual.** Right-click the running taskbar button → *Pin to taskbar*, or create a
  Start Menu shortcut and pin that:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\create-start-menu-shortcut.ps1 `
    -AppPath "C:\Tools\RayNeoSpatial\RayNeo Spatial.exe"
```

The helper writes the `.lnk`, tags it with the same AppUserModelID and prints the pinning steps.
It never pins and never installs anything.

## 8. Portable folder

```powershell
powershell -ExecutionPolicy Bypass -File scripts\stage-portable.ps1 -BuildDir build\agent-app
```

Stages `dist\RayNeoSpatial` with `RayNeo Spatial.exe`, `spatial_desk.exe`,
`orientation_calibrate.exe`, `hidapi.dll`, `config\` (app.json with developer state stripped,
layouts, and per-device `orientation.json` only with `-IncludeCalibration`), `docs\`, `README.md`,
the shortcut helper, an empty `logs\` and a `PORTABLE-NOTES.txt`. The executables resolve
everything by walking up from their own location, so the staged folder needs no source tree.
Nothing is installed, no driver is touched, nothing is pinned. Calibrate from the staged folder
after staging: copying another machine's `orientation.json` would silently bias head tracking.

## 9. Manual hardware checks the app cannot do for you

1. **Extend mode** — `Win+P` → Extend; the RayNeo display must appear in Windows display settings.
   Duplicate/PC-screen-only makes the glasses invisible to the engine.
2. **120 Hz** — set the glasses to 1920 × 1080 @ 120 Hz in Windows display settings for the best
   motion-to-photon feel.
3. **Parsec VDD** — the signed driver must be installed by you (device class `PSCCDD0`). The
   controller only reports its status; it never installs or removes it. Verified procedure
   (0.45, signature `Parsec Cloud, Inc.`): download `parsec-vdd-0.45.0.0.exe` from
   `https://builds.parsec.app/vdd/`, run it with `/S`, then run
   `C:\Program Files\Parsec Virtual Display Driver\vddinstall.bat` from an elevated prompt
   (the installer only extracts files; the script registers the `Root\Parsec\VDA` device).
   No reboot was required; the dashboard row flips to `ready` immediately.
4. **Calibration** — wear the glasses normally and run the calibration tool; it writes
   `config/orientation.json` and the app refuses to start head tracking without it.
5. **Workspace sanity** — after *Start workspace*, confirm the new virtual desktops in Windows
   display settings, then move the mouse onto each one and check the dashboard telemetry and the
   engine log for capture errors.
6. **Recentering** — press `Ctrl+Shift+R` or the tray **Recenter** entry, then look straight ahead and
   confirm the centre screen sits in front of you.
7. **Stopping** — use **Stop engine** and confirm the virtual desktops disappear. If the engine is
   killed with Task Manager, the Parsec driver watchdog removes them; that path is not the normal
   one.

## 10. Limitations, stated plainly

* Non-ASCII install paths are supported: the engine uses a wide entry point and every file path
  is handled as UTF-8 end to end (command line, status file, telemetry, layout, calibration).
* The controller never installs a driver and never enables the magnetometer; all of that stays with
  the engine and with you. The only display change it can make is the manual Recover displays
  button (re-applies the Extend topology); the workspace takeover and its restore belong to the engine.
* One engine at a time: `spatial_desk.exe` keeps its single-instance guard, and the controller
  attaches to an already-running engine instead of starting a second one.
* With no absolute heading reference (magnetometer off) a steady slow yaw is indistinguishable from
  yaw bias; the estimator's trade-offs are documented in `AGENTS.md` and are unchanged here.
* Preview without head tracking is a fixed camera: it exists to verify the render path, and the
  side screens of the triple-screen world cannot be viewed by turning (the dashboard says so when
  this mode starts). Preview also shows labelled test screens, never real desktops: only Start
  workspace (signed Parsec VDD required) creates the real Windows virtual monitors.
