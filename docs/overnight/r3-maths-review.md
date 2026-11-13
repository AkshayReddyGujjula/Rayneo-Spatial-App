# R3-C: adversarial review of tonight's IMU maths

Branch `overnight-claude-improvements` at `e7dcfa2`. I reviewed two pieces of code:

- **A.** Single-episode startup confirmation (`confirm_single_episode`, `confirm_startup_window` and
  their callers in `src/imu/pose_estimator.cpp`).
- **B.** Calibration repeats (`split_gestures`, `combine_axis_repeats`, `analyze_motion_repeats`,
  quality and file keys in `src/imu/orientation_calibration.cpp`).

Build: MSVC Ninja RelWithDebInfo with `/W4 /permissive-`, no new warnings, and 11/11 ctest. No
glasses were used. The evidence comes from synthetic runs through the real estimator and from the
replay audit of the recorded 2026-09-25 session.

## Confirmed bug (fixed)

### 1. The single-episode gravity check compared different instants (commit `2b4f64a`)

**The defect.** Each window's mean gravity stands for the orientation half-way through that window.
The integrated rotation `single_rotation_` covers a different span: from the **end** of the first
window to the **end** of the current one. Each end of the comparison is therefore skewed by half a
window, which is 0.63 s.

Rest windows may contain motion up to the 3 deg/s rest ceiling. With motion near that ceiling in both
reference windows, the two skews add up to about 3.8 deg. The checks they have to beat are a 5 deg
minimum predicted swing and an observed ratio of 0.3, which allows only 1.5 deg of observed swing.
So a real slow nod could look like "gravity did not move", and that would reseed a **correct**
startup bias.

This is outside the stated exposure. Scenario 27 needs a contaminated startup plus a level turn.
This case needs neither.

**Proof.** New scenario `pose_scenarios` 28:

- The startup is clean: true bias (0, 0.4, 0), estimated bias within 0.002 deg/s.
- The wearer nods up slowly at 2.9 deg/s.
- The wearer looks left and back at 3.5 deg/s. That is too fast for a rest window and too slow to
  count as a detected motion, so everything stays one episode.
- The wearer nods down slowly. The nod-down length is swept over 41 values (1.0 to 5.0 s), so the
  end of the movement lands at every phase of a window.

| | Single-episode reseeds | Worst yaw drift over the next 60 s still |
|---|---:|---:|
| Before | 16 / 41 | 2.784 deg (rule off: 0.020 deg) |
| After | 0 / 41 | 0.020 deg |

One false reseed changed the bias from (-0.002, 0.40, -0.002) to (-0.001, 0.325, 0.739):

- The horizontal error is 0.74 deg/s.
- The vertical error is 0.075 deg/s, picked up from the tail of the look-around inside a window.
- The 0.74 residual also locks out routine adaptation until the slow escape catches up.

A wider offline sweep of the same pattern (16,800 timings and filler variants, private harness, not
committed) found 191 reseeds before the fix and 0 after.

**The fix.**

- The rotation over every calibration window is now integrated from the window's first sample, with
  the bias in use (`window_rotation_`, reset together with the window).
- At window completion its half-rotation, the quaternion square root, is taken.
- The predicted swing now uses the rotation between the two windows' mid-points:
  `mid = half(first window) * single_rotation_ * conj(half(current window))`.

For a still head the result is the same as before. With a wrong bias, the window rotations are about
the same axis as `single_rotation_`, so the conjugation leaves its swing unchanged. Unchanged by the
fix:

- the gates and all parameters;
- the "carries at least half" test (end to end on both sides);
- the snapshot and revert logic;
- the clamps;
- `reset_escape()`.

**Regression check.**

- Scenarios 1-27 print **byte-identical** output, including 25-27, and so does `pose_selftest`.
- The replay startup audit, run as `imu_replay imu_raw.csv --orientation orientation.json
  --startup-audit 10 --audit-horizon 60`, gives the results below.

| Audit measure | Before | After |
|---|---:|---:|
| 60 s drift mean | 6.52 deg | 6.51 deg |
| p90 | 16.75 deg | 16.43 deg |
| Reseeds (single-episode) | 259 (94) | 258 (94) |

Eight of the 870 starts changed. Six moved by less than 3 deg: 2850, 2960, 3180, 5070, 7370 and 8230.
The two that moved by more than 3 deg:

- **Better:** start 4890, 22.4 -> 10.8 deg. The reseed now lands at 24.8 s instead of a later
  two-episode reseed at 39.0 s.
- **Worse:** start 3450, 4.1 -> 13.5 deg. Its startup was contaminated, and the old check reseeded it
  only thanks to the skew. With matching instants the swing no longer passes the unchanged 0.3 ratio.

This is a correctness fix, not a tuning change. On real data it is neutral.

## Not confirmed (checked, no defect I could prove)

### A. Single-episode and two-episode confirmation

- **Frames.** The window means, `accel_mean`, `bias_degs_`, the integrated rotation and the
  horizontal/vertical split are all in the raw sensor frame. `map_*` is applied only on the fusion
  path. The sign of gravity (specific force points up) cannot matter: the projection and the angles
  are invariant to it.
- **Rotation convention.** `single_rotation_ = q * step` composes body rates on the right. Gravity at
  the end is therefore `conj(q) g0 q`. The rotation vector (`atan2`, shortest arc) has the same sign
  as `d * span` for a constant error.
- **A correct bias cannot pass, except through the skew.** With the true bias, the integrated
  rotation is the real rotation, so the predicted and observed swings agree. The only remaining ways
  to fool the check are accelerometer errors larger than 70% of a swing of at least 5 deg. Sustained
  linear acceleration is held under the 0.3 m/s2 window deviation gate, which corresponds to about
  1.75 deg of apparent tilt. Centripetal acceleration of a slow head turn is below 0.001 m/s2.
- **Sawtooth motion** (windows sample one direction, the return happens in the 3-4 deg/s band). The
  "carries at least half" test rejects it whenever the net rotation is small, and the two-episode
  agreement ends confirmation when a later episode's mean returns within 0.3 deg/s of the startup.
  Before the fix, the only way past both was the skew above.
- **Gaps and motion.** A gap of 50 ms or more increments `motion_events_` and returns before any
  integration, and the next window starts a new single episode. Integration across motion before the
  next window is discarded by that reset. Gap time is not counted in `confirm_time_s_` either. In
  estimator time this is harmless, and a long disconnect is normally followed by a reconfigure.
- **Window-free motion inside an episode.** Motion in the hysteresis band between the enter and exit
  gates (gyro deviation 1.5 to 3 deg/s, or raw rate 3 to 4 deg/s) keeps `still_` and the same
  episode, but `single_rotation_` integrates every sample, so the motion is accounted for.
  Rotation about earth-vertical is the one exception, and that is the documented exposure.
- **Precision.**
  - Angles use `atan2` of the cross and dot products.
  - The rotation angle uses `atan2(|v|, |w|)`.
  - Per-sample quaternion rounding over 60 s at 476 Hz costs less than 0.1% of the vector part,
    with no effect on the 0.5x and 0.3x thresholds.
  - `time_s_` in float has an ulp of 4e-6 s at 60 s, so the error in `span` is negligible.
- **Division by small numbers.** Every division is guarded: `h_norm >= 0.35` before the `along`
  division, both gravity norms against 1e-3, `|v| > 1e-9`, and at least 2 windows or at least 1
  episode window.
- **Escape interplay (invariant 6).** Both reseeds and the revert call `reset_escape()`. It clears
  the dwell, open, armed and return-dwell state together, so the escape is never left half-armed.
  The confirmation runs before the rollback and routine blocks in the same sample.
- **Clamps (invariant 5).** The reseed, the snapshot and the revert all go through `clamp_bias`. The
  escape rollback restores a snapshot that was itself clamped.
- **Confirmation period.**
  - `confirm_startup_window` runs only while `confirm_active_` is true.
  - The flag is set only by `finish_calibration()` and cleared by a timeout, a decision,
    episode-slot exhaustion (8) or `configure()`.
  - When confirmation ends mid-episode it simply stops: the integration needs `confirm_active_`.
  - `recenter()` does not touch the bias path, which is correct because the reference has nothing to
    do with the bias.
  - `configure()` resets every confirmation field.
- **The revert cannot undo a horizontal reseed.** When `d` is purely horizontal, the snapshot equals
  the consensus. A later episode then matches the consensus branch first, so no revert is possible.
  This is the stated design (the horizontal part is "proven"). With the skew fixed, that proof is now
  sound.
- **Single-window decisions.** A two-episode consensus or revert can be decided by one 1.26 s window
  of a new episode. That is Track A's measured design (track-a.md), not a code defect.

### B. Calibration repeats

- **Antipodal axes.** Each gesture's axis is signed by its own out-half and then by `command_sign`.
  How wrong-direction gestures are handled:

  | Gestures | Outcome |
  |---|---|
  | 3 or more, one wrong | The wrong one is 180 deg from the medoid and is rejected (median rule, limit at least 8 deg) |
  | 2 opposite | Kept together; the sum is near zero |
  | 2 against 2 | A tie; the sum is near zero |

  A zero sum normalises to `Vec3{}`. A near-zero sum has a random direction roughly perpendicular to
  the true axis, and it always fails a later gate:
  - a zero yaw axis fails `UP_MISMATCH` (90 deg);
  - a zero nod or tilt axis fails `HANDEDNESS` (90 deg);
  - a perpendicular near-zero sum fails `UP_MISMATCH` for yaw, `NON_ORTHO` for nod and `HANDEDNESS`
    for tilt.

  An improper or wrong matrix is never saved. The error message is misleading ("keep your head
  level"), but that is cosmetic, so it was not changed.
- **Never the majority.** The outlier limit is at least 3x the median deviation from the medoid, and
  the median is that of the other gestures. With 3, 4 or 5 gestures, at least the medoid plus
  ceil((n-1)/2) are kept. The medoid itself is never rejected, so the case where all gestures are
  rejected cannot happen. When no gesture is usable, the step fails with the first failing gesture's
  code.
- **Proper rotation with gravity on +Z.** The combined axes feed the unchanged basis construction:
  `up = bisector(gravity, yaw)`, then two cross-product passes, with `right x forward = up` and the up
  row in row 2. That keeps the determinant at +1 by construction, and `load_orientation_calibration`
  rechecks it.
- **N = 1 path.** The code takes `options.repeats <= 1` straight to `analyze_motion_phase`, and the
  existing golden-matrix test pins it. `calibration_quality` returns `orientation_quality` unchanged
  without repeats.
- **File keys.**
  - `orientation_repeats` is written only when it is above 1, and `orientation_spread_deg` only when
    it is at least 0. N = 1 leaves the spread at -1, so it is not written.
  - Older files load with unknown values: quality -1, repeats 0, spread -1.
  - A `--mag` save reloads the info and writes it back, and an orientation save keeps the mag keys.
  - `"mag_quality"` cannot match `"orientation_quality"`, because the key search includes the opening
    quote.
  - `setprecision(3)` stays in effect after the quality key, but the mag fields set the same precision
    themselves.
- **Spread precision.** `angle_deg` here is `acos`-based, so angles below about 0.02 deg read as 0.
  It only feeds the displayed consistency (spread / 8 deg), and the effect there is below 0.003, so it
  was not changed.

## Commits

- `2b4f64a` Single-episode startup check: compare gravity at matching instants
- (this file) docs: R3-C maths review
