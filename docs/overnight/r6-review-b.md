# R6-B review: calibration tool, maths, live guide, controller plumbing

Reviewer R6-B, 2026-11-13. Scope: `git diff main...overnight-claude-improvements` restricted to
`src/imu/orientation_calibration.*`, `src/imu/calibration_live.*`, `src/tools/orientation_calibrate.cpp`,
`src/app_shell/calibration_progress.*`, `calibration_runner.*`, `calibration_guide.*`, `ui_cal_guide.*`,
`cal_guide_window.*` and their tests. No hardware was used: the tool was run only with an unknown
option (rejected in `parse_args`, exit 2, before hidapi). Build: MSVC Ninja RelWithDebInfo outside the
Desktop tree, `/W4 /permissive-`, no warnings; ctest 11/11 green before and after.

## Commits

| Commit | Subject |
|---|---|
| `64782fe` | Keep a running calibration when the setup guide is reopened |
| `5ef5f06` | orientation_calibrate: decode the command line as UTF-8 (wmain) |

Neither changes the calibration maths, gates, result values, `@@progress` or `@@live`.

## Findings (most severe first)

### 1. Non-ASCII `--output` decoded with the ANSI code page — CONFIRMED, fixed (`5ef5f06`)
`src/tools/orientation_calibrate.cpp` `main(int, char**)`. The controller passes the calibration path
as UTF-8 through a wide command line (`calibration_tool_arguments`, `CreateProcessW`). The tool's
narrow argv is the CRT's ANSI decoding: a character outside the code page becomes `?` (invalid path:
head flow ends `SAVE_FAILED`, compass flow `NO_ORIENTATION`) or a best-fit letter (U+0142 -> `l`: the
calibration is written to a *different* directory, the controller reports "saved", and the engine
keeps reading the old file). `default_output_path()` used `path::string()`, which throws on such
characters. `spatial_desk` already uses `wmain` for exactly this reason (`util/utf8_path.h`).
Fix: `wmain` narrows each argument to UTF-8 once; default path via `utf8_from_path`; the two narrow
path constructions left (`create_directories`, `--log`) go through `path_from_utf8`.
Test: `orientation_calibration_selftest` runs the built tool with `--r6b-probe-<U+65E5><U+0142>`;
with the ANSI decoding it echoes `--r6b-probe-?l` (FAIL), after the fix the exact UTF-8 (PASS).
Side effect: console echoes of a non-ASCII path now show UTF-8 bytes (cosmetic).

### 2. "Run setup guide" orphans a running calibration — CONFIRMED, fixed (`64782fe`)
`src/app_shell/app_window.cpp:1492` (owned by another reviewer; one-line change, flagged here).
`kUiRunSetupGuide` reset `state_.cal_run` unconditionally; the Settings button is enabled while an
in-app calibration runs (no navigation lock). Trace: Tracking -> Calibrate now -> Settings -> Run
setup guide: `cal_run` becomes Idle while `calibration_.active()` stays true, so the glasses guide
closes mid-run (`cal_guide_view().visible` false), Cancel disappears, the tool's `done` and exit are
ignored (`apply`/`process_exited` need Running), the wizard's head step is never marked, and
"Start calibration" is silently refused until the hidden tool exits. The other reset sites
(wizard buttons, Dismiss, finish_onboarding) were already guarded.
Fix: `CalRunState::clear_unless_running()`; the call site uses it.
Test: `app_selftest` — a running run survives the clear and still reaches Succeeded; with the
old unconditional reset it FAILs.

### 3. Console close does not run the stream-off guard — CONFIRMED by API semantics, not fixed
`orientation_calibrate.cpp:73` `console_handler`. For `CTRL_CLOSE_EVENT` (and logoff/shutdown, which
it does not handle) Windows terminates the process as soon as the handler returns, so setting
`g_cancel` and returning TRUE does not let the main thread reach `StreamGuard`; the comment and
track-b.md ("console close ... the stream-off guard runs") overstate it. Impact is low: the stream
stays on until the next tool/engine start, which sends stream-off first. A fix would have the
handler wait (< 5 s) on an event set after `StreamGuard` ran; not done because no hardware-free
regression test is possible.

### 4. Cancel after the last capture check still saves — PLAUSIBLE, not fixed
`orientation_calibrate.cpp:450` / `:889`. `g_cancel` is checked only inside the wait and capture
loops. A `q` that lands after the tilt (or compass) recording's last loop check is ignored: the
tool analyses, saves and prints `done`, while the controller (already Cancelled) shows
"Calibration cancelled. Nothing was changed." and `refresh_health` re-reads the new file. Window
is a few ms. Trivial guard (`if (g_cancel) fail "cancelled"` before each save), untestable without
a device.

### 5. Non-UTF-8 bytes can make the final `@@progress` line malformed — PLAUSIBLE, not fixed
`orientation_calibrate.cpp:886`. `SAVE_FAILED: + std::error_code::message()` is the system's
localized ANSI text (e.g. French accents); `append_json_string` passes bytes through, nlohmann
rejects the line as invalid UTF-8, and the controller shows the generic "closed before it finished
(exit code 1)" instead of the reason. Needs a non-English Windows and an uncreatable config folder.

### 6. Session end does not cancel the calibration — PLAUSIBLE (low), not fixed
`app_window.cpp:463` `on_end_session` stops only the engine; the tool's handler ignores
`CTRL_LOGOFF_EVENT`/`CTRL_SHUTDOWN_EVENT`. Stream left on; mitigated as in 3.

### 7. Smaller items (not fixed)
- `orientation_calibrate.cpp:570` | cosmetic | "%d of %d gestures used" uses `options.repeats` as the
  denominator; a wearer doing four gestures reads "4 of 3".
- `calibration_progress.cpp` `cal_reason_text` | UX | the final per-step failure ("the yaw step did
  not pass after three attempts (OFF_AXIS: ...)") is shown raw: the code is mid-sentence, so it is
  neither mapped nor stripped, and says "yaw" to the wearer.
- `docs/orientation-calibration.md:381` | doc | says the compass flow's still step emits `@@live`; the
  mag `Progress` has no `LiveReporter`, so it never does (the controller would not draw it anyway).
- `cal_guide_window.cpp:15,160` | efficiency | 50 Hz full-monitor repaint with a fresh
  monitor-sized `CreateCompatibleBitmap` per frame, even when nothing moves (countdown, stale live).
  A cached back buffer and repainting only while the ball eases would cut it to near zero.
- `cal_guide_window.cpp` `update` | PLAUSIBLE (low) | the window keeps its creation rect if the
  glasses monitor moves or changes mode mid-run.
- `calibration_guide.cpp:188` | cleanup | fallback `repeats_required = 3` duplicates `kCalHeadRepeats`.
- `ui_cal_guide.cpp:22` | cleanup | `style.glasses ? 0.30f : 0.30f`.
- `calibration_progress.cpp:452,455` | cleanup | the same condition twice in a row.
- Retry clicked within ~1 s of Cancel is silently ignored while the old tool is still exiting.

## Checked and rejected

- **stdin watcher**: detached by design; ReadFile holds no CRT lock, ExitProcess ends it. `q`, EOF and
  an empty line (CR/LF trimmed) map correctly; `g_start_now` is reset at each wait, so a late
  Start-now cannot skip the next step's countdown; the mag ticker thread is joined on every path.
- **Countdown IMU reads**: read errors sleep and fall through to `capture_phase`, which reports the
  disconnect; cancel and Start now are polled each 20 ms read. Draining during the countdown removes
  the old up-to-134 ms of stale buffered samples at the start of each recording (an improvement).
- **Runner**: the reader drains stdout independently of the UI (no back-pressure deadlock); child
  ends closed after `CreateProcessW`, controller ends non-inheritable; `take()` joins only after
  the exit is published; CRLF, partial lines, 64 KiB lines, malformed JSON, out-of-range numbers and
  non-finite values are handled by the parsers. A 0-byte successful read would be taken as EOF, but
  the tool never writes 0 bytes.
- **Exit codes**: 0 done, 1 failed/cancelled (`cancelled` detail -> Cancelled), 2 -> "outdated tool".
- **Guide signs**: tool (yaw + left, pitch + up, roll + right; along = left/down/right) and drawing
  (turn ball left, nod ball down, tilt ball clockwise) agree in the gravity, file, nominal and
  measured frames; `live_forward_from_sensor_to_head` recovers forward_body exactly; the measured
  nod axis is +right after `command_sign`; the ball is clamped to the guide region.
- **Repeats maths**: splitter, medoid rejection (never the majority, medoid kept), chordal mean,
  zero-sum case (fails a later gate, no NaN), consistency cap, N = 1 path unchanged.
- **File back-compat**: old files load; orientation and mag saves keep each other's keys;
  `mag_quality` cannot be read as `orientation_quality`; matrix precision unaffected by the
  later `setprecision(3)`.
- **Guide window**: non-activating (`WS_EX_NOACTIVATE`, `SW_SHOWNOACTIVATE`, `MA_NOACTIVATE`),
  click-through (layered + transparent + `HTTRANSPARENT`), timer killed before destroy, fonts and
  per-frame GDI objects freed, closed on done/failed/cancel/app exit; the engine cannot start
  while a calibration is active.
