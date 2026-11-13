# Track C — controller UI redesign and first-run setup guide

Overnight run, 2026-11-12. Branch `worktree-agent-af4658d0d73d6c1e7`. Everything below was
built and verified without the glasses and without a person: model code by `app_selftest`, the
UI by offscreen renders (`ui_snapshot`) that were looked at and iterated on. The engine was never
started and no display setting was touched.

## What changed

**Structure.** The six stacked dashboard panels became a sidebar with pages: Home, Layout,
Comfort, Tracking, Diagnostics, Settings. The sidebar's bottom card shows the engine state on
every page. Code: `ui_paint.cpp` (shell, shared building blocks, health/engine view models),
`ui_pages.cpp` (Home, Comfort, Tracking, Diagnostics, Settings, calibration progress block),
`ui_page_layout.cpp` (Layout page, 2D/3D editor, editor geometry, arc thumbnail),
`ui_onboarding.cpp` (setup guide), `ui_paint_internal.h` (the `Ui` paint helper).

**Design tokens** (`ui_theme.h`): a `Theme` struct with complete dark and light schemes (plus
"Match Windows"), spacing scale (2/4/8/12/16/24/32/48), radii (6/8/12), component sizes
(controls 32/40/46 px, nav 44, sidebar 236 or 72 compact, slider 58), and a larger type scale
(display 22 pt, title 14, heading 11.5, body 10.5, small 9.5, caption 9; Segoe UI, ClearType)
plus Segoe MDL2 Assets icons. Shapes are anti-aliased with GDI+; text stays GDI.

**Animation** (`ui_anim.h`): immediate-mode `Animator` (widgets declare targets each paint;
frame-rate independent exponential easing; first sight snaps; unseen keys dropped), plus
`Transition` and easing curves. Used for hover/press fades, sliding segmented pills and the
sidebar highlight, switch thumbs, slider tweening, the progress bar, the page cross-fade (old
frame alpha-blended over the new page while it eases up 14 px), the pulsing status while
starting/stopping/calibrating, and the toast entrance. The 15 ms animation timer runs only while
something moves; Windows "Show animations" off snaps everything.

**Setup guide** (`onboarding.h/.cpp` model + `ui_onboarding.cpp`): Welcome, Glasses (read-only
display / Extend / sensor / Parsec VDD checks with plain fixes and Check again), Head calibration,
Compass (optional; earbuds note), Layout preset (keep current or a factory arc), Done. Skippable
per step and as a whole; completion persisted as `onboarding_complete` in app.json; Settings →
Run setup guide re-opens it. Never starts the engine or touches displays; on Finish it saves the
picked preset as the layout (only if one was picked).

**In-app calibration** (`calibration_progress.h/.cpp` parser + run model, `calibration_runner.h/.cpp`
process with piped stdout/stdin): runs `orientation_calibrate.exe --progress-json --output …`
(`--mag` for the compass), parses `@@progress {…}` lines, shows step n/N, progress, prompt,
detail, quality, an **I'm ready** button on `waiting` (sends ENTER), Cancel (sends `q`, closes
stdin, terminates after 4 s), and a clear failure with Try again when the tool exits without
`done`/`failed`. The console launch is kept as "Open in console".

**Other polish:** focus rings only after keyboard use; Ctrl+1..6 page shortcuts; arrow keys move
segmented selections and pages; dismissible banner; toast as a floating pill; disabled controls
show an arrow cursor; plain-language readiness texts with fix links on Home while Diagnostics keeps
the raw detail; About card with version, paths and Quit; keyboard shortcut reference (replaces the
old footer); window minimum 760 × 560 with a compact icon sidebar under ~1000 px.

## Every previous feature, where it is now

| Before | Now |
|---|---|
| Status panel (8 rows + help) | Diagnostics → Health checks (8 rows + help); 5 of them as Home → Readiness |
| Engine control: Start workspace/preview, Stop, Recenter, Reload layout | Home hero (start actions when stopped; Recenter / Reload / Stop while running), tray unchanged |
| Yaw/Pitch tracking toggles, Preview without head tracking | Tracking → Head tracking |
| View comfort (stabilise, dimming + sliders, night tint + warmth, cursor to centre) | Comfort page |
| Diagnostics: Run calibration, engine log, logs folder, close to tray, recover displays, events | Tracking (calibration, in-app + console), Diagnostics (logs, recover, activity log), Settings (close to tray) |
| Layout: factory/user presets, save/delete preset, screen chips, add/remove, save/revert, summary | Layout page (presets card, inspector, header Save and apply / Revert, summary under the editor) |
| Arc editor 2D/3D, fullscreen with quick sliders | Layout → Arrangement card (2D/3D segmented, fullscreen) |
| 12 field sliders with help | Layout → inspector, grouped (Placement, Size, View, Capture rate, Gaze thresholds) |
| 5 persisted splitters | 1 splitter (editor | inspector) persisted in `splits.column`; the other four separated panels that are now pages (`splits.left/right` still round-trip) |
| Header Refresh | Home and Diagnostics headers |
| Footer shortcut hints / toast | Settings → Keyboard shortcuts / floating toast |

Behaviour that did not change: every engine command, launch command line, comfort push, layout
save/reload, preset files, tray menu, quit/stop grace, session log archive, help topic texts
(wording updated for the new page names; five topics added).

## Hooks for new engine options (integration)

* `EngineOptions` in `src/app_shell/ui_model.h` (persisted as `engine_options` in app.json,
  round-trip tested). `motion_prediction` (default false) has a Tracking-page switch
  (`kUiMotionPrediction`, help `EngineMotionPrediction`). To wire: pass it to
  `build_engine_launch_command` / a live message in `app_state.cpp`. To add an option: add the
  field + JSON key in `app_config.cpp`, a row in `tracking_card_head` (`ui_pages.cpp`), an id in
  `app_state.h`, a case in `AppWindow::execute`.
* `AppState::heading_lock` (`HeadingLockState`: Unknown/Off/Locked/Disturbed/Relearning, with
  `parse_heading_lock_state` accepting `off|locked|disturbed|relearning`). The Tracking page shows
  it as a status row with a coloured badge (Relearning pulses). To wire: set it in `poll_engine()`
  from a new status-file key or telemetry column; Unknown reads "Not reported".

## Offscreen renders (`scratch/ui/`, regenerate with `ui_snapshot --out scratch\ui`)

Final set, all `PASS` on the hotspot checks:

* `home-ready-1280x800.png`, `home-running-1920x1080-150.png`,
  `home-setup-needed-1280x800-light.png`, `home-narrow-820x640.png`
* `layout-1280x800.png`, `layout-3d-1920x1080-150.png`, `layout-fullscreen-1280x800-light.png`,
  `layout-narrow-820x640.png`
* `comfort-1280x800.png`
* `tracking-calibrating-1280x800.png`, `tracking-running-1920x1080-150-light.png`
* `diagnostics-1280x800.png`, `diagnostics-problems-1920x1080-150.png`
* `settings-1280x800-light.png`, `settings-focus-1280x800.png` (keyboard focus ring + help bubble)
* `wizard-1-welcome-1280x800.png`, `wizard-2-glasses-problems-1280x800.png`,
  `wizard-3-head-recording-1920x1080-150.png`, `wizard-3-head-failed-1280x800-light.png`,
  `wizard-4-compass-1280x800.png`, `wizard-5-layout-1280x800.png`, `wizard-6-done-1280x800-light.png`,
  `wizard-narrow-820x640.png`

## Still needs a real display / a person

1. **Run the actual window.** The paint code is the same function the renders use, but the
   message handling (hover, press, drags, splitter, wheel, keyboard, tray, `WM_DPICHANGED` when
   moving between monitors, theme switch updating the title bar) was not exercised interactively.
2. **Animation feel**: timing constants (`kTauHover` 60 ms, slides 80 ms, page 220 ms) and that
   the timer really stops when idle (Task Manager CPU at 0 % with the window open).
3. **Calibration protocol against the real tool** once Track B lands `--progress-json`:
   assumptions to confirm — `waiting` means the tool is blocked on ENTER (the UI sends a newline
   on I'm ready), the tool flushes stdout after each `@@progress` line (otherwise progress arrives
   late through the pipe), `--mag --output <file>` is the right compass invocation, and `q` on
   stdin cancels cleanly with the IMU stream turned off. Without the flag the tool fails fast and
   the UI shows the failure path (rendered in `wizard-3-head-failed`).
4. **First launch for the owner**: an existing `app.json` has no `onboarding_complete`, so the
   guide appears once on the next launch (intended; Skip setup closes it for good).
5. Light theme on a real panel, ClearType on the memory DC at 125 %/175 %, and the Segoe MDL2
   glyphs on Windows 10 (Windows 11 was used for the renders).

## Where it stopped

Complete: all requirements implemented, committed, full suite green (11 tests incl. the new
`ui_snapshot`). Possible follow-ups: per-monitor pages remembered per-session scroll, a stacked
Layout page for very narrow windows (currently editor and inspector stay side by side down to the
760 px minimum), and an animated engine-start progress readout once the engine reports phases.

## Integration (tracks A + B + C)

Integration branch on top of `overnight-claude-improvements`; MSVC RelWithDebInfo, warning-clean,
11/11 ctest at each commit. No glasses: nothing here ran against hardware.

* **Turn prediction** (Track A `--predict-ms`): the Tracking page's unwired "Motion prediction"
  switch became a *Turn prediction* segment, Off / 20 / 35 / 50 ms, default 35, with Track A's help
  text. `EngineOptions::predict_ms` is persisted as `engine_options.predict_ms`; a file without it,
  or with the old `motion_prediction` key (never wired, so ignored), loads as 35; out-of-range hand
  edits clamp to 0..100. The launch command always passes `--predict-ms N` when head tracking is on
  (never with `--no-imu`). `kUiMotionPrediction` (1434) is retired, not renumbered.
* **Earbud magnet compensation (experimental)** (Track B `--mag-track-hard-iron`): Tracking-page
  switch, default off, persisted as `engine_options.mag_track_hard_iron`; help text says it is
  measured on recordings only and can react to a real field change.
* **Heading lock badge**: `parse_engine_status` reads `mag`, `mag_error_deg`, `mag_offset_ut`;
  `poll_engine` sets `AppState::heading_lock` via `heading_lock_from_status` (new `Acquiring`
  state). Not reported when no fresh status, no `mag` key (older engine), `imu=0` or an unknown
  value. Wording follows track-b.md (Gyro only / Finding north / Locked / Interference / Checking).
* **Calibration wizard vs the real tool** (`orientation_calibrate.cpp`), mismatches found:
  1. The wizard waited for ENTER ("I'm ready"); a wearer holding a pose cannot reach the keyboard.
     In-app runs now use `--progress-json --auto --auto-delay 5`, with a "Starts in N s" countdown
     and a *Start now* button (sends an empty line).
  2. Cancel sent `q` on stdin, but `--auto` never read stdin, so every cancel ended in
     TerminateProcess with the IMU stream on. Fixed in the tool: with `--auto` and a piped stdin, a
     watcher thread (Win32 ReadFile, no CRT lock) maps `q` or EOF to the normal Ctrl+C path (stream
     off, final `failed`/`cancelled`, exit 1) and an empty line to "start now". Console use is
     unchanged. Closing the controller mid-run now cancels and waits up to 3 s before terminating.
  3. A `retry` detail was overwritten by the `waiting` line that follows within milliseconds; it
     is kept (shown as "Again: ...") until the step changes.
  4. Reason codes are shown as plain sentences (`cal_reason_text`; setup codes such as NO_DEVICE
     get a fix-it sentence), exit code 2 names an outdated tool, and a tool-side `cancelled` reads
     as cancelled. Exit codes 0/1 and the done/failed final line already matched.
  5. `--mag --output <file>` was already right (the tool reads the orientation from and merges the
     mag block into the same file). The compass step and card gain an *Earbuds in* switch mapped to
     `--earbuds` (also on the console launch); it is session-only, not persisted.
* Tests: `app_selftest` (options -> command line, status keys -> state, old-file loading, tool
  arguments, countdown, retry reason, plain failures, exit 2); `orientation_calibration_selftest`
  (stdin command parser). Renders: `tracking-running-1920x1080-150-light`,
  `tracking-compass-earbuds-1280x800`, `tracking-calibrating-1280x800` (retry + countdown, below
  the fold), `wizard-4-compass-1280x800`, `wizard-4-compass-countdown-1280x800`,
  `wizard-3-head-failed-1280x800-light` (NO_DEVICE before step 1).
* Unverified: the stdin watcher and graceful cancel against real glasses (does the stream really go
  off, how long the stop takes); whether 5 s is the right reading delay; the live `mag` badge.
