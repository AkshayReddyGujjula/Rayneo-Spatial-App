# Track A: head-tracking accuracy, drift and stabilisation (overnight run)

Everything here was measured offline: synthetic scenarios through the real estimator, and
`imu_replay` on the recorded worn session `dist/reference-logs-2026-09-25.zip` (8812 s, 4.19 M
samples, temperature 28.0 -> 44.6 C, with that session's own `config/orientation.json`, which
carries a magnetometer calibration). The glasses were not available, so nothing below has been
verified on a head.

## Commits

| Commit | Subject |
|---|---|
| `06b9eaf` | Confirm the startup gyro bias against later rest episodes |
| `7e26bdb` | imu_replay: drift trace and startup audit |
| `c8026f2` | Predict the shown pose ahead during head turns |
| (this file) | docs: Track A overnight report |

The tree built warning-free (MSVC, `/W4 /permissive-`, RelWithDebInfo) and passed all ten ctest
suites at each commit (`06b9eaf` was tested with the then-uncommitted replay tool in the tree; it
only adds callers of existing APIs). Scenarios 1-19 and `pose_selftest` print output identical to the starting commit
(`5756604`) to the last digit; only the new scenarios 20-23 were added.

## Finding 1: the "thermal drift" was mostly a bad startup bias

AGENTS.md section 7 attributes ~20 deg of drift in the 2026-09-25 session to thermal gyro drift.
The data do not support that:

- **Bias versus temperature is weak.** Over 4,263 quiet 1-s windows the gyro rest rate changes by
  about 0.005 deg/s per deg C per axis (head-yaw axis +0.0064 deg/s/C), i.e. about 0.08 deg/s
  over the whole 16 C warm-up, far inside routine authority (0.35 deg/s). The residual scatter of
  window means (0.16-0.20 deg/s) is worn-head micro-motion, not temperature. Five-minute block
  means of the yaw-axis rate stay within 0.49-0.65 deg/s with no clean temperature trend. The
  runtime bias estimate itself stays within y 0.38-0.48, z 0.33-0.45 deg/s all session.
- **The big drift was at startup.** With the heading lock in observe mode, the session's own
  start calibrated the bias at (0.00, 0.35, **-1.08**) deg/s against a true (0.0, 0.42, 0.38): the
  1.26 s startup window (5.2-6.5 s) contained a smooth ~3 deg/s head roll (raw z -2.5 to -3.6
  deg/s, gravity turned 2 deg) that passes every fluctuation gate. That is a ~0.6 deg/s yaw-axis
  error, outside routine authority, so only the slow escape could fix it (bias z reached +0.30
  after ~250 s). Gyro-only heading, measured against the magnetometer in the estimate's own frame
  (`imu_replay --trace`), drifted **+35 deg in the first 300 s**. With the lock on, the wearer never
  saw it; for a user without a magnetometer calibration this is the most visible defect in the log.

**Temperature-compensated bias was therefore not built.** A feed-forward model would act only
between rest windows, where the measured temperature change is at most ~2.4 C/min (first minute),
i.e. ~0.015 deg/s per minute of bias change, which the routine update already tracks. It cannot
be shown to win, so it was not shipped (lesson 4).

## Change 1: startup confirmation (on by default)

`PoseEstimator::Config::startup_confirm_s = 60`, `startup_confirm_agree_degs = 0.3`.

Tracking still opens on the first 1.26 s window (startup time unchanged). For 60 s afterwards the
same calibration-grade windows keep being collected (startup gates plus the runtime raw-rate rest
ceiling) and averaged per rest episode; a detected motion (or an IMU gap) starts a new episode.

- The first two episodes whose means agree within 0.3 deg/s form a consensus. Inside routine
  authority (<= 0.35 deg/s from the current bias) it just ends confirmation.
- Outside it, the consensus replaces the bias in one step (only the rate changes; the pose is
  continuous) and clears the escape state. The replaced bias is snapshotted.
- A later episode that agrees with the consensus ends confirmation; one that agrees with the
  snapshot instead reverts the bias to it in one step (the snapshot/return pattern of invariant 6
  and lesson 7) and ends confirmation.
- One continuous slow turn is a single episode, so it can never confirm itself.

Invariants: the bias stays the single owner of steady error (4), every write is clamped (5), and
the escape is never left half-armed across a reseed or revert (6).

### Measured (replay, fresh estimator every 10 s of the recording, gyro-only, observe-mode lock)

`imu_replay FILE --orientation ORIENT --startup-audit 10 --audit-horizon H [--confirm 0]`

| Gyro-only heading drift vs magnetometer | Before (`--confirm 0`) | After | Change |
|---|---:|---:|---:|
| First 60 s, mean (870 starts, 821 with clean mag) | 9.21 deg | 7.13 deg | -23% |
| First 60 s, p50 / p90 / p99 | 4.33 / 24.4 / 57.7 | 3.82 / 19.3 / 40.9 | |
| First 60 s, starts above 10 deg | 228 | 186 | |
| First 180 s, mean / p90 / p99 (796 starts) | 15.7 / 42.6 / 98.9 | 11.0 / 24.3 / 58.2 | -30% / -43% |
| Yaw-axis bias error above 0.35 deg/s, 60 s after open | 65 starts | 12 starts | |
| Paired per start (60 s): better / worse by > 3 deg | | 134 / 11 | |
| Paired per start (180 s): better / worse by > 3 deg | | 156 / 24 | |
| Time to tracking p50 / p90 | 5.76 / 10.40 s | 5.76 / 10.40 s | unchanged |

For reference, the long-running estimator's own drift over the same 60 s intervals (distortion
plus genuine drift, the floor this metric can reach) is mean 2.15, p50 1.37 deg.

Reseeds happen at p50 27 s after tracking opens: the startup error accrues until two separate
rest episodes exist. The session's own start is **not** fixed by this: after the bad window the
wearer moved, then rested continuously from 30 s to 66+ s (one episode, 28 windows averaging the
true bias), and a single episode cannot outvote the startup by design.

### Tests

- `pose_scenarios` 20: a 0.8 deg/s turn spans the startup window, then 55 s of work (3 s at the
  screen, glance, repeat). Drift 46.5 -> 9.7 deg, final yaw bias error 0.77 -> 0.006 deg/s, one
  reseed. Asserts the unconfirmed run exceeds 10 deg (the defect) and that confirmation removes at
  least 70% of it.
- `pose_scenarios` 21: scenario 11's 20 s / 1.0 deg/s turn inside the confirmation period after a
  glance: 17.83 deg registered, 0.13 deg reverse, no reseed, bit-identical to the unconfirmed
  estimator.
- `pose_scenarios` 22: two separate 4 s turns at 0.6 deg/s outvote a clean startup (1 reseed),
  the next rest episode reverts it (1 revert); 2.6 deg of drift remains (asserted < 3.5) versus
  0.27 deg unconfirmed. This is the physical yaw ambiguity, now bounded instead of left to the
  escape.

### Rejected along the way (measured, not shipped)

| Idea | Result |
|---|---|
| Segment-mean consistency inside the startup window (0.25 s segments) | Worn-head micro-motion makes 0.25 s segment means differ by ~1.2 deg/s at p50 even at rest; any useful threshold rejected 30-90% of starts within 60 s |
| Gravity-direction stability inside the window | 0.5 deg tolerance: drift mean 9.2 -> 7.5 but tracking p90 10.4 -> 15.1 s (good windows already move gravity 0.27 deg p50, 0.70 p90); combined with confirmation only 6.85 -> 6.23 |
| Longer startup window 2 / 3 / 4 s | Drift mean 7.4 / 5.7 / 5.0 deg but tracking p50 +0.75 / +1.75 / +2.75 s, p90 +5 / +10 / +14 s. Available as `--cal-window` in imu_replay for future decisions; not changed |
| Extra settle time before the window | 0.5-2.0 s settle barely helps (8.0-9.1 deg) at +1-2.5 s startup |
| Per-window (not per-episode) confirmation | 8 window slots filled inside one long rest episode and never decided |
| Agreement 0.2 / 0.4, separate minimum reseed size 0.5 | All within +/-0.4 deg of the shipped setting; 0.3 with the routine cap as the threshold has the best better/worse ratio |

## Change 2: motion-to-photon prediction (on by default, 35 ms)

`PoseSmoother::Config::predict_s = 0.035` (with `predict_cutoff_hz = 8`, gate 10-30 deg/s).

The shown pose is extrapolated along the raw frame-to-frame angular velocity (low-passed at 8 Hz),
faded in by a smoothstep between 10 and 30 deg/s of head speed, and applied to the output after
the 1-euro stage and the reading hold, so neither filter's own behaviour changes and the lead is
exactly the identity while reading or still. It needs `opt.smoothing` (inactive with
`--no-smoothing`).

### Measured (`imu_replay FILE --orientation ORIENT --no-mag --smooth-eval --stabilise N --latency L --predict S`)

"Photon error" is each shown 60 Hz frame against the raw pose L later, rms, frames classed by the
0.25 s head-speed EMA (reading < 3 deg/s, turning > 20 deg/s). 1949 s of reading, 291 s of turning.

| Medium stabilisation | L = 15 ms | L = 25 ms | L = 40 ms |
|---|---:|---:|---:|
| Turning rms, no prediction | 2.44 deg | 2.92 deg | 3.72 deg |
| Turning rms, 35 ms | 0.86 deg | 1.19 deg | 1.91 deg |
| Turning max, no prediction -> 35 ms | 8.96 -> 3.09 | 11.41 -> 4.38 | 15.42 -> 8.39 |
| Transition (3-20 deg/s) rms | 0.570 -> 0.531 | 0.605 -> 0.561 | 0.670 -> 0.619 |

At L = 25 ms by level (off/low/medium/high/ultra): turning rms 2.57 -> 0.84, 2.76 -> 1.03,
2.92 -> 1.19, 3.04 -> 1.32, 3.85 -> 2.17 deg (44-67% lower). **Note:** the `c8026f2` commit
message says "59-66% at every stabilisation level"; that figure came from an earlier pre-filter
variant. The shipped output-side lead measures 44% (ultra) to 67% (off), as listed here.

Reading is unchanged to the printed digit at every level (e.g. medium: 6.6 px/s text motion,
p95 step 0.0119 deg, world offset 0.220 deg, photon error while reading 0.2684 deg).

35 ms was chosen because it improves the turning error for every assumed latency from 15 to
40 ms; 50 ms is better at 25-40 ms but worse at 15 ms (max error 6.99 vs 3.09 deg).

### Test

`pose_scenarios` 23: 6 s of reading sway, a 0.5 s 40 deg turn, 2 s still. Turning error against
the pose 25 ms later 4.863 -> 1.855 deg rms; reading sway bit-identical (difference 0); overshoot
past the stop 0.021 deg (bound 0.25).

## Tooling added to `imu_replay`

- `--trace OUT.csv [--trace-hz HZ]`: decimated rows with bias, gates, pose, signed yaw travel, and
  the calibrated magnetometer heading in the estimate's earth frame (its change is the gyro-only
  heading drift, continuous across lock reacquisitions, unlike the lock's `err`).
- `--startup-audit STEP [--audit-horizon S] [--audit-out OUT.csv]`: fresh estimator every STEP s,
  time-to-tracking and first-horizon drift statistics. Needs a mag calibration in the orientation
  file.
- `--confirm S`, `--confirm-agree D`, `--cal-window S`: startup confirmation and window overrides.
- `--smooth-eval` additions: `--latency L` (photon error line), `--predict S`,
  `--predict-gate lo,hi,cutoff_hz`.
- `PoseEstimator::fused_orientation()` (diagnostic accessor) supports the trace and audit.

## Needs live-glasses verification

1. **Startup confirmation on real launches.** The audit starts the estimator at arbitrary points of
   a working session; a real launch is preceded by the "hold still" prompt, so real startup windows
   are probably cleaner and reseeds rarer. Check the engine log for the new line
   `startup confirmation: reseeds=N reverts=M bias=(...)` over several launches, and whether the
   view stops drifting after a reseed.
2. **Prediction horizon.** The real motion-to-photon latency of the GT path (late pose sample ->
   render -> flip -> panel) has never been measured. The 35 ms default is robust for 15-40 ms in
   replay, but the perceptual check is: turn the head quickly and stop; the screens should neither
   lag nor bounce. If they bounce, lower `--predict-ms`; if they still trail, raise it.
3. Prediction only affects turns above 10 deg/s; confirm reading text is as steady as before on
   each stabilisation level (replay says it is identical).

## Settings / flags for the UI (Track C)

| Setting | Engine flag | Default | Notes |
|---|---|---|---|
| Turn prediction (ms) | `--predict-ms N` | 35 (0 = off, clamped 0..100) | Suggest a slider 0-60 ms or Off / 20 / 35 / 50. Help text: "Shows the world slightly ahead during fast head turns so it keeps up with your head. Increase if screens trail a quick turn; decrease if they overshoot when you stop. Has no effect while reading." |
| Startup confirmation | none | on (60 s, 0.3 deg/s) | Not user-facing; diagnostic only. Could surface `reseeds`/`reverts` in a debug panel. |

## Not done / next

- Seeding the startup bias from the previous session (goal 3): the per-episode consensus already
  has the shape for it (a persisted bias + temperature could act as one extra voter, never as data
  that opens tracking alone, per lesson 9), but there is only one recording, so cross-session bias
  repeatability cannot be measured yet. Record two or more sessions with `--log` on different days
  first.
- The single-long-episode case (the recorded session's own start) is still left to the escape.
  Letting a long episode outvote a short startup episode would fix it but also lets a long steady
  slow turn after a glance be learned; it needs a guard that can be proven on recordings before it
  is worth trying.
- AGENTS.md section 7's "~20 deg of thermal gyro drift" sentence should be corrected at
  integration (Track A may not edit AGENTS.md): the replay shows a startup-window error, not
  thermal drift, as the dominant term.
