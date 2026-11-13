# Round 2 (R2-C): a single long rest episode as startup confirmation

Everything here was measured offline: synthetic scenarios through the real estimator, and
`imu_replay` on the recorded worn session `dist/reference-logs-2026-09-25.zip` (8812 s, 4.19 M
samples), with that session's own `config/orientation.json` from the zip (the same file Track A
used; it carries a magnetometer calibration, which only the audit's drift metric uses). The
glasses were not available, so nothing below has been checked on a head.

## The gap

Track A's startup confirmation decides only when two separate rest episodes agree, so that one
continuous slow turn can never confirm itself (scenarios 11/21). The recorded session's own start
falls through that design. The startup window held a smooth head roll. The wearer glanced away and
then rested for 36+ s. That rest is one episode, so nothing decided, and the slow escape owned a
1.4 deg/s error.

## What tells a long rest from a long slow turn

A gyro alone cannot tell them apart: a steady rate `d` during an episode is the same measurement
whether it is a turn or a stale bias (AGENTS.md section 5). Episode length, how consistent the
windows are with each other, and how well they agree with the routine estimate all fail for the
same reason, because a steady turn passes all of them.

The accelerometer can tell them apart for every axis except the vertical one. A real rotation
turns gravity in the sensor frame exactly as the gyro says, unless the rotation is about gravity
itself. If the current bias were right, the head turned by `R = integral(raw - bias)` during the
episode, and gravity should now read `R^T g0`. If gravity did not move, the bias is wrong in its
horizontal part (perpendicular to gravity), and the episode mean is the rate measured at rest.

What the accelerometer cannot see:
- **A pure yaw about the vertical predicts no gravity swing.** Scenario 11/21-type turns therefore
  never pass: the horizontal part of the disagreement is about 0. This holds at any head tilt: a
  yaw about the head's own up axis with the head pitched does predict a swing, and because the turn
  is real, the swing happens (scenario 26 proves the comparison is what rejects it).
- **The vertical part of the disagreement cannot be checked by gravity.** It is taken along with
  the episode on the strength of the horizontal proof, since the startup window has then been shown
  to contain motion. That leaves one exposure: a contaminated startup *and* a steady slow level turn
  inside the same episode, which are two independent events. Scenario 27 measures it.

The audit splits the startup errors evenly enough to make this rule worth having. Among starts
whose startup bias is more than 0.35 deg/s off, the horizontal part exceeds 0.35 in 126 of 253
starts and the vertical part in 88 of 253. The vertical part is what drives heading drift, so a
horizontal-only correction was rejected: it changes nothing in the drift metric while the posture
stays the same.

## The change (commit 1)

`PoseEstimator::Config`:
- `startup_episode_min_s = 10`: minimum episode span (0 disables the rule)
- `startup_episode_tilt_degs = 0.35`
- `startup_episode_predict_min_deg = 5`
- `startup_episode_observed_ratio = 0.3`

The rule applies during startup confirmation, before any reseed. The current rest episode's
calibration-grade windows (excluding the startup window) are tracked with their accelerometer
means, and the rotation `R` is integrated over every sample with the bias in use. The episode
replaces the bias in one step when all of the following hold:

1. The episode spans at least 10 s.
2. `|mean - bias|` is above the routine cap (0.35).
3. The horizontal part of that disagreement is at least 0.35 deg/s.
4. `R` carries at least half of that horizontal part. This stops window means that do not stand
   for the whole episode, such as a tilt during the windows that is undone between them.
5. `R` would swing gravity by at least 5 deg.
6. The observed gravity direction (first window against the latest) moved by at most 0.3 of that
   swing.

When the episode replaces the bias:
- The snapshot for a revert is the startup bias with only its horizontal part corrected. A later
  episode that agrees with the snapshot restores it.
- The escape state is cleared, as for the two-episode reseed (invariants 4, 5, 6 kept: single
  owner, every write clamped, the escape is never left half-armed).
- The pose stays continuous; only its rate changes.

`startup_episode_reseeds()` counts these; they are also counted in `startup_reseeds()`.
Angles use atan2 of the cross product and the dot product, never `acos` (bug 25).

`imu_replay` gains `--episode S` (0 disables) and `--episode-gates TILT,PREDICT,RATIO`, and the
audit prints single-episode reseeds and reverts. The audit CSV gains `episode_reseeds,reverts`
columns.

## Measured

### Synthetic (`pose_scenarios`, MSVC RelWithDebInfo)

Scenarios 1-23 and `pose_selftest` print output identical to the starting commit (`6b64660`) to
the last digit. The diff of the full `pose_scenarios` output is only the appended scenarios
25-27. The simulator has no magnetometer, and the rule never fires in 1-23: 21's turn is level,
so its horizontal disagreement is 0. Scenario 24 belongs to the coordinator's branch (turn
prediction), so these are numbered 25 and up.

**Scenario 25 (fails before, passes after).** The startup window catches a 1.3 deg/s movement,
about 1.2 deg/s roll plus 0.5 deg/s yaw. The wearer glances away, then rests for 45 s.

| Configuration | Drift over 45 s | Bias error at end | Reseeds |
|---|---:|---:|---:|
| Two-episode confirmation only (rule off) | 15.56 deg | 0.523 deg/s | 0 |
| With the rule | 6.00 deg | 0.006 deg/s | 1 (single-episode) |

The assertions are: the run without the rule drifts more than 8 deg with 0 reseeds (the gap),
exactly one single-episode reseed with the rule, the rule removes at least 50% of the drift, and
the bias ends within 0.1 deg/s of the truth. With the rule off, the scenario's first assertion
fails as intended.

**Scenario 26 (the guard).** Two long slow turns run with and without the rule:

| Turn | Registered | Reseeds | End yaw, rule on / off |
|---|---:|---:|---|
| Level head, 0.6 deg/s for 40 s | 76.0% | 0 | 18.4099 / 18.4099 (bit-identical) |
| Head pitched 25 deg, 1.0 deg/s about its own up axis for 30 s | 86.3% | 0 | 28.3818 / 28.3818 (bit-identical) |

The tilted turn's disagreement has a horizontal part of 0.39 deg/s, so it reaches the gravity
comparison. With that comparison switched off (ratio 1000), the same turn is learned (1
single-episode reseed, registered falls to 55.3%), which proves the comparison is the guard.

**Scenario 27 (the exposure, measured).** The contaminated startup from scenario 25 is followed by
a 0.6 deg/s level turn for 20 s inside the long episode.

| Configuration | Drift | Reverts |
|---|---:|---:|
| With the rule | 22.37 deg | 0 (the turn rode on the proof) |
| Without the rule | 19.16 deg | |

The rule costs 3.2 deg (17%) more here. The assertion is "at most 25% more than without the
rule". The revert cannot help in this case: the startup's own vertical part was contaminated as
well, so the later rest agrees with neither the consensus nor the snapshot, and the escape owns the
learned rate afterwards, as it owns the startup error without the rule.

### Replay startup audit

Fresh estimator every 10 s, gyro-only, heading lock in observe mode:
`imu_replay FILE --orientation ORIENT --startup-audit 10 --audit-horizon H [--episode 0] [--confirm 0]`.
"Track A" means `--episode 0`, which reproduces Track A's numbers exactly.

**60 s horizon**

| Gyro-only heading drift vs magnetometer | No confirmation (`--confirm 0`) | Track A (two-episode) | This change | Change vs Track A |
|---|---:|---:|---:|---:|
| Mean (821 starts with clean mag) | 9.21 deg | 7.13 deg | 6.52 deg | -8.6% |
| p50 / p90 / p99 | 4.33 / 24.39 / 57.72 | 3.82 / 19.29 / 40.88 | 3.64 / 16.75 / 38.94 | |
| Starts above 10 deg | 228 | 186 | 163 | |
| Paired per start vs Track A: better / worse by > 3 deg | | | 52 / 4 | |
| Paired per start vs Track A: better / worse by > 1 deg | | | 65 / 13 | |
| Paired vs no confirmation: better / worse by > 3 deg | | | 166 / 13 | |
| Reseeds (single-episode) / reverts | 0 / 0 | 221 / 17 | 259 (94) / 29 | |
| Time to tracking p50 / p90 / p99 | 5.76 / 10.40 / 23.98 s | same | same | unchanged |

**180 s horizon**

| Gyro-only heading drift vs magnetometer | No confirmation | Track A | This change |
|---|---:|---:|---:|
| Mean (796 starts) | 15.68 deg | 10.99 deg | 10.25 deg (-6.7%) |
| p50 / p90 / p99 | 8.11 / 42.55 / 98.88 | 7.32 / 24.30 / 58.17 | 6.83 / 21.92 / 53.71 |
| Starts above 10 deg | 334 | 293 | 281 |
| Paired vs Track A: better / worse by > 3 deg | | | 54 / 9 |
| Paired vs no confirmation: better / worse by > 3 deg | | | 181 / 28 |

**Starts with a single-episode reseed (92 at 60 s):**
- Mean drift 11.93 -> 6.44 deg (60 s) and 16.39 -> 9.71 deg (180 s).
- The reseed lands at p50 12.7 s and p90 22.9 s after tracking opens.
- For 56 of them, Track A's two-episode consensus would have fixed the bias later (p50 35.3 s ->
  12.4 s now). The other 38 were never reseeded within the 60 s period.
- 13 were later reverted to the horizontally corrected snapshot.

**The recorded session's own start (start 0):**
- 20.33 -> 14.68 deg over 60 s; 32.84 -> 15.99 deg over 180 s.
- The reseed lands 35.7 s after tracking (10.1 s into the long rest): bias `(0.009, 0.363, -0.825)`
  -> `(-0.002, 0.416, 0.330)`. Of the disagreement, 1.10 deg/s is horizontal and 0.37 vertical.
  The predicted gravity swing was 12.4 deg, the observed 0.79 deg.

**The worse starts are the exposure in practice.**
- Start 7780: the episode mean's vertical component was about 0.3 deg/s off the truth (a slow yaw
  during the rest), and the rule later reverted.
- Start 2100: a startup that was badly wrong horizontally but right vertically (0.3 deg of drift
  without the rule). The episode's vertical estimate carried about 0.1 deg/s of rest micro-motion
  (6.1 deg in 60 s).

The paired ratio (52:4 at 60 s, 54:9 at 180 s) is in line with Track A's own confirmation (134:11
and 156:24).

## Rejected along the way (measured, not shipped)

| Idea | Result |
|---|---|
| Gravity-verified reseed of the horizontal part only | Safe without assumptions, but heading drift depends only on the vertical part of the bias error, so it does nothing for the drift metric while the posture is unchanged |
| Raw-magnetometer stationarity bound, no calibration (the earth field's horizontal part is at least 12 uT, total at most 65 uT) | Without the hard iron, a worn rest's real tilt drift (p50 0.5 deg, p90 1.3 deg over 8-24 s) moves the field by up to 65 uT per rad. The vertical-rate bound came out p50 0.29-0.42, p90 0.47-1.33 deg/s, too loose to verify anything |
| Fitting the bias and the field from gravity plus raw mag over the episode (Python prototype, field direction profiled, minimum horizontal field as a constraint) | On the recorded rest, the fitted field and the vertical estimate were unstable: the field 67 -> 20 uT and the vertical 0.56 -> 0.22 deg/s as the span grew from 8 to 36 s. The few degrees of real motion during a rest do not determine the hard-iron-free field. Not robust enough for the single bias owner |
| Analytic swing prediction from the window means (`|d| * span` about `d`), instead of integrating every sample | Same audit result (60 s mean 6.56, 49 better / 4 worse), but it trusts the window means for the time between windows. Replaced by the integrated rotation plus the "carries at least half" test (mean 6.52, 52 / 4) |
| Span 15 / 20 s | 60 s mean 6.70 / 6.84 deg, better/worse 37:2 / 27:2. Fewer worse starts, but most of the benefit is lost (the drift accrues while the rule waits) |
| Horizontal minimum 0.5 deg/s | 6.72 (span 10), 34:4. Lower power, same worse count |
| Predicted-swing minimum 8 deg | 6.65, 41:3 |
| Observed ratio 0.2 | 6.54, 49:3, within noise of 0.3; 0.3 kept for the slightly larger benefit |

## Needs live-glasses verification

1. A real launch is preceded by the "hold still" prompt, so a clean startup should be the common
   case. The rule only acts after a startup window that contained head motion, followed by a long
   rest. Signs that it fired: the engine's `startup confirmation: reseeds=N` line (single-episode
   reseeds are included in N; the engine log does not yet print them separately), and the view
   stopping its slow turn about 10 s into the first long rest.
2. The exposure in scenario 27: if a wearer starts with a fidgety startup and then deliberately
   turns very slowly about the vertical for more than 10 s, that turn's rate can be learned. The
   escape and the next motion-separated rest bound it, as without the rule.
