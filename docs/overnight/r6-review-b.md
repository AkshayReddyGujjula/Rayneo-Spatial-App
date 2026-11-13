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
| `e234f26` | Calibration tool: cancel before save, UTF-8-safe progress, console close keeps stream-off |
| `5245ea2` | Controller: plain words for a calibration step's third failure |
| `b763b79` | Glasses guide: repaint on change, reuse the back buffer, follow display changes |

The last three are the follow-up round, on top of merge `4abbf99`: ctest 11/11, no warnings.
None of the commits changes the calibration maths, gates, result values, `@@progress` or `@@live`.

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

### 3. Console close does not run the stream-off guard — CONFIRMED by API semantics, fixed (`e234f26`)
`orientation_calibrate.cpp:73` `console_handler`. For `CTRL_CLOSE_EVENT` (and logoff/shutdown, which
it does not handle) Windows terminates the process as soon as the handler returns, so setting
`g_cancel` and returning TRUE does not let the main thread reach `StreamGuard`; the comment and
track-b.md ("console close ... the stream-off guard runs") overstate it. Impact is low: the stream
stays on until the next tool/engine start, which sends stream-off first.
Fix: sending stream-off from the handler thread is not safe, because the main thread may be inside
`hid_read` on the same handle and `send_command_verified` reads its own ack. So the handler sets
`g_cancel` and holds the close, up to 4 s, on an event that `wmain` sets after unwinding past
`StreamGuard`, the device close and `hid_exit`. A main thread blocked in the interactive ENTER
prompt may not return; the wait then times out as before. track-b.md now says so. No
hardware-free seam: it needs a console close and a device.

### 4. Cancel after the last capture check still saves — PLAUSIBLE, fixed (`e234f26`)
`orientation_calibrate.cpp:450` / `:889`. `g_cancel` is checked only inside the wait and capture
loops. A `q` that lands after the tilt (or compass) recording's last loop check is ignored: the
tool analyses, saves and prints `done`, while the controller (already Cancelled) shows
"Calibration cancelled. Nothing was changed." and `refresh_health` re-reads the new file. Window
is a few ms. Fix: `cancel_requested_before_save()` before the head flow writes anything and before
the compass save. No meaningful seam: the decision is one atomic load, and the race needs the HID loop.

### 5. Non-UTF-8 bytes can make the final `@@progress` line malformed — PLAUSIBLE, fixed (`e234f26`)
`orientation_calibrate.cpp:886`. `SAVE_FAILED: + std::error_code::message()` is the system's
localized ANSI text (e.g. French accents); `append_json_string` passes bytes through, nlohmann
rejects the line as invalid UTF-8, and the controller shows the generic "closed before it finished
(exit code 1)" instead of the reason. Needs a non-English Windows and an uncreatable config folder.
Fix: the message now comes from `FormatMessageW`, converted to UTF-8. `format_calibration_progress`
also replaces any ill-formed UTF-8 (RFC 3629) with U+FFFD, so no other source can break the line.
Tests: `orientation_calibration_selftest` pins the replacement and checks that the whole line is
well-formed (both FAIL without the sanitiser). `app_selftest` shows that the strict parser accepts
the sanitised final line and rejects the raw ANSI one.

### 6. Session end does not cancel the calibration — PLAUSIBLE (low), fixed by R6-C
`app_window.cpp` `on_end_session` stopped only the engine, and the tool's handler ignores
`CTRL_LOGOFF_EVENT`/`CTRL_SHUTDOWN_EVENT`, so the stream was left on (mitigated as in 3). R6-C's
`on_end_session` now cancels the calibration gracefully. The tool's handler is unchanged for those
two events.

### 7. Smaller items
- Fixed `e234f26` | `orientation_calibrate.cpp` | cosmetic | "%d of %d gestures used" used
  `options.repeats` as the denominator ("4 of 3"). It now reads "3 of 4 detected gestures used".
  No hardware-free seam (tool step).
- Fixed `5245ea2` | `calibration_progress.cpp` `cal_reason_text` | UX | the final per-step failure
  ("the yaw step did not pass after three attempts (OFF_AXIS: ...)") was shown raw. The wrapped
  code is now mapped or dropped behind a plain prefix ("The turn step did not pass after three
  attempts. Too much off-axis motion - ..."). The still and compass wrappers get the same
  treatment. `app_selftest` covers it and FAILs without the change.
- Fixed `b763b79` | `docs/orientation-calibration.md:381` and the `CalGuideView::visible` comment |
  doc | they no longer claim that the compass still step emits `@@live`.
- Fixed `b763b79` | `cal_guide_window.cpp` | efficiency | the frame timer repainted the whole
  monitor every 20 ms with a new monitor-sized bitmap per paint. Now `update()` repaints once per
  new sample or state, and the timer repaints only while the eased ball moves
  (`cal_guide_ease_ball`: the same exponential, snapping within half a pixel). The back buffer is
  reused until the size changes. `app_selftest` pins the easing. ui_snapshot's 62 PNGs are
  byte-identical before and after.
- Fixed `b763b79` | `cal_guide_window.cpp` | PLAUSIBLE (low) | monitor change mid-run: on
  `WM_DISPLAYCHANGE` the window re-queries the glasses monitor and moves to its new rect. If the
  monitor is gone, it closes, and the next `update()` probe reopens it. No hardware-free seam.
- Fixed `b763b79` / `5245ea2` | cleanups: `kCalHeadRepeats` replaces a literal 3, the
  identical-branch ternary is gone, and the duplicated condition in `CalRunState::apply` is folded.
- Not fixed | Retry clicked within about 1 s of Cancel is silently ignored while the old tool is
  still exiting.

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
