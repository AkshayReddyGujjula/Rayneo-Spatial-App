# RayNeo GT head-alignment calibration - design

**Problem.** After the repo's package mapping `[x, -z, y]` the mapped "body" frame `B` has
`+Z ~= up` (verified), so the fused quaternion's *vertical* is correct. What was never
measured is the constant mounting rotation **about that vertical** - i.e. where the head's
*forward* and *right* axes sit inside the `B` X/Y plane. As a result a nod and a head-tilt
land on swapped/mirrored euler channels, and there is an unknown constant azimuth.

**Deliverable of this doc.** A ~20 s guided procedure that measures three unit vectors -
the head's **up / right / forward** axes expressed in `B` - and emits a 3x3 matrix to bake
into the source. Validated by a synthetic Monte-Carlo (`scratch/calib_design.py`).

---

## 1. Why this works (one line of math)

Let `C` be the fixed unknown rotation that maps head coordinates to `B` coordinates
(`v_B = C v_H`). For a head motion that is a pure rotation about head axis `a_H` at rate
`w_rad/s`, the gyro reports `w * (C a_H)`: **the body-frame direction of the measured
angular-rate vector is `C a_H`**. So one guided motion per anatomical axis recovers that
axis:

| Guided motion | turns about head axis | body-frame gyro direction |
|---|---|---|
| turn left/right (yaw) | up    | `u_B = C [0,0,1]` |
| nod down/up (pitch)   | right | `r_B = C [1,0,0]` |
| tilt right/left (roll)| forward | `f_B = C [0,1,0]` |

The head frame is right-handed, `right x forward = up` (`u_B = r_B x f_B`). Madgwick is
Z-up, so head vectors must use the canonical coordinate order **(right, forward, up)**.
The matrix `M` whose rows are `(r_B, f_B, u_B)` maps body -> head
(`v_head = M v_B`) and equals `Cᵀ`.
Folding in the repo's package mapping `P = [[1,0,0],[0,0,-1],[0,1,0]]` gives the
bakeable **sensor -> head** matrix

```
A = M * P        (3x3, row-major)
```

apply `A` to raw `99 65` vectors and the Madgwick body frame *is* the head frame: the
fused quaternion then has head-up on its Z, and `quat_to_euler` separates turn/nod/tilt
onto Z/X/Y cleanly instead of smearing nod and tilt together.

Only two independent horizontal quantities are strictly needed (`u` from gravity +
`r` from a nod; then `f = u x r`); the tilt phase is redundancy/validation.

Front door sign conventions used below (right-handed rotation, `+` = RH rule):
`+about up = turn left`, `+about right = look up`, `+about forward = tilt right`.
These are baked into the prompts so the algorithm knows the expected sign of each motion.

---

## 2. Procedure

Total ≈ **18-20 s of "hold still / move"**, no exact angles required.

| Phase | Prompt shown to wearer | Record | Recorded data |
|---|---|---|---|
| **P0 still** | "Sit still and look straight ahead. Calibrating..." | 3.0 s | raw gyro, raw accel |
| **P1 yaw** | "Slowly turn your head to the **LEFT** as far as is comfortable, then back to centre." | ready cue 1.5 s, then 4.5 s window | raw gyro, accel, tick |
| **P2 nod** | "Slowly nod **DOWN** (chin toward chest), then back up to centre." | ready cue 1.5 s, then 4.5 s | raw gyro, accel, tick |
| **P3 tilt** | "Slowly tilt your head to your **RIGHT** shoulder, then back to centre." | ready cue 1.5 s, then 4.5 s | raw gyro, accel, tick |

Between phases: "return to centre and hold still" (0.5 s). Each motion window is
auto-cropped; the user may move slowly or briskly. Nothing is required to be exactly 90°.

Per phase keep a buffer of `ImuSample` (gyro deg/s, accel m/s², `tick_100us`). At 476 Hz
a 4.5 s window is ~2150 samples; ~9k samples total (~250 kB) - trivially cheap. Also
offer a debug CSV dump (`--cal-log out.csv`) in the existing probe format
`host_us,tick_100us,ax,ay,az,gx,gy,gz,mx,my,mz,temp_c` so a run can be re-analysed
offline.

No fusion filter is involved - calibration runs on raw samples, so it is independent of
Madgwick's transient and unaffected by the mag being disabled.

---

## 3. Algorithm (pseudocode)

```
# ---------------- helpers
eigen_sym3(C) -> (lams[3] desc, vecs[3])      # 3x3 symmetric Jacobi, ~20 sweeps
closest_rotation(Q):                          # nearest proper rotation to Q
    return Q * inverse_sqrt(Q^T Q)            # via eigen_sym3 of Q^T Q

# ---------------- per-phase axis extraction
pca_axis(w[], cmd_sign):                      # w = bias-corrected rad/s or deg/s samples
    m   = mean(w)
    X   = w - m                               # mean-centring removes the constant gyro bias
    Cov = X^T X / n                           # 3x3 symmetric
    (lam, v) = eigen_sym3(Cov)                # lam[0] >= lam[1] >= lam[2]
    axis      = v[0]
    dominance = lam[0] / (lam[1] + lam[2])    # how single-axis the motion was
    rms_perp  = rms( |w - (w.axis) axis| )    # off-axis residual
    # sign: the *first half* is the authoritative commanded direction
    p = sum_{i < n/2} w[i].axis
    if p * cmd_sign < 0: axis = -axis
    # excursion = peak-to-peak accumulated angle (robust to out-and-back net ~0)
    cum = prefix_sum(w[i].axis / fs);  excursion = max(cum) - min(cum)
    return {axis, excursion, dominance, rms_perp, n}

# ---------------- full procedure
calibrate(samples_by_phase):
    # ---- P0: bias + up from gravity
    g0 = samples_by_phase.still
    bias   = mean(g0.gyro)
    up_g   = normalize(mean(g0.accel))
    if rms(|g0.gyro - bias|) > 1.5 deg/s: FAIL("HOLD_STILL")   # combined 3-axis rms

    # ---- P1..P3
    cmd_sign = { yaw:+1 (turn left), nod:-1 (look down), tilt:+1 (tilt right) }
    for phase in [yaw, nod, tilt]:
        w   = samples_by_phase[phase].gyro - bias
        [s,e] = active_window(w, on=5.0, off=3.0)          # contiguous |w|>on, hysteresis
        if e - s < 0.25*fs:  FAIL("NO_MOTION", phase)
        est = pca_axis(w[s:e], cmd_sign[phase])
        if est.excursion < 25.0:  FAIL("SMALL_EXCURSION", phase)
        if est.dominance < 8.0:   FAIL("AMBIGUOUS_AXIS", phase)
        if est.rms_perp  > 4.0:   FAIL("OFF_AXIS", phase)
        est[phase] = est

    # ---- consistency between independent axes
    if angle(up_g, up_yaw)               >  8.0: FAIL("UP_MISMATCH")
    if |90 - angle(right_nod, up_g)|     >  8.0: FAIL("NOD_NOT_LEVEL")
    if |90 - angle(right_nod, fwd_tilt)| > 20.0: FAIL("NON_ORTHO")
    if angle(cross(up_yaw,right_nod), fwd_tilt) > 15.0: FAIL("HANDEDNESS")

    # ---- least-squares orthonormal basis
    u_est = normalize(up_g + up_yaw)                 # average two independent up estimates
    [u,r,f] = build_basis(u_est, right_nod, fwd_tilt)
    M = rows(r, f, u)                                # body -> head; Z remains up
    A = M * P                                        # sensor -> head  (P = [x,-z,y])
    emit LOG(A, M, diagnostics)

build_basis(u_est, r_est, f_est):
    Q = columns(r_est, f_est, u_est)                 # candidate head->body, (right,forward,up)
    C = closest_rotation(Q)                          # spreads the residual error, det=+1
    M = transpose(C)                                 # rows (right,forward,up)
    return (M.row2, M.row0, M.row1)                  # (up, right, forward)
```

`active_window` marks samples with `|gyro - bias| > on`, with hysteresis `off`, and takes
the contiguous active run; motions that dip through zero in the middle stay one window.

### Validation checks & failure strings

| Check | Threshold | Failure message (app prints) |
|---|---|---|
| P0 stillness | combined 3-axis gyro rms ≤ 1.5 deg/s | `not enough stillness detected - please hold still and try again` |
| motion present | window ≥ 0.25 s | `not enough motion detected in the <phase> step - try again` |
| excursion | ≥ 25° peak-to-peak | `not enough motion detected (<phase> only reached X deg) - try again` |
| dominance | `lam1/(lam2+lam3)` ≥ 8 | `motion was ambiguous in the <phase> step - move clearly in one direction` |
| off-axis rms | ≤ 4 deg/s | `keep your head level - only move <phase> during the <phase> step` |
| up vs turn | ≤ 8° | `sensor gravity and turn axis disagree - redo, keep head level` |
| nod level | `|90-angle|` ≤ 8° | `nod was not level - keep your head level while nodding` |
| nod⊥tilt | `|90-angle|` ≤ 20° | `nod and tilt got mixed up - move one axis at a time` |
| handedness | `u x r` vs `f` ≤ 15° | `the tilt did not match turn x nod - redo the tilt step` |

On any FAIL the app re-runs only the offending phase, or the whole procedure after 3 tries.

---

## 4. Runtime log format (exact)

One stable, greppable format. `[cal]` prefix on every line.

```
[cal] begin v1 fs=476.0 mag=off
[cal] phase=still  n=%d dur=%.2fs gyro_rms=%.3f deg/s accel=%.4f m/s^2
[cal] still  up_B=(%+.4f %+.4f %+.4f) tilt_Z=%.2fdeg bias=(%+.3f %+.3f %+.3f) deg/s
[cal] phase=%s n=%d active=%.2fs excursion=%.1fdeg dominance=%.1f perp_rms=%.3f deg/s
[cal] phase=%s axis_B=(%+.4f %+.4f %+.4f) sign=%+d prompt="%s"
[cal] check %s value=%.2f limit=%.2f %s          # PASS / FAIL
[cal] WARN %s
[cal] FAIL %s : %s
[cal] result %s                                   # ok / failed
[cal] head_basis_B up=(%+.4f %+.4f %+.4f) right=(%+.4f %+.4f %+.4f) forward=(%+.4f %+.4f %+.4f)
[cal] sensor_to_head A rows:
[cal]   { %+.5ff, %+.5ff, %+.5ff},
[cal]   { %+.5ff, %+.5ff, %+.5ff},
[cal]   { %+.5ff, %+.5ff, %+.5ff},
[cal] bake_block begin
[cal] <C++ snippet, one line per source line>
[cal] bake_block end
[cal] summary msg="%s"
```

### Example single run (a fixed synthetic mount, azimuth 37°, mount tilt 2.5°, bias 0.2 deg/s)

```
[cal] begin v1 fs=476.0 mag=off
[cal] phase=still  n=1428 dur=3.00s gyro_rms=0.692 deg/s accel=9.8399 m/s^2
[cal] still  up_B=(+0.0001 -0.0436 +0.9991) tilt_Z=2.50deg bias=(+0.081 -0.204 +0.113) deg/s
[cal] phase=yaw n=1402 active=2.95s excursion=85.9deg dominance=13678.8 perp_rms=0.550 deg/s
[cal] phase=yaw axis_B=(+0.0001 -0.0436 +0.9991) sign=+1 prompt="turn left"
[cal] phase=nod n=1401 active=2.94s excursion=76.3deg dominance=9910.9 perp_rms=0.570 deg/s
[cal] phase=nod axis_B=(+0.7986 +0.6013 +0.0262) sign=-1 prompt="nod down"
[cal] phase=tilt n=1393 active=2.93s excursion=62.0deg dominance=7153.1 perp_rms=0.550 deg/s
[cal] phase=tilt axis_B=(-0.6019 +0.7979 +0.0349) sign=+1 prompt="tilt right"
[cal] check up_vs_yaw value=0.02 limit=8.00 PASS
[cal] check nod_level value=0.00 limit=8.00 PASS
[cal] check nod_dot_tilt value=0.02 limit=20.00 PASS
[cal] check handedness value=0.02 limit=15.00 PASS
[cal] result ok
[cal] head_basis_B up=(+0.00006 -0.04359 +0.99905) right=(+0.79861 +0.60128 +0.02619) forward=(-0.60185 +0.79785 +0.03485)
[cal] sensor_to_head A rows (right, forward, up):
[cal]   { +0.79861f, +0.02619f, -0.60128f},
[cal]   { -0.60185f, +0.03485f, -0.79785f},
[cal]   { +0.00006f, +0.99905f, +0.04359f},
[cal] bake_block begin
[cal] // head-alignment calibration, run YYYY-MM-DD, device <serial>, mag off
[cal] // head axes in the mapped package frame B (after [x,-z,y]); right x forward = up
[cal] constexpr float kUpB[3]      = {+0.00006f, -0.04359f, +0.99905f};
[cal] constexpr float kRightB[3]   = {+0.79861f, +0.60128f, +0.02619f};
[cal] constexpr float kForwardB[3] = {-0.60185f, +0.79785f, +0.03485f};
[cal] // sensor -> head, row-major (right, forward, up); apply to raw 99 65 vectors
[cal] constexpr float kSensorToHead[9] = {
[cal]     +0.79861f, +0.02619f, -0.60128f,
[cal]     -0.60185f, +0.03485f, -0.79785f,
[cal]     +0.00006f, +0.99905f, +0.04359f,
[cal] };
[cal] bake_block end
[cal] summary msg="calibration ok - paste bake_block into src/imu/pose_estimator.cpp"
```

(That run's recovered basis is within **0.004°** of the true mounting rotation.)

---

## 5. Runtime integration

1. **Feed head-frame data.** `PoseEstimator::Config::sensor_to_head` is applied by
   `map_gyro/map_accel/map_mag`:

   ```cpp
   Vec3 PoseEstimator::map_gyro(const Vec3& v) const {
       if (!cfg_.map_package_axes) return v;
       const float* m = cfg_.sensor_to_head;   // kSensorToHead, 9 floats
       return Vec3{
           m[0]*v.x + m[1]*v.y + m[2]*v.z,
           m[3]*v.x + m[4]*v.y + m[5]*v.z,
           m[6]*v.x + m[7]*v.y + m[8]*v.z,
       };
   }
   ```
   The default `{1,0,0, 0,0,-1, 0,1,0}` is the old uncalibrated package mapping.
   `spatial_desk` requires a valid calibration file before it starts IMU tracking.

   The `kUpB/kRightB/kForwardB` vectors are informational; `kSensorToHead` is what the code
   needs. `kSensorToHead = M * P` exactly.

2. **Relabel the euler channels.** With head-frame data, `quat_to_euler(q_rel)` gives
   `yaw` about head-**up** (turn), `pitch` about head-**forward** (tilt), `roll` about
   head-**right** (nod). So the camera must take nod from the *roll* channel and tilt from
   the *pitch* channel:

   ```cpp
   // src/render/camera.cpp - camera_applied_euler()
   Euler e = quat_to_euler(head_relative);
   Euler out;
   out.yaw_deg   = signs.yaw   * e.yaw_deg;    // turn  (about head up)
   out.pitch_deg = signs.pitch * e.roll_deg;   // nod   (about head right)
   out.roll_deg  = signs.roll  * e.pitch_deg;  // tilt  (about head forward)
   ```
   Calibration removes the cross-coupling. The verified default camera signs are
   `(-1,-1,-1)` for turn, nod, and tilt respectively, and `camera_selftest.exe` locks the
   mapping with assertions. The unsafe live `I/K/L` sign toggles are intentionally absent.

Run `build\orientation_calibrate.exe` while wearing the glasses. It records the four phases,
validates the result, and atomically replaces `config\orientation.json` only after success.
`spatial_desk.exe` loads that file by default, resolved relative to the executable rather
than the current working directory.

---

## 6. Robustness of each design choice (against the stated conditions)

- **Gyro bias drift 0.2 deg/s:** bias is a *constant* vector over the 20 s session. It is
  (a) estimated in P0 and subtracted, and (b) removed again by mean-centring before the
  PCA. PCA on the covariance is completely insensitive to a constant offset. Drift (the
  slow part) adds at most ~0.2 deg/s·t to a single axis and is below the noise floor.
- **Gyro noise 0.4 deg/s rms:** PCA/least-squares averages over ~2000 samples. The axis
  is the dominant eigenvector, whose direction error scales like `noise / signal`: with a
  60 deg/s peak the axis error is ~0.02° (measured below). Not argmax of a single sample.
- **Sample rate 476 Hz:** used only to turn `deg/s` into `deg` for the excursion; a wrong
  rate scales the excursion check, not the axis. Use the device `tick_100us` deltas
  (already done in `PoseEstimator`) to be rate-independent.
- **Magnetometer off:** the algorithm never needs heading; it measures the mount relative
  to the *head*, which is self-referenced by the user's own motions. Gravity (accel) gives
  up; the yaw motion corroborates it.
- **Approximate motions (no exact 90°):** the algorithm only needs a *direction* and a
  minimum excursion, never an exact angle. Out-and-back motions with ~60° peak are plenty.
- **Axis ambiguity when two axes are close:** the dominance ratio `lam1/(lam2+lam3) >= 8`
  and the off-axis rms gate catch it; if a motion leaks into another axis the app asks for
  a cleaner try rather than emitting a bad matrix.

---

## 7. Edge cases

| Case | Behaviour |
|---|---|
| Turns at the wrong speed (very slow / very fast) | Fine - axis extraction is speed-independent; only `|w|>5 deg/s` window detection matters. Tested at T=0.8 s and T=4.0 s: max error 0.04°. |
| Accidental roll during a nod | Adds an off-axis component. The polar-decomposition basis spreads it (15% crosstalk -> ~4° basis error; 25% -> ~7°). At 40% crosstalk the `NON_ORTHO` check rejects and asks to retry. The app may also WARN below the hard limit. |
| Near-zero net rotation (jitter, or perfectly symmetric out-and-back) | The PCA still finds the axis (covariance), but `excursion < 25°` fires: "not enough motion detected". |
| Only jitters, never moves | `NO_MOTION` / `SMALL_EXCURSION`. |
| Two axes close in magnitude (e.g. a sloppy turn that is really a tilt) | `dominance < 8` -> `AMBIGUOUS_AXIS`. |
| Head not level while nodding | `NOD_NOT_LEVEL` (>8° off horizontal). |
| Wrong tilt direction | `HANDEDNESS` (>15°) - catches a sign flip on one axis. |
| Big mount tilt (`Z_B` 20° off vertical) | Still fine: up comes from gravity, the other axes from the motions. Tested up to 20° tilt: max error 0.03°. |
| Non-rigid mount / user re-dons glasses | Alignment is per-*wear*, not per power cycle. Offer to re-run in ~20 s; the constants are only valid for that seating. |
| User loses the prompt | Timeout each phase after its window; re-prompt once, then `FAIL("TIMEOUT")`. |

---

## 8. Prototype & measured accuracy

`scratch/calib_design.py` (stdlib only, run with `C:\Python314\python.exe`). It simulates
an arbitrary unknown mount `C = Rz(azimuth) * small_tilt`, generates realistic gyro
(deg/s, bias + 0.4 deg/s rms noise @ 476 Hz) and accel for the three guided motions, runs
the *same* algorithm as above, and compares the recovered basis to ground truth. Output of
the last run:

```
[0] worked example (az=37, tilt=2.5)  geodesic 0.004 deg
[1] single nominal run .......... geodesic 0.01 deg, axes <=0.01 deg
[2] 200 random mounts ........... geodesic mean 0.012  p95 0.022  max 0.027 deg   (0 failures)
[3] nod with 15/25/40% roll ..... 4.3 / 7.0 / rejected(NON_ORTHO) deg
[4] barely moves (18/14/14 deg) .. 100/100 rejected, reason SMALL_EXCURSION
[5] slow (T=4s) / fast (T=0.8s) .. max 0.038 / 0.017 deg
[6] mounts near 0/90/180/270 az .. 50/50 accepted, max 0.028 deg
[7] mount tilt 6/12/20 deg ...... max 0.025 / 0.020 / 0.025 deg
[8] gyro bias 0.2/0.5/0.8 deg/s .. max 0.027 / 0.025 / 0.030 deg
```

Worked-example bake block produced by the prototype (identical to the doc example above):

```
{ +0.79861f, +0.02619f, -0.60128f},
{ -0.60185f, +0.03485f, -0.79785f},
{ +0.00006f, +0.99905f, +0.04359f},
```

**Conclusion.** The procedure recovers the mounting alignment to well under 1° under the
stated noise/bias, tolerates arbitrarily large azimuth and up to ~20° mount tilt, and
degrades gracefully (bounded error under mild crosstalk, hard failure under gross misuse)
with the validation gates above.

---

## 9. Files

- `docs/orientation-calibration.md` - canonical design and runtime contract.
- `src/imu/orientation_calibration.cpp` - validated C++ implementation.
- `src/tools/orientation_calibrate.cpp` - guided live capture tool.
- `src/tools/orientation_calibration_selftest.cpp` - deterministic regression tests.
- `scratch/calib_design.py` - prototype + Monte-Carlo validation (run:
  `C:\Python314\python.exe scratch\calib_design.py`).
- `scratch/calib_run_output.txt` - captured prototype output (if present).
