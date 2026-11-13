# Track B: calibration accuracy and magnetometer robustness

Overnight run, 2026-11-12 to 13. Branch `worktree-agent-adb4e6e0fd7294fe2`, three commits on top
of `5756604`. All builds MSVC Ninja RelWithDebInfo, warning-clean, 10/10 ctest at every commit.
No hardware was available: everything below is synthetic or replayed from
`dist/reference-logs-2026-09-25.zip` (2.4 h worn session recorded with a magnetometer
calibration, `mag_hard_iron_ut = (-16.81, -17.41, 3.00)`).

| Commit | What |
|---|---|
| `dcdb242` | `orientation_calibrate`: `--progress-json`, `--auto`, `--auto-delay`, `--earbuds`, Ctrl+C cancel; quality scores; hard-iron fit observability and weak axis; `mag_earbuds` / `mag_quality` file keys |
| `0510441` | Opt-in online hard-iron tracker (`MagHardIronTracker`) for head-worn magnets such as earbuds; `relearning` lock state; replay tooling |
| `4eea2af` | Engine wiring: `--mag-track-hard-iron`, status keys `mag`, `mag_error_deg`, `mag_offset_ut`; named states in the console line |

## 1. Progress protocol for the onboarding wizard (required by Track C)

`orientation_calibrate.exe --progress-json` prints, in addition to its normal output, one line
per state change on stdout, flushed immediately:

```
@@progress {"flow":"orientation","step":2,"steps":4,"state":"recording","prompt":"...","detail":"...","quality":-1}
```

- `flow`: `"orientation"` (4 steps: 1 still, 2 turn left, 3 nod down, 4 tilt right) or `"mag"`
  (with `--mag`; 2 steps: 1 still, 2 the 45 s turn-and-nod recording).
- `step`: 1-based, `steps`: total for the flow.
- `state`: `waiting` (instruction shown; waiting for ENTER or the `--auto` delay), `recording`,
  `ok` (step passed, `quality` set), `retry` (attempt failed, `detail` says why, the same step
  starts again with `waiting`), `failed`, `done`.
- The final line of every run is `done` (file written; `quality` = overall score) or `failed`
  (`detail` = reason; nothing was written). Failures before the first step (no device, stream
  could not start) are reported as step 1 `failed`.
- `prompt`: a short instruction for the wearer (may contain `\n`); during the mag recording a
  second `recording` line at 15 s switches the prompt from "turn all the way around" to "nod,
  tilt and look around". `detail`: measured values or a reason code (`HOLD_STILL`, `NO_MOTION`,
  `SMALL_EXCURSION`, `AMBIGUOUS_AXIS`, `OFF_AXIS`, `SMALL_COVERAGE`, `POOR_FIT`,
  `IMPLAUSIBLE_FIELD`, `GAP`, `NO_ORIENTATION`, `NO_DEVICE`, `OPEN_FAILED`, `STREAM_FAILED`,
  `SAVE_FAILED`, `cancelled`, ...) followed by a human sentence.
- `quality`: 0..1 guidance score, or `-1` when not applicable. Strings are JSON-escaped (quotes,
  backslashes, control characters as `\uXXXX`; UTF-8 passes through).
- Formatter: `gt::format_calibration_progress()` (`src/imu/orientation_calibration.*`); the exact
  shape is pinned by `orientation_calibration_selftest`.

No console needed: `--auto` starts each step after `--auto-delay S` seconds (default 4, 0..60) of
"waiting" instead of waiting for ENTER. Cancel by Ctrl+C / Ctrl+Break / closing the console (the
stream-off guard still runs, the last line is `failed` with detail `cancelled`; for a console close
the handler holds the close, up to 4 s, until that path has finished - R6-B). A GUI parent
without a console may also just terminate the process; the next tool or engine start sends
stream-off first anyway. Exit codes: 0 saved, 1 failed or cancelled, 2 bad arguments.

Wizard command lines:

```
orientation_calibrate.exe --progress-json --auto
orientation_calibrate.exe --mag --progress-json --auto [--earbuds]
```

## 2. Calibration quality and magnetometer UX

Quality scores (guidance only; acceptance gates are unchanged):

| Score | Definition | Where |
|---|---|---|
| still | `1 - rms / 1.5 deg/s` | still step `ok` |
| axis | `sqrt(purity * travel)`, purity `1 - 4/dominance`, travel `excursion / 25 deg` | each motion step `ok` |
| orientation | mean headroom of up_vs_yaw (8), nod_level (8), nod_tilt (20), handedness (15 deg) | `done` |
| mag | `(1 - residual / 1.5 uT) * min(1, observability / 0.15)` | mag `ok` / `done` |

`fit_mag_hard_iron` now reports **observability**: the smallest eigenvalue of
`I - Mbar^T Mbar` over the gyro-tracked rotations (0 = a hard-iron direction never rotated,
1 = uniform coverage) and the package-frame **weak axis**. Synthetic full turn plus nods: 0.169,
quality 0.92; a yaw-only recording: 0.000 with the turn axis reported. With quality below 0.6 the
tool names the missing motion in head terms (e.g. "nod up and down and tilt ear to shoulder
more"). The tool now tells the wearer to calibrate the way they work, earbuds in if they
normally wear them (their magnets are then absorbed into the offset), and `--earbuds` records
that in the file.

Calibration file (version stays 1; both keys optional, older files load unchanged):
`"mag_earbuds": true|false`, `"mag_quality": 0.000..1.000`. `MagCalibration::earbuds` /
`quality` (-1 = unknown).

## 3. Online hard-iron tracking (earbuds) - opt-in

### The problem, measured

A magnet that moves with the head is a step in the hard-iron offset. Dipole model (in
`mag_heading_selftest`): `|B| = 1e-7 m / r^3` T (x2 on axis); an earbud's driver plus
case-retention magnets, about 0.02-0.05 A m^2, 8-15 cm from a temple-mounted sensor, give
roughly 1-10 uT (the test's 0.04 A m^2 at 9.5 cm: 4.7 uT). That is within the lock's 8% |B| and
often within its 4 deg dip gate, so the lock pulls the view onto the shifted heading.

Replay measurement (`imu_replay --inject-hard-iron X,Y,Z --inject-from T`, which runs an
untouched baseline estimator alongside and reports the view-yaw departure): 30 injected steps
on the 2026-09-25 session (5 times x 6 directions, 3-5.4 uT, each evaluated for 1000 s):

| | rms mean | rms median | rms worst | p95 worst |
|---|---:|---:|---:|---:|
| lock alone (today) | 4.86 deg | 5.76 deg | 7.71 deg | 13.10 deg |
| lock + tracker | 1.91 deg | 0.93 deg | 6.29 deg | 9.31 deg |

Per direction (rms mean, off -> on): head-right 7.17 -> 2.15, left 7.52 -> 1.95, forward
2.28 -> 2.23, up 0.63 -> 0.95, mixed 4.50 -> 2.12, large mixed 7.08 -> 2.08. Five of 30 cases are
worse by more than 0.2 deg, all small-effect directions (worst: 1.98 -> 3.53 deg).

Synthetic worn head through the full `PoseEstimator` (`mag_heading_selftest`, centre-hold yaw
departure after 200 s):

| Event | tracking off | tracking on |
|---|---:|---:|
| none | 0.07 deg | 0.07 deg (no step, offset untouched) |
| earbud in | 0.54 deg | 0.05 deg, offset learned within 0.06 uT |
| earbud in, out 200 s later | 2.73 deg | 0.12 deg, offset back within 0.05 uT |
| magnet left on the desk (earth-fixed 3 uT) | 5.93 deg | 5.96 deg (not learned: reverted) |
| phone carried past at 25 cm | 0.07 deg | 0.07 deg (transient, not learned) |

### Design (`src/imu/mag_heading.*`)

1. **Step detection.** Two gyro-rotation-compensated estimates of the package-frame field
   (fast tau 0.15 s, slow tau 2 s). When they differ by 1.2 uT the slow one freezes and the lock
   holds (no correction: a suspect field cannot pull the view). Below 0.8 uT the watch ends as a
   transient; a difference that settles for 10 s and is at least 2 uT is a step; one that never
   settles within 30 s, is larger than 8 uT, or starts/ends outside the |B| reference band goes
   back to the lock's existing disturbance gates.
2. **Provisional adoption.** A confirmed step is folded into the offset at once, so the
   earth-frame field (and the heading the lock sees) is continuous: the lock's reference stays
   valid and is never re-taken.
3. **Verification.** Staggered 15 s windows of the gyro-constrained model
   `M_t (m_t - c) = b`, fitted along the step direction only (`c = c_before + a h`): `a ~ 1`
   head-fixed (kept, and the step size refined by the fit), `a < 0.35` earth-fixed (reverted).
   Decides after 3 uT^2 of rotation evidence; without enough rotation within 10 min the step is
   kept as measured. Total departure from the stored calibration is bounded (15 uT).

Rejected along the way (measured on the same session, documented in the code):

- A free per-window 3-D re-fit of the offset: in desk wear observability is only 0.02-0.2, the
  re-fit chased +/-2-3 uT of model noise, held the lock 17% of the session and moved the view
  3.1 deg rms with nothing changed.
- Re-referencing the lock at every step: lost the absolute anchor during real disturbances
  (6.7 deg rms with nothing injected).
- Re-referencing when a step is reverted as earth-fixed: fixes the desk-magnet case in
  simulation (5.9 -> 1.1 deg) but made the worst injected replay case 7.6 -> 11.0 deg.

### Why it is off by default

On the recorded session with nothing injected, the tracker matches the lock alone to
0.18 deg rms with arming at 2.0 uT, but with the final 1.2 uT watch threshold it reacts to one
real, ambiguous field event at t = 2356 s (a 3.6 uT step that its own fit called half
head-fixed), and over 150-5500 s the view departs from the lock-alone run by 3.1 deg rms. Nobody knows
what happened then (the wearer may well have touched an earbud), so neither run is ground truth,
and it has never run on live glasses. It is therefore behind `--mag-track-hard-iron` /
`MagHeadingLock::Config::hard_iron.enabled`.

### Not done / next

- Live verification: put an earbud in and take it out with the lock on, with and without
  `--mag-track-hard-iron`; record with `--log` and replay with `--tracker-log`.
- A stale offset present from the start of a session (calibrated with earbuds out, worn in) has
  no step to detect and is not corrected; the calibration UX (calibrate as you work, `--earbuds`)
  is the mitigation.
- Orientation-calibration accuracy (averaging repeated motions with outlier rejection) was not
  started: the usage budget went to the tracker measurements.
- The selftest's synthetic field has +66.7 deg dip; the recorded session reports -66 deg (the
  sensor's sign convention for the field; consistent everywhere, so heading is unaffected).

## 4. Settings, flags and status keys for the UI

| Kind | Name | Meaning |
|---|---|---|
| tool flag | `--progress-json` | machine-readable progress (section 1) |
| tool flag | `--auto`, `--auto-delay S` | no keyboard; each step starts after S s (default 4) |
| tool flag | `--earbuds` | with `--mag`: record that earbuds were worn |
| engine flag | `--mag-track-hard-iron` | experimental online hard-iron tracking (default off) |
| status key | `mag` | `off` / `acquiring` / `locked` / `disturbed` / `relearning` |
| status key | `mag_error_deg` | current heading disagreement of the lock (deg) |
| status key | `mag_offset_ut` | how far tracking moved the hard iron from the file (uT) |
| file key | `mag_earbuds`, `mag_quality` | calibration metadata (optional) |

Suggested UI wording: locked "Heading locked to the magnetic field"; disturbed "Magnetic
interference - holding heading"; relearning "Checking a magnetic change (earbuds?)"; acquiring
"Finding magnetic north"; off "Heading uses the gyro only (calibrate the magnetometer to stop
drift)". The status keys are written by `spatial_desk` but `parse_engine_status` (Track C) does
not read them yet; it ignores unknown keys, so nothing breaks.

## 5. Tooling for the next person

`imu_replay` additions: `--hard-iron-track`, `--inject-hard-iron x,y,z`, `--inject-from S`,
`--inject-until S`, `--baseline-no-track`, `--eval-until S`, `--tracker-log` (one line per
tracker event), and a per-print `vs-baseline` line with the departure, both lock states and the
detector signal. Note for concurrent agents: `layout_selftest` and `app_selftest` share fixed
temp-file names and failed once while another track ran the suite at the same time; a rerun was
green.
