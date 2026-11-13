# Round 2 review (R2-A): adversarial correctness review of the overnight merge

Scope: `git diff main..overnight-claude-improvements` (tracks A, B, C and the integration).
Method: every suspected bug was either proven (a failing test, a probe, or a concrete trace) or
listed below as not confirmed. Each fix has a regression test that fails with the old behaviour
restored and passes with the fix. MSVC Ninja RelWithDebInfo, `/W4 /permissive-`, no new
warnings; ctest 11/11 at every commit. No glasses, no engine, no live controller window.

## Confirmed and fixed

### 1. Concurrent test runs fail (shared temp names, machine-wide archiver lock) - `574d7ef`

- **Symptom.** Two build trees running ctest at once failed intermittently. Measured with N
  concurrent copies of each binary: `layout_selftest` 1/120, `orientation_calibration_selftest`
  8/80, `app_selftest` 50/60 runs failed.
- **Root cause.** `layout_selftest` and `orientation_calibration_selftest` wrote fixed `%TEMP%`
  names (plus the `.tmp` sibling of the atomic save); `app_selftest` wrote two fixed `%TEMP%`
  files. The main `app_selftest` cause was product code: `run_session_archiver` used one
  machine-wide mutex (`Local\RayNeoSpatialSessionArchiver`), so a helper draining *another* log
  folder made this folder's helper return 0 at once with its pending folder left behind
  ("no pending folder is left", "the archive is a real zip", ... failed).
- **Fix.** Per-process temp names (pid) in the three selftests; the `app_selftest` files moved
  into its own per-process directory. The archiver lock is now named per archive directory
  (`session_archiver_mutex_name`: FNV-1a of the absolute, normalised, case-folded path). One
  controller with one log folder behaves exactly as before.
- **Test.** `app_selftest`: one folder (two spellings) maps to one name, two folders to two
  names; a helper holding another folder's lock no longer blocks this folder's drain. Old fixed
  name restored: 2 failures; fix: 0. Concurrency harness after the fix: 0/120, 0/80, 0/60.

### 2. Turn prediction throws the view past a yaw/pitch tracking toggle - `efea972`

- **Symptom.** Switching yaw (or pitch) tracking back on (Ctrl+Alt+Y/P or the controller)
  replaces the held twist with the live one, so the smoother input steps by however far the
  wearer turned meanwhile in a single frame. The new prediction read that step as head velocity
  (30 deg in one 60 Hz frame = 1800 deg/s) and overshot the new pose for several frames.
- **Root cause.** `PoseSmoother::prediction_lead` differentiates consecutive inputs; only
  recenter reset the smoother, the toggles did not.
- **Fix.** `PoseSmoother::restart_prediction()` restarts the velocity estimate from the next
  input; the engine calls it on all four toggle paths. The 1-euro and reading hold are untouched,
  so the step still eases in exactly as it did before prediction existed.
- **Test.** `pose_scenarios` 24 (30 deg view step, medium hold, 60 Hz): overshoot 0.000 deg
  without prediction, **12.936 deg** with prediction and no restart (fails with a no-op
  restart), 0.000 deg with restart; final pose identical to the no-prediction run. Scenarios
  1-23 unchanged.

### 3. Animation timer keeps ticking while the window is in the tray - `694d9c7`

- **Symptom.** Closing the controller to the tray while the status card pulsed (engine starting
  or stopping, calibration running, heading-lock relearn) or during a page transition left the
  15 ms animation timer running (~66 wakeups/s) for as long as the window stayed hidden.
- **Root cause.** `Animator::wants_frames()` is cleared only by the next paint's
  `begin_frame()`, and a hidden window receives no `WM_PAINT`. Probe (a never-shown window,
  20 `InvalidateRect`s): **0** `WM_PAINT`. `update_animation_timer` ignored visibility.
- **Fix.** The decision is `anim::animation_timer_wanted(AnimationDemand)`, which also requires
  the window to be visible and not minimized. Showing the window paints, and that paint
  re-evaluates and restarts the timer if anything still moves.
- **Test.** `app_selftest` animation section, 5 checks; with the old formula 2 fail.

### 4. Setup guide overwrote an app.json that had failed to load - `c18595d`

- **Symptom.** With an invalid `config/app.json` (hand edit, a newer build's value such as an
  unknown theme), the controller runs on defaults (`config_valid = false`) and the banner says
  "Fix or delete the file". Defaults have `onboarding_complete = false`, so the guide opens;
  finishing or skipping it saved app.json unconditionally, silently replacing every preference
  in the user's file with defaults.
- **Root cause.** `finish_onboarding` was the one unguarded writer; its comment assumed the file
  "was replaced by defaults anyway" (only memory is).
- **Fix.** `onboarding_close(flow, config, config_file_valid)` returns whether app.json may be
  written; otherwise the guide closes for this session and the file is left for the user.
- **Test.** `app_selftest` onboarding section; the old always-save behaviour fails it.

## Confirmed, not fixed (no test seam without refactoring the window class)

These are real by Win32 semantics or a concrete trace, but the code lives in `AppWindow`
(not linked by any test) and the rules require a failing regression test per fix.

1. **Engine start is not blocked while an in-app calibration runs.** `start_calibration` refuses
   while the engine is busy, but `execute(kUiStartWorkspace/Preview)` (Home buttons, and the tray
   menu even while the modal guide is open) never checks `calibration_.active()`. Both processes
   then drive the same HID device; the tool's stream-off on exit would stop the engine's IMU
   stream. Suggested fix: refuse the start with a banner while `calibration_.active()` (which
   also covers the 4 s post-cancel shutdown). The same conflict existed before with the console
   calibration, so it is not a regression, but the controller now knows the tool is running.
2. **No `WM_CAPTURECHANGED` handling (pre-existing on main).** If capture is lost mid-drag
   (Alt+Tab, another window taking capture), `drag_active` stays true and the next plain mouse
   move keeps dragging the slider/screen/splitter until a button-up arrives in the window.
   Suggested fix: on `WM_CAPTURECHANGED` run the drag-end half of `on_lbutton_up` (no execute).
3. **Stuck pressed look (new tonight, cosmetic).** `press_id` is set on button-down without
   capture and cleared only by a button-up in the window; press, drag out and release outside
   leaves the button drawn pressed until the next click. Suggested fix: clear `press_id` on
   `WM_MOUSELEAVE` (the click itself is still decided by `pending_click_id_`).

## Reviewed, not confirmed (no change)

- **Startup bias confirmation** (`PoseEstimator`): every bias write is clamped; a reseed or
  revert clears the whole escape state (`reset_escape`), so the escape is never half-armed;
  windows cannot span a gap (window reset + new episode on a gap); one continuous slow turn
  stays a single episode (motion events only count after a rest dwell). No invariant violation
  found.
- **Prediction** other discontinuities: recenter already resets; `dt <= 0` or `> 0.1 s` re-seeds;
  stabilise-level changes keep `predict_s`; heading-lock corrections are rate-limited.
- **Calibration runner**: handles are released only after the reader thread is joined; the
  reader uses the process handle only before that; EOF reaches the reader because the parent's
  pipe ends are closed after `CreateProcess`; the destructor terminates then joins; `cancel()`
  writes 2 bytes (no blocking risk). Exit 0/1/2 mapping and the `cancelled` detail match the
  tool. The tool's stdin watcher is the only stdin reader in `--auto` mode.
- **`@@progress` parsing**: prefix look-alikes rejected, bounded strings, clamped ints, quality
  range-checked, retry reason kept until the step changes, countdown restarts per attempt.
- **GDI/GDI+ per paint**: the one GDI brush is deleted; every GDI+ object is RAII; fonts are
  destroyed before re-creation on DPI change; back and fade buffers restore their previous
  bitmap before deletion. No leak found.
- **Animator convergence**: every tracked value is in 0..~6 (indices, fractions), so the
  0.0015 epsilon always lands; no float-precision stall.
- **Mag hard-iron tracker**: disabled path is a pure pass-through (returns the stored offset,
  no state touched); it handles stale (repeated) mag samples and tick gaps itself. Internals not
  re-derived (opt-in, measured by Track B).
- **Status keys**: `mag` values written by the engine (`mag_state_name`) all parse;
  `heading_lock_from_status` is Unknown for unparsed, `imu=0`, missing or unknown values.
- **Modal guide**: shell hotspots go to a sink; Esc, Ctrl+S and Ctrl+1..6 are ignored while it is
  open. Arrow keys acting on a stale shell focus id (a nav item or segmented control under the
  guide) were considered: the guide opens at launch with no focus, or from the Run setup guide
  button, which then holds focus, so no path reaches it.
