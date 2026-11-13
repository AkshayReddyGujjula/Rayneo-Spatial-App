# R2-B: orientation calibration accuracy and a trustworthy quality score

Overnight run, 2026-11-13, on top of `overnight-claude-improvements` (`6b64660`). MSVC Ninja
RelWithDebInfo, warning-clean, 11/11 ctest at every commit. No glasses: every number below comes
from synthetic IMU data pushed through the real calibration code
(`orientation_calibration_selftest`); nothing was run against hardware.

## What changed

1. **`orientation_calibrate --repeats N`** (1-5, default 1). Each turn / nod / tilt step asks for
   the gesture N times in one recording ("... Do this 3 times, coming back to centre each time.");
   the recording grows from 6 s to `6 + 3.5 (N - 1)` s. Nothing changes without the flag.
2. **Gesture splitting** (`split_gestures`). Each motion run (the existing on/off hysteresis) is
   split into out-and-back gestures by integrating the angle about the run's principal axis: a
   gesture ends once it has swung at least 6 deg (half the 12 deg excursion gate), is back within
   30% of its peak and has stopped returning. A pause at centre is therefore optional (the worn
   GT's 0.75 deg/s noise means pauses rarely end a hysteresis run anyway). Settling tails that
   never swing 6 deg are dropped.
3. **Per-gesture gates, robust combination** (`analyze_motion_repeats`, `combine_axis_repeats`).
   Every gesture must pass the unchanged single-gesture gates (excursion, dominance, off-axis)
   on its own. Usable axes are combined by a chordal mean on the sphere (normalised sum of unit
   axes) after outlier rejection: with three or more gestures, any gesture further than
   `max(8 deg, 3 x median deviation)` from the medoid is dropped (never the majority; with two
   gestures both are kept because nobody can tell which one is wrong - the spread says so
   instead). The combined axes go into the **unchanged** basis construction, so the result is
   still a proper rotation (det +1) with gravity in the up row, exactly as invariant 3 requires.
4. **Consistency and quality.** Per step: the spread (rms angle of the used gestures about their
   mean). Overall: `repeat_spread_deg` (rms over the three steps), an estimated axis error
   (`spread / sqrt(n - 1)`, the standard error of the mean direction), and
   `consistency = 1 - spread / 8 deg`, capped at 0.6 (at most "Fair") when a step yielded fewer
   than two usable gestures. The orthogonality residual is the existing `orientation_quality`
   (headroom of up-vs-yaw, nod level, nod-tilt orthogonality and handedness against their gates).
   The reported and stored score is `calibration_quality = min(orientation_quality,
   consistency)`; without repeats it is `orientation_quality` exactly. Bands
   (`calibration_quality_label`): **Good** >= 0.7, **Fair** >= 0.4, **Retry recommended** below -
   the same thresholds/colours as the running "Signal quality" bar.
5. **Output.** `--progress-json`: each motion step's `ok` carries
   `min(axis_quality, step consistency)` and a detail like `3 of 3 gestures used, spread 1.1 deg,
   excursion 24.8 deg`; `done` carries `calibration_quality` and appends
   `; 3 repeats: spread 1.05 deg, estimated axis error 0.74 deg` to the existing detail. Console:
   `[cal] repeats phase=...`, `[cal] consistency ...` and, on every run,
   `Calibration quality: Good (0.87)`, plus a "repeat (ideally with --repeats 3)" hint below 0.4.
   The protocol shape is unchanged (no new keys or states).
6. **UI** (`ui_pages.cpp`, `diagnostics.cpp`). The finished-calibration panel (Tracking page and
   onboarding wizard) shows one line "Calibration quality: Good / Fair / Retry recommended" in
   the band colour, and its tick becomes a warning icon for Retry recommended. The Head
   calibration health detail names the stored band ("valid, quality Good, saved ..."). Snapshots:
   `tracking-calibration-saved-good-1280x800`, `wizard-3-head-saved-retry-1280x800-light`.

## File format (backward compatible, version stays 1)

Optional keys after `sensor_to_head`, written by `save_orientation_calibration`:

```
"orientation_quality": 0.873,      // calibration_quality, always written
"orientation_repeats": 3,          // only with --repeats > 1
"orientation_spread_deg": 1.050,   // only when a spread was measured
```

`load_orientation_calibration` is unchanged and ignores them; `load_orientation_calibration_info`
reads them (absent = unknown: quality -1, repeats 0, spread -1). A later `--mag` save keeps them,
and an orientation save keeps the mag keys (as before). `"mag_quality"` is never mistaken for
`"orientation_quality"` (tested).

## Measured accuracy (orientation_calibration_selftest, reproducible)

Model: the test's 4 deg / 37 deg mount; every gesture rotates about the commanded head axis tilted
by a random 3 deg rms (direction random), white gyro noise 0.45 deg/s per axis (~0.75 deg/s
magnitude, the measured GT stillness), speed and size +/-20%, a 0.3-0.9 s pause at centre after
half the gestures. Error = rotation angle between the result and the true `sensor_to_head`.
150 sessions, the same gestures reused (single-shot uses gesture 1 only):

| Gestures per step | Mean error | p95 error | Sessions rejected |
|---|---:|---:|---:|
| 1 (today's flow) | 1.902 deg | 3.443 deg | 0 / 150 |
| 3 | 1.138 deg (0.60x) | 2.064 deg | 0 / 150 |
| 5 | 0.914 deg (0.48x) | 1.615 deg | 0 / 150 |

- Splitting: 900 of 900 repeat steps found exactly N usable gestures, with and without pauses.
- Outlier: three nods, the second rolled 15 deg toward the shoulder (the bug-20 coupling): it is
  rejected (14.1 deg from the medoid); nod axis error 0.74 deg robust vs 5.45 deg for a plain
  mean; final calibration error 0.327 deg. With rejection disabled the same session gives
  2.670 deg.
- Outlier floor: 5 deg dropped 93 honest gestures across the Monte Carlo (3-gesture mean
  1.154 deg); 8 deg drops 5 (1.138 deg) and still rejects the 14 deg outlier; no rejection at
  all gives 1.138 / 0.903 deg (3 / 5 gestures) but 2.67 deg on the outlier session. 8 deg is the
  shipped floor.
- Quality: per-gesture wobble 1 deg -> spread 0.89 deg, estimated error 0.44 deg, quality 0.89
  **Good** (true error 0.186 deg); wobble 5 deg -> spread 4.40 deg, estimated error 2.20 deg,
  quality 0.45 **Fair** (true error 0.919 deg).
- Default path: pinned to the pre-change matrix (golden values from `6b64660`, < 1e-6), and a
  `--repeats` run on a single-gesture recording gives the identical matrix with quality capped
  at 0.6 (consistency unverifiable).

Tests that fail without the change: the 0.75x / 0.65x error ratios (fail if only the best
gesture is used), the outlier test (robust < 1 deg vs plain > 4 deg; fails without rejection),
the quality-drop test (fails if quality ignores the spread), the split-rate test, the golden
default, and the file round trips.

## Recommended default

Keep the tool default at 1 (exact old behaviour), and **use `--repeats 3` for the in-app wizard**
once the user agrees: it cuts the modelled error by 40% (p95 3.4 -> 2.1 deg) for about +21 s
(three motion recordings of 13 s instead of 6 s; about 63 s instead of 42 s with the 5 s
countdowns), and it is the only way the quality line can say anything about consistency. 5 gives
another 20% for another +21 s - not worth it for a routine recalibration. The wizard currently
still passes no `--repeats` (`calibration_tool_arguments` in `calibration_progress.cpp`); adding
`"--repeats", "3"` there and the app_selftest argument expectation is a two-line change I left
for the integrator/user, as it lengthens the default flow.

## Limits (stated, not hidden)

- **Synthetic only.** The win assumes the wearer's axis error is random from gesture to gesture
  (bug 20 saw the nod lean vary in direction between calibrations, which fits, but nothing has
  measured within-session gesture-to-gesture scatter on a worn GT). A habitual lean that is the
  same in every gesture is not removed by averaging - no IMU-only method can tell it from the
  mount. Validate with a worn `--repeats 3 --log` session: the per-step spreads printed by the
  tool are exactly that measurement.
- **Gravity is not averaged.** Up is still the bisector of the still-phase gravity and the
  averaged turn axis; a forward/back head lean while "looking straight ahead" is unchanged by
  repeats. In the model (perfect gravity) the bisector rule itself lets turn-axis wobble tilt
  "up" (worst gravity-to-+Z over all 450 modelled results: 3.42 deg); averaging shrinks that
  together with the turn-axis error.
- **Splitting** needs each repeat to come back within 30% of its peak; gestures that stop at 40%
  merge into one segment (still averaged by the covariance, but counted as one repeat, so the
  spread under-reports).
- **Two gestures** cannot reject an outlier; the spread rises instead (quality drops), which is
  the honest outcome.
- The quality thresholds (8 deg spread = 0, Good >= 0.7, Fair >= 0.4) are judgement, not
  measurement; revisit once real spreads are known.
- Not run against hardware, the controller UI live, or the user's `config/orientation.json`.
