# R4-A: startup phases and the hold-still prompt

Why: the largest drift in the recorded 2026-09-25 session (AGENTS.md bug 26) came from the
~1.26 s startup gyro-bias window catching a slow head roll. The wearer had no way to know the
window was being measured. Startup confirmation (bugs 26/28/32) repairs a bad startup bias after
the fact; a prompt at the right moment avoids it. Startup is also long (display takeover 13-35 s,
IMU warm-up 4 s, then the window) with only a pulsing "Starting" for feedback.

No glasses were available. Everything below is synthetic (`pose_selftest`), a parser/model test
(`app_selftest`) or an offscreen render (`ui_snapshot`). The engine and controller were never run live.

## 1. Status key contract (new, optional)

Two keys appended to `logs/engine-status.txt`; no existing key was renamed or renumbered.

| Key | Values |
|---|---|
| `phase` | `preparing` (config/layout/calibration load) · `takeover` (virtual displays + arrangement) · `waiting_display` (the glasses display dropped out after the takeover; up to 15 s) · `connecting` (waiting for the glasses' IMU stream, also after a USB reconnect or stream timeout) · `imu_warmup` (4 s, samples discarded) · `hold_still` (searching for / measuring one contiguous still window) · `tracking` · `running` (`--no-imu`) · `restoring` (shutdown restore started) |
| `phase_progress` | `0.00`..`1.00`. `imu_warmup`: share of the warm-up done (time and settle-sample floors). `hold_still`: fill of the open contiguous window (time and 600-sample floors); `0` while waiting for rest; back to `0` whenever motion or a >= 50 ms sample gap discards the window. `1.00` once tracking. Not meaningful in the other phases (readers ignore it there). |

Write cadence: once per second as before, plus on every phase change, plus every 0.25 s during
`imu_warmup` and `hold_still`. A **status heartbeat thread** repeats the last published report
once per second while the main thread is blocked in the startup sequence (takeover, display wait),
so the controller's 3 s freshness gate keeps the phase; it stops before the render loop. It only
rewrites a copy (under a mutex shared with `publish_status`), never touches `g_status`, and is
joined by its destructor on every early return. Same atomic temp-file + `MoveFileExW` write.

Readers: unknown `phase` values must be treated as "no phase" (newer engines may add values). The
controller parses `phase_progress` with the existing finite-number parser, clamps > 1 to 1 and
treats a negative or non-numeric value as not reported.

Compatibility: an older controller ignores both keys (it tolerates unknown keys). This controller
with an older engine sees no `phase` and keeps exactly the old Starting / Running view.

## 2. Engine side

* `PoseEstimator::startup_phase()` / `startup_progress()` (`src/imu/pose_estimator.h`): read-only,
  derived from `calibrated_`, the warm-up floors and the calibration window accumulators. Nothing in
  the estimator reads them. `pose_scenarios` output is byte-identical to the base commit
  (`cmp` of the full output); `pose_selftest` output is identical up to the appended case.
* `ImuSource::start_phase()` / `start_progress()` (`ImuStartPhase`): `connecting` from each
  (re)connection attempt until that connection's first decoded sample, then the estimator's phase.
  A reconnect therefore shows warm-up / hold still again, which is true: tracking is closed then.
* `spatial_desk.cpp`: phase set at the takeover, the display wait, window ready (`connecting` or
  `running`), from the IMU in the render loop, and `restoring` at shutdown when a takeover is armed.
  The console prints `[t] startup phase: <name>` on every change (useful in `engine.log`).
* Not changed: takeover, VDD, capture, rendering, estimator maths.

## 3. Glasses view

Not done. The renderer has no text or overlay path: the only GDI-to-texture code is the immutable
per-screen label texture for `--no-virtual-displays`. Building a head-locked text quad was out of
scope for tonight. A cheap follow-up that needs no text renderer: use the existing per-screen colour
multiplier (`Renderer::set_screen_color`) to dim the screens until tracking opens and fade them up
when it does, as a "you can move now" cue. That changes what the wearer sees, so it needs a live trial.

## 4. Controller UI

* `EngineStatusFile::phase` / `phase_progress`; `EnginePhase` + `parse_engine_phase` and
  `startup_view(phase, progress, head_tracking)` in `ui_model` (pure, tested);
  `engine_startup_view(AppState)` gates on the process state being Starting or Running.
* Home hero during startup: headline/detail per phase, accent ring (pulsing, but still during hold
  still), four steps (Screens, Glasses sensor, Warm-up, Hold still: done = green check, active =
  accent dot, pending = outline, hairlines between), and a progress bar for the warm-up and the
  hold-still window. The metric tiles are hidden while the steps show.
* Last 40% of the warm-up (`kStartupGetReadyAt` 0.6, about 1.6 s): **Get ready to hold still**.
  `hold_still`: **Hold your head still** in the display font on an `accent_soft` panel (text in
  `text`, a checked contrast pair), "Keep looking straight ahead without moving..." until the window
  opens, then "Measuring the sensor while you keep still. Any movement starts it again."
* Before Start (when either start is allowed): "After you start, look straight ahead and keep your
  head still for a few seconds when asked."
* Without head tracking (`imu=0`) only the screen-setup phases show, with no steps row.
* Polling: `kTimerStartup` (500 ms) runs only while warm-up or hold still is reported and the window
  is visible; it re-reads only the status file and skips when the last read is < 0.49 s old, so the
  2 Hz read cap holds. `refresh_engine_status()` was factored out of `poll_engine()` unchanged.

## 5. Tests

* `pose_selftest` "startup phase accessors": production startup constants, still head with a
  0.4 deg/s bias. Fresh = warm-up at 0; 2 s = 0.500; a 60 ms gap during warm-up leaves 0.500;
  hold still starts at exactly 4.00 s; window fill 0.718 then 0 after a 0.2 s 40 deg/s movement;
  refills to 0.293 then 0 after a 60 ms gap; tracking 2.26 s later with last hold-still progress
  0.998; the phase never steps back; tracking reported exactly when `calibrated()`; confirmation
  windows after tracking never reopen hold still. (The warm-up gap arms the 1 s motion hold-off,
  bug 12 behaviour, which is why the first window opens ~1 s into hold still.)
* `app_selftest` "startup phase status keys and Home steps": parsing, an older engine (no keys)
  keeps the generic view, unknown phase and `nan` progress ignored, > 1 clamped, < 0 = not
  reported, all nine names round-trip, phase -> step mapping, get-ready threshold, hold-still
  prompt and detail switch, no-head-tracking behaviour, pre-start hint.
* `ui_snapshot`: six new Home renders (takeover dark, warm-up light, get-ready dark, hold still at
  150% dark, hold still at 0 light, hold still narrow); all hotspot and WCAG checks pass.
* Without the change these tests do not compile (new accessors, keys and model); their assertions
  pin the semantics (reset to 0 on movement / gap, warm-up gap not counted, unknown -> generic).

`ctest`: 11/11 passed, MSVC Ninja RelWithDebInfo, no new warnings.

## 6. Needs live verification

1. The hero follows a real start: takeover step stays fresh through a 13-35 s takeover (heartbeat),
   then connecting -> warm-up -> hold still -> tracking. Watch `engine.log` for
   `startup phase:` lines and their timing against what Home showed.
2. Whether the prompt actually reduces bad startup windows: compare `startup confirmation:
   reseeds=` counts and first-minute drift over several starts with and without following it.
3. The hold-still bar resetting visibly when the wearer moves (2 Hz reads of a 4 Hz file: expect
   up to ~0.75 s latency).
4. Reconnect mid-session (unplug the glasses' USB): Home should show Glasses sensor / Warm-up /
   Hold still again, then return to Running.
5. The 30 s controller start timeout ("did not create its window within 30 s") still applies
   during a long takeover; the phase display does not change it.
