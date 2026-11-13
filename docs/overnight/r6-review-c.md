# R6-C review: controller window, state and model

Scope: `src/app_shell/app_window.*`, `app_state.*`, `app_config.*`, `engine_commands.*`,
`engine_client.*`, `session_logs.*`, `diagnostics.*`, `onboarding.*`, `ui_model.*`,
`ui_input.h`, `app_layout.*`, `main.cpp` and their `app_selftest` coverage, as changed by
`main...overnight-claude-improvements`. Painting and calibration internals are out of scope
(other reviewers).

Baseline: 11/11 ctest green, no warnings (MSVC Ninja RelWithDebInfo, `/W4 /permissive-`).
After the fixes: 11/11 green, no warnings.

## Fixed (each with a regression check in `app_selftest`)

| # | Where | Finding | Commit |
|---|---|---|---|
| 1 | `app_window.cpp` `execute(kUiRunSetupGuide)` | **Run setup guide orphaned a live in-app calibration.** The case reset `state_.cal_run` unconditionally (every other reset path is guarded by `running()`). Trace: start a head calibration on Tracking, Ctrl+6 to Settings, click Run setup guide: `CalRunState` goes Idle while the tool keeps streaming; `apply()` and `process_exited()` ignore a non-running state, so progress and the exit are lost; the Cancel button and the glasses guide window disappear; Start stays refused ("Finish or cancel the calibration first") for a calibration nobody can see or cancel; the wizard's own Start button is enabled but silently ignored (`start_calibration` returns on `calibration_.active()`). Fix: `open_setup_guide()` in `ui_input.h` refuses while the process exists or the run is running; the click shows a toast. 5 checks fail without the guard (verified by disabling it). | `f3d6b07` |
| 2 | `app_window.cpp` `execute(kUiLaunchCalibration / kUiCalMagConsole)` | **Console calibrations launched while the sensor was in use.** No engine or in-app check. The Tracking card enables "Open in console" whenever no in-app run is *shown* (`!run_active`), which includes a cancelled run still in its shutdown grace, and it never considers the engine. Two tools on one sensor both write `orientation.json`; beside an engine, the tool's exit turns the IMU stream off under it (the contract `engine_start_calibration_block` already documents). The head button's lack of an engine check predates the branch; the compass button is new. Fix: `console_calibration_block()` gates both clicks with a banner. | `582353d` |

## Follow-up fixes (coordinator request, commit `c32a5b5`)

All four go through `ui_input.h` seams. Reverting each seam to the old behaviour fails 6
`app_selftest` checks; with the fixes, all pass and ctest is 11/11.

| # | Where | Was | Now |
|---|---|---|---|
| 3 | `execute(kUiStartWorkspace/Preview)` | The tray menu could start the engine under the open setup guide | Refused by `engine_start_guide_block()` with a toast (the banner is painted under the modal) and an event-log line |
| 4 | `on_end_session` | Logoff/shutdown skipped the calibration, so Windows killed the tool with the IMU stream on | `stop_calibration_gracefully(calibration_, 2000 ms)`: cancel (`q` + stdin EOF, the tool's stream-off path from bug 27), bounded wait, then terminate only if it is still running. `on_destroy` uses the same helper with its 3 s. **Not proven live:** Windows allows about 5 s in WM_ENDSESSION by default before it shows the "preventing shutdown" screen or ends the app. The tool's cancel-to-exit time is stated in bug 27 as a few hundred ms, but nobody has measured it on hardware. The 2 s cap keeps us inside the allowance either way. In the worst case (the tool hangs) the behaviour is the old one: terminate. |
| 5 | `start_calibration` | Start/Retry during a cancelled run's 4 s shutdown grace did nothing | `calibration_start_block()` shows a toast: "The previous calibration is still closing; try again in a moment." |
| 6 | `navigate()` (Ctrl+1..6, sidebar arrows) | A captured drag continued on the previous page's cached track | `pointer_on_page_change()` ends the drag like a button-up, **with the normal save** of a moved splitter or comfort slider and no click, then releases capture. The resulting WM_CAPTURECHANGED is a no-op. A pending plain click is left as before. |

The dead `kUiEditorDimToggle` `execute` case was removed in its own commit. Nothing paints or
sends that id; the enum value stays, because shipped ids are never renumbered.

## Not fixed

| Where | Status | Finding / scenario |
|---|---|---|
| `app_config.cpp` `read_ui_fields` | PLAUSIBLE (left as is, by request) | An unknown `last_page` falls back to Home, but an unknown `theme` (a newer build's value) rejects the whole app.json; inconsistent forward compatibility. |
| `app_config.cpp` invalid file | by design | With app.json failing to load, defaults include `onboarding_complete = false`, so the guide shows on every launch until the file is fixed (it never writes the file: bug 31 guard verified). |
| `app_config.cpp` legacy `motion_prediction` | REJECTED | `false` is ignored and 35 ms applies; documented as intentional (the old switch never reached the engine). |
| `app_window.cpp` page fade | REJECTED | A resize or DPI change during the 0.22 s fade makes `GdiAlphaBlend` read past the old snapshot and fail: the fade is skipped, nothing else. |
| `app_window.cpp` `on_destroy` | REJECTED | The 3 s un-pumped wait for a cancelled calibration is bounded and runs after the window is gone. |

## Verified unchanged / correct

- Quit and stop: 45 s stop grace (`engine_client.cpp`) and the 60 s message-pumped quit wait
  (`stop_engine_for_quit`) are unchanged; nested commands are ignored while quitting.
- Engine start is refused during an in-app calibration on every path (button, keyboard, tray
  all go through `execute`).
- Launch contract: `--predict-ms N` (0..100, matching the engine's clamp) and
  `--mag-track-hard-iron` only with head tracking; status keys `mag*`, `phase`,
  `phase_progress` match `engine_protocol.h` and the engine writer; NaN/negative progress is
  "not reported", >1 clamps; the 3 s freshness gate and the heartbeat's live `updated_unix`.
- Startup timer (id 3) is distinct from health (1) and animation (2), honours the 0.5 s status
  read cap, and is killed when hidden/minimised or on destroy; the animation timer still
  requires a visible, non-minimised window (bug 31).
- WM_CAPTURECHANGED / WM_MOUSELEAVE / TrackMouseEvent re-arming, WM_SETTINGCHANGE theme
  (falls through to DefWindowProc), modal guide (shell hotspots go to a sink; Esc, Ctrl+S,
  Ctrl+digit blocked).
- Session archiver: per-folder mutex over the absolute, case-folded path; `log_dir` is
  resolved against the root, so the helper and the controller agree.
- Removed-behaviour sweep: every UiId emitted on `main` is still emitted, except
  `kUiEditorDimToggle`, replaced by the 2D/3D segmented control (`kUiEditor2D/3D`); its dead
  `execute` case has been removed (see the follow-up section).
