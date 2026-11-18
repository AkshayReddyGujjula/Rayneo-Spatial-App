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
        |  status file              logs\engine-status.txt  (key=value, rewritten 1/s (4/s while the wearer
        |                           holds still), read at <= 2 Hz)
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
| `app_selftest` | test binary | app-model regression (config, presets, add/remove, session log archive, commands, telemetry, pages/theme preferences, setup guide, calibration progress protocol, animation maths) |
| `ui_snapshot` | test binary | renders the real UI offscreen for every page at several sizes, DPIs and both themes and checks the hotspot layout (inside the window, no overlaps, every help mark has a topic). `ui_snapshot --out scratch\ui` also writes the frames as PNGs; `--only NAME` filters |

`spatial_desk.exe`, `orientation_calibrate.exe` and the existing test binaries keep their names and
behaviour. `spatial_desk.exe` gained exactly two things: the private controller messages and the
optional `--status FILE` switch (both additive; the existing keys, hotkeys and CSV columns are
unchanged).

## 3. Using the app

A sidebar on the left switches between six pages (`Ctrl+1` … `Ctrl+6`); its bottom card always
shows the engine state, and clicking it returns to Home. Every setting keeps its `?` hover help.

| Page | What it holds |
|---|---|
| **Home** | The state in one sentence with the next action (Start workspace / Start preview, or Recenter / Reload layout / Stop while running, with screens, frame rate and uptime), a **Readiness** checklist (glasses display, head calibration, sensor, virtual display driver, layout file) with a fix link for each problem, a layout thumbnail and live head angles. While the engine starts, the hero shows its startup steps (see *Startup steps* below) |
| **Layout** | Presets, the arrangement editor (2D map / 3D view, fullscreen), the screen list with Add/Remove and the selected-screen inspector; **Save and apply** and **Revert** in the page header |
| **Comfort** | Reading stabilisation, screen dimming (with per-screen or focus sliders), night tint and warmth, cursor to centre |
| **Tracking** | Yaw/pitch tracking (live), preview without head tracking, motion prediction, the magnetic heading lock status, and both calibrations (in-app with live progress, or in a console window) |
| **Diagnostics** | All eight health checks with their raw detail, Recover displays, the engine log and logs folder, the controller activity log and the newest engine log lines; **Refresh** in the header |
| **Settings** | Theme (Dark, Light, Match Windows), keep running in the notification area, run the setup guide again, keyboard shortcuts, version and paths, Quit |

### First-run setup guide

On first launch (`onboarding_complete` missing or false in `config/app.json`) a modal guide walks
through: **Welcome** → **Glasses** (read-only checks: display present, Extend mode, motion sensor,
Parsec VDD, each with a plain-language fix and *Check again*) → **Head calibration** →
**Compass** (optional magnetometer calibration; do it wearing what you normally wear, earbuds
included) → **Layout** (keep the current layout or pick a factory arc) → **Done**. Both
calibration steps and the guide itself can be skipped; finishing or skipping sets
`onboarding_complete`, and *Settings → Run setup guide* shows it again. The guide never starts the
engine and never touches displays; the only file it writes besides `app.json` is the layout,
and only when a preset was picked and **Finish** pressed.

### In-app calibration

*Calibrate now* (Tracking page or setup guide) runs `orientation_calibrate.exe --progress-json
--output <calibration>` (plus `--mag` for the compass) with no console window and renders its
progress: step *n of N*, a progress bar, the current instruction, an optional signal-quality bar,
and **I'm ready** whenever the tool waits for the wearer (it sends ENTER on the tool's stdin;
**Cancel** sends `q` and closes stdin, terminating the tool only if it has not exited 4 s later).
The tool prints one line per state change:

```
@@progress {"flow":"orientation"|"mag","step":2,"steps":5,"state":"waiting"|"recording"|"ok"|"retry"|"failed"|"done","prompt":"…","detail":"…","quality":0.8}
```

Other output lines are ignored (the last few are shown if the run fails). If the tool exits
without a `done` or `failed` line, the run fails with the exit code and offers **Try again**.
Calibration is disabled while the engine runs (both need the sensor). **Open in console** keeps
the previous behaviour: the tool in its own console window.

### Startup steps and the hold-still prompt

The engine reports its startup phase in the status file (`phase=` and `phase_progress=`, contract in
`src/app/engine_protocol.h` and `docs/overnight/r4-startup-phases.md`). While it starts, the Home hero
replaces the generic "Starting the engine" with four steps — **Screens**, **Glasses sensor**,
**Warm-up**, **Hold still** — and a progress bar for the warm-up and the hold-still measurement:

| Engine phase | Hero says |
|---|---|
| `preparing`, `takeover`, `waiting_display` | Starting the engine / Setting up your screens (up to half a minute) / Waiting for the glasses display |
| `connecting` | Connecting to the glasses sensor |
| `imu_warmup` | Warming up the sensor; for the last 40% of the 4 s warm-up **Get ready to hold still** |
| `hold_still` | **Hold your head still** on an accent panel; the bar fills over the ~1.3 s measurement and drops back to empty if the head moves |
| `tracking`, `running`, `restoring` | the normal Running / Stopping view |

The startup gyro-bias window is measured during *Hold still*; a head movement there used to be the
biggest source of early drift (AGENTS.md bug 26). Before a start the hero also says *After you start,
look straight ahead and keep your head still for a few seconds when asked.* An engine without the
`phase` key, or one reporting a value this controller does not know, keeps the generic view. While the
engine reports the warm-up or hold still, a second timer re-reads only the status file at 2 Hz. The
glasses themselves show no prompt: the renderer has no text overlay.

### Start workspace vs Start preview

The two actions are deliberately separate and neither silently degrades into the other:

| Action | Command line | Creates virtual desktops? |
|---|---|---|
| **Start workspace** | `spatial_desk.exe --layout … --calibration … --log … --status …` | yes, one Parsec VDD desktop per layout screen |
| **Start preview** | the same plus `--no-virtual-displays` | no; the engine renders labelled test screens only |
| **Start preview** with *Preview without head tracking* ticked | the same plus `--no-imu` | no; fixed camera, no calibration needed |

A blocked action is disabled and explains itself on Home, for example:

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
`Ctrl+Alt+P` pitch, `Ctrl+Alt+S` reading stabilisation (cycles off / low / medium / high / ultra;
the dashboard adopts and remembers the level), `Ctrl+Alt+F` cursor to the middle of the centre
screen, `Ctrl+Alt+Q` quit.

### View comfort

The **Comfort** page is saved in `config/app.json` (`view_comfort`), passed to the engine at
start and applied live while it runs:

| Setting | Choices | Effect |
|---|---|---|
| Reading stabilisation | Off, Low, Medium (default), High, Ultra | Holds the screens steady while your head is nearly still (a soft deadband after the 1-euro filter). Higher holds harder and trails more on a turn; Ultra is the maximum (1.0 deg hold). |
| Screen dimming | Off (default), Manual, Focus | Manual: one brightness slider per screen (10..100 %). Focus: the screen you look at stays bright, the others fade to the *Screens you are not looking at* level (0.25 s fade). |
| Night tint | off (default) / on, warmth 0..100 % | Warms every screen by cutting blue and some green; 100 % is roughly a warm-white lamp. |
| Cursor to centre screen | button, or `Ctrl+Alt+F` | Moves the pointer to the middle of the layout screen nearest straight ahead. |

### Keyboard

| Key | Effect |
|---|---|
| `Tab` / `Shift+Tab` | move focus through every control; focus rings appear once you use the keyboard and hide again on a mouse click |
| arrows | adjust the focused slider (`PgUp`/`PgDn` ×10, `Home`/`End` to the limit); move the selection of a focused segmented control (stabilisation, dimming, theme, 2D/3D); on a sidebar item, Up/Down change page. On a focused button they scroll its page instead |
| `Space` / `Enter` | activate the focused button, chip or toggle |
| `Ctrl+1` … `Ctrl+6` | Home, Layout, Comfort, Tracking, Diagnostics, Settings |
| `Esc` | leave the fullscreen editor; otherwise hide to the tray (or quit when *Keep running in the notification area* is off). Ignored while the setup guide is open |
| `Ctrl+S` | save the layout and reload the engine |
| mouse wheel | scroll the page or panel under the cursor (scrollbars appear wherever content overflows; drag the thumb or click the track to page). Over a slider in a panel with nothing to scroll, the wheel adjusts the slider instead |

### Animation and accessibility

Hover and press states ease in and out, segmented controls, the sidebar selection and switches
slide, sliders tween when changed from the keyboard, pages cross-fade, the engine status pulses
while starting or stopping, and toasts slide in. The animation timer (15 ms) runs only while a
value is still moving and stops as soon as everything has settled, so an idle window uses no CPU.
With Windows *Show animations* off (`SPI_GETCLIENTAREAANIMATION`) every value snaps and nothing
pulses. The window is per-monitor DPI aware (fonts and every size are scaled; it re-lays out on
`WM_DPICHANGED`), double-buffered, and resizes down to 760 × 560; below about 1000 px wide the
sidebar collapses to icons.

## 4. Layout editor

* **Presets** — factory layouts include single centred, triple arc (default, −45°/0°/+45°),
  quad arc, five arc and a wide 3 × 4 m arc. Named layouts can also be saved and selected; the
  shipped names include `triple` and `ultrawide`. A preset is loaded into the editor and only
  written when you press **Save and apply**.
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
* **Save and apply** validates, writes `config/layouts/default.json` atomically through
  `gt::save_layout` (temporary file + `MoveFileEx`), and then tells the engine to reload it
  immediately. **Revert** re-reads the file from disk and discards unsaved edits.
* **Splitter** — drag the divider between the arrangement editor and the inspector; the
  inspector's share of the row is remembered in `splits.column`. (The earlier dashboard had
  splitters between its six stacked panels; those panels are now separate pages, so
  `splits.left` / `splits.right` are still read and written for compatibility but no longer
  used.)

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
  "last_engine_error": "",
  "window": { "x": 120, "y": 90, "width": 1180, "height": 860, "maximized": false },
  "view_comfort": { "stabilise": "medium", "dim_mode": "off",
                    "screen_brightness_pct": [100, 100, 100, 100, 100, 100, 100, 100],
                    "focus_dim_pct": 40, "night_tint": false, "night_tint_pct": 50 },
  "onboarding_complete": true,
  "theme": "dark",
  "last_page": "home",
  "engine_options": { "predict_ms": 35, "mag_track_hard_iron": false }
}
```

| Field | Accepted range | Meaning |
|---|---|---|
| `layout_path`, `calibration_path`, `log_dir` | 1..512 printable chars, relative to the app folder or absolute | where the engine reads and writes |
| `monitor_index` | `-1` (auto: the RayNeo display) or `0..15` | forwarded to the engine as `--monitor` |
| `last_mode` | `none`, `workspace`, `preview` | remembered for the header chip and diagnostics |
| `health_poll_ms` | `500..10000` | dashboard polling; 500 ms is the hard 2 Hz floor |
| `window` | width ≥ 320, height ≥ 240 | restored on start and clamped to the work area |
| `view_comfort.stabilise` | `off`, `low`, `medium`, `high`, `ultra` | reading stabilisation (`--stabilise`) |
| `view_comfort.dim_mode` | `off`, `manual`, `focus` | screen dimming (`--dim-mode`) |
| `view_comfort.screen_brightness_pct` | up to 8 values, 10..100 | manual brightness per layout screen (`--screen-brightness`) |
| `view_comfort.focus_dim_pct` | 10..100 | brightness of the screens you are not looking at (`--focus-dim`) |
| `view_comfort.night_tint` / `night_tint_pct` | bool / 0..100 | warm tint (`--night-tint`, 0 = off) |
| `onboarding_complete` | bool (missing = false) | the first-run setup guide was finished or skipped |
| `theme` | `dark` (default), `light`, `system` | controller colours; `system` follows the Windows app theme |
| `last_page` | `home`, `layout`, `comfort`, `tracking`, `diagnostics`, `settings` | page shown at launch; an unknown value falls back to Home |
| `engine_options.predict_ms` | int 0..100 (default 35; UI offers 0/20/35/50) | `--predict-ms`: turn prediction horizon, 0 = off; applies at the next engine start. An old `motion_prediction` key is ignored |
| `engine_options.mag_track_hard_iron` | bool (default false) | `--mag-track-hard-iron`: experimental earbud magnet compensation; applies at the next engine start |

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
| Session archives | `logs/sessions/*.zip` | the last three sessions over 10 minutes, and separately the last three failed sessions (`failed-session-*.zip`, any length); replay `imu_raw.csv` with `imu_replay` |

* **Session logs.** Every engine session starts on empty files: `engine.log`, `telemetry.csv`,
  `imu_raw.csv` (every IMU sample) and `engine-status.txt`. When a session that ran for **more
  than 10 minutes** ends (stopped, quit, exited or crashed), its files are moved into
  `logs/sessions/.pending-session-<start time>/` and compressed into
  `logs/sessions/session-YYYY-MM-DD_HH-MM-SS.zip` by a background copy of the controller
  (`RayNeo Spatial.exe --archive-sessions <dir>`, Windows' own `tar.exe`). Only the **newest three**
  archives are kept: the fourth session's archive removes the first. Quitting never waits for the
  compression, a zip only appears under its final name once it is complete, and anything a crash
  or power cut left pending is archived the next time the controller starts. Shorter successful
  sessions are not archived; their files stay in `logs/` until the next session starts.
  A session whose engine **failed** (the controller showed Failed, a non-zero exit code, or
  `engine-status.txt` says `state=failed`) is kept whatever its length: its files stay in `logs/`
  for the Diagnostics page and Open engine log, with a `session-failed.txt` marker (stem and
  reason), and the next start stages them as
  `logs/sessions/failed-session-YYYY-MM-DD_HH-MM-SS.zip` before it clears the folder. A failed
  status left on disk by a controller that has since restarted is kept the same way. Failed
  archives keep their own newest three, pruned separately, so failures never evict a long
  session's archive. Other files in
  `logs/sessions/` (for example a reference archive you keep) are never touched. The telemetry CSV
  is read back at no more than 2 Hz and only its tail is parsed.
* **Polling**: the health timer is the only recurring timer, it can never be faster than 500 ms
  (twice per second), and HID/VDD/calibration checks run at most every 2 s. A second timer exists
  only while something animates (see *Animation and accessibility*) and is killed as soon as it
  settles; the idle UI is fully event-driven. In-app calibration output arrives through a posted
  message from the reader thread, not a poll. A status-only 500 ms timer runs only while the engine
  reports the sensor warm-up or the hold-still measurement (a few seconds per start), and skips its
  read when the last one is under 0.5 s old.
* **Recovery states** are explicit: the banner (dismissible) names the failure, `Open engine log`
  shows the engine's own output, the Tracking page runs calibration in-app or in its own console,
  and the health checks keep the VDD/calibration/display state visible. A failed engine never leaves a stale "ready"
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
layouts and the named presets, and per-device `orientation.json` only with `-IncludeCalibration`),
`docs\`, `README.md`, `LICENSE`, the shortcut helper, an empty `logs\` and a
`PORTABLE-NOTES.txt`. The executables resolve everything by walking up from their own location, so the staged folder needs no source tree.
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
   `config/orientation.json` and the app refuses to start head tracking without it. Then run
   `orientation_calibrate.exe --mag` once (turn fully around and nod/tilt while wearing them): it adds the
   magnetometer calibration to the same file, and the engine then locks yaw to the local field so
   the workspace cannot drift in heading.
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
* The controller never installs a driver; that stays with you. The magnetometer heading lock is
  engine-side and switches on only when `config/orientation.json` carries a `--mag` calibration.
  The only display change it can make is the manual Recover displays button (re-applies the Extend topology); the workspace takeover and its restore belong to the engine.
* One engine at a time: `spatial_desk.exe` keeps its single-instance guard, and the controller
  attaches to an already-running engine instead of starting a second one.
* Without a magnetometer calibration there is no absolute heading reference, so a steady slow yaw
  is indistinguishable from yaw bias; the estimator's trade-offs are documented in `AGENTS.md`. A
  field that changes while you work (a magnet, a moving laptop lid near your head) is gated out
  rather than corrected.
* Preview without head tracking is a fixed camera: it exists to verify the render path, and the
  side screens of the triple-screen world cannot be viewed by turning (the dashboard says so when
  this mode starts). Preview also shows labelled test screens, never real desktops: only Start
  workspace (signed Parsec VDD required) creates the real Windows virtual monitors.
