# AGENTS.md — RayNeo Spatial Workspace

Notes for any agent (or human) working on this repository. Read this **before** changing anything.
It records the invariants, the bug history (so the same mistakes are not made twice), the test map,
and the traps that have already cost time.

---

## 1. What this is

A Windows C++20 / Direct3D 11 application that turns a RayNeo GT pair of AR glasses into a spatial
workspace: N real Windows virtual monitors (default 3 at yaw -45 / 0 / +45) rendered as world-locked
screens, head-tracked in 3DoF from the glasses' own IMU, arranged and controlled from a local web UI.

```
GT IMU (HID, 66/99 protocol, ~476 Hz)
  -> gt_protocol    decode 99 65 nine-axis reports
  -> PoseEstimator  Magdwick fusion + gyro-bias ownership + stillness/constancy gating
  -> ImuSource      worker thread; publishes a coherent pose + telemetry
  -> Renderer       D3D11 flip-model swapchain on the glasses monitor, waitable, quaternion camera
  -> Capture        DXGI Desktop Duplication (GDI fallback) feeding each screen's texture
  -> Vdd            Parsec virtual display driver creates the real Windows monitors
```

Entry points: `spatial_desk.exe` (the app), `orientation_calibrate.exe` (guided sensor-to-head
calibration), `gt_imu_probe.exe` (protocol probe), plus the test binaries below.

---

## 2. Hard invariants — do not break these

1. **Frames.** The fused quaternion is **sensor -> earth, right-handed, Z up** (Madgwick's convention).
   The render world is **Y up, left-handed** (`XMMatrixPerspectiveFovLH`). The camera pose is the
   head-relative rotation conjugated by the earth->render basis, implemented as
   `camera_view_matrix` with the fixed sign triple `(-1, -1, -1)`. Those signs are **load-bearing**:
   flipping any one mirrors the world by up to 180 degrees. There are no user-facing sign toggles and
   there must not be.
2. **Relative rotation is EARTH-frame**: `q_rel = q * conj(q_ref)`. The body-frame order
   (`conj(q_ref) * q`) mixes yaw into pitch/roll whenever the head is tilted at recenter; that was a
   real field bug (up to 16 deg lost pan, 26-34 deg spurious tilt).
3. **Sensor axes.** The package frame is only known through `config/orientation.json`
   (`sensor_to_head`, a proper orthonormal rotation measured by `orientation_calibrate.exe`).
   Never re-introduce a hard-coded `[x, -z, y]` assumption: for the GT the real mounting is
   approximately head-right = sensor X, head-forward = sensor Y, head-up = sensor Z, and gravity must
   map to **+Z of the calibrated frame** (Madgwick's gravity reference). The app refuses to run head
   tracking without a valid calibration file.
4. **One owner for steady error.** The runtime gyro-bias adaptation is the *single* mechanism that
   removes steady error. Drift absorption (subtracting a correction from the published pose) is
   **opt-in and disabled by default** (`drift_rate_cap_degs = 0`). Reason: a correction that nothing
   releases becomes a permanent workspace rotation (see bug 6). If it is ever re-enabled it must keep
   the leak (`drift_leak_tau_s`) and the hard clamp (`drift_limit_degs`). The published pose is
   **always live**: every sample integrates the corrected gyro, and there is no freeze, deadband,
   snap or retroactive quaternion fix anywhere in the estimator.
5. **The bias is bounded.** `bias_limit_degs = 1.5` (the documented plausible bias band) is clamped
   after every adaptation and after calibration. Nothing in the estimator may be able to spin away.
6. **Stale estimates must always be able to recover - and must not leave a reverse slide.** A
   residual the routine update cannot explain (larger than `adapt_residual_cap_degs`) accumulates
   rest time across rest windows; after `adapt_escape_s` of accumulated rest the slow escape may
   follow it. When the escape opens it snapshots the pre-escape bias, and if the observed rate then
   returns within `escape_return_tol_degs` of that snapshot for `escape_return_dwell_s`, the bias
   rolls back to the snapshot in one step - an ambiguous slow turn therefore cannot produce a
   post-stop reverse slide. Rollback confirmation accumulates only during dwell-qualified rest;
   crossing the snapshot band while either the gyro or accelerometer reports motion must reset it.
   While an escape excursion is armed the fast routine update must NOT be
   allowed to undo it (that unwind is the slide this guard prevents); the excursion ends only via
   the rollback or by the escape itself converging on the persistent rate. A rate that persists (the
   residual never returns) is a genuine bias change and is kept. Keep both halves: a stale estimate
   that locks adaptation out produces unbounded creep, while an escape that unwinds after the wearer
   stops is the reverse slide this guard exists to prevent.
7. **Config paths are discovered, not assumed.** `repo_root_from_executable()` walks parent
   directories looking for `config/layouts/default.json`; do not reintroduce fixed `../..` depths.
8. **The app never renders onto a display it captures.** Parsec/VDD screens are excluded from the
   monitor fallback; keep that exclusion.

---

## 3. Bug history — learn from these

| # | Symptom | Root cause | Fix / guard |
|---|---|---|---|
| 1 | "Monitors spinning around me while the crosshair stays centred" | The Z-up fused quaternion was fed to a Y-up renderer as if the frames matched, so head **yaw acted as a roll** | Exact quaternion basis transform in `camera.cpp`; `camera_selftest` |
| 2 | World swung/tumbled when turning with a tilted head | Relative rotation built in the **body** frame instead of the earth frame | `q * conj(q_ref)`; `pose_selftest` tilted-recenter case |
| 3 | "It spins even when I'm still" (slow creep) | The refinement gate used the *corrected* signal, so a large initial bias error kept it closed forever (up to 379 deg/min in simulation) | Variation-based stillness, rate caps, still-gated calibration, continuous adaptation |
| 4 | Micro head movements felt unregistered, then jumped | A hard **freeze while still** discarded everything below its release threshold | Freeze removed entirely: the pose path is always live and rest only gates *adaptation*. `pose_scenarios` scenario 12 publishes >=90% of 0.5/1.0/2.0 deg adjustments with no sample step > 0.05 deg |
| 5 | Centre screen off-centre after panning out and back | The old bias adaptation used a permissive **raw**-rate gate, so slow pan settling was absorbed into the bias | The original tighter raw-rate gate was later superseded by measured deviation dwell, post-motion hold-off and a 0.35 deg/s residual cap. The current estimator has no separate raw-rate adaptation gate; do not restore the historical value from this row. |
| 6 | The whole workspace slowly rotated until the left screen was almost centred (~10 min) | The drift **correction** and the bias adaptation removed the same error; the correction (owning the integral) was never released, and it also swallowed post-pan accelerometer settling (3.87 deg/min measured in the field) | Single owner (bias); absorption off by default; leak (tau 20 s) + 2 deg clamp if re-enabled |
| 7 | Gating bug: my "corrected-rate" gate was mathematically a *lag* test; a gently ramping rotation passed it, inflated the bias, then locked adaptation out (runaway to -117 deg) | Gate on RAW rate, hard bias clamp, escape valve |
| 8 | Calibration tool aborted at stream-on although the glasses had accepted the command | hidapi on Windows returns `length + 1` for a successful write to this device; the code compared it to `length` | `write_report` returns the raw value; command flows use `send_command_verified` (waits for the `99 c8` ack) |
| 9 | Calibration could not complete ("SMALL_EXCURSION", "HOLD_STILL") | 25 deg excursion gate + 5 deg/s motion detection + a motionless-window still requirement that a worn head cannot satisfy; failures also hid their own diagnostics | 12 deg excursion, 2.5 deg/s detection, quietest-1.5-s still window, diagnostics populated on failure, per-step retries |
| 10 | Capture policy implemented nowhere, GDI fallback churning objects, transient errors killing the app, VDD rollback gaps | See `docs/PROTOCOL-NOTES.md`; fixed in commit `caeef11` |
| 11 | Sub-degree head adjustments disappeared or arrived as a snap, and a slow steady turn left a visible reverse slide as soon as it stopped | The pose froze while "still"; the escape then released the absorbed rate with the fast tau once the raw rate dropped, unwinding the published turn | Always-live pose path; routine updates bounded by `adapt_residual_cap_degs`; escape snapshot + guarded rollback (`pose_scenarios` 11/12/13, `pose_selftest` contiguous-calibration and accel-blocked-rest cases) |
| 12 | A moving rate crossing or an IMU packet gap could validate stale rest state | Rollback dwell did not require qualified rest; a >=50 ms sample gap preserved calibration/rest/absorption history | Rollback dwell is rest-gated and resets on motion; gaps invalidate the startup window, rest dwell, rollback dwell and prior live increment (`pose_scenarios` 14 and timestamp-gap selftest) |
| 13 | Centre screen followed the head and all diagnostics stayed at zero | Startup never finished: the synthetic 0.15 deg/s noise model produced 0.25/0.5 deg/s calibration/rest gates, but measured GT stationary deviation is 0.75 deg/s median and 1.37 deg/s p99 | Hardware-measured 1.5 deg/s gyro deviation gates, separate unchanged 0.35 deg/s runtime residual cap, realistic-noise startup regression, and stream-rate/deviation telemetry during calibration |
| 14 | Takeover left laptop windows minimized/invisible; each run stranded more (record found 21, then 3, then 0 nondeterministically); restored windows landed at virtualized coords, off-home | Windows 11 minimize-on-disconnect hides windows by stale display-association on both legs; post-connect record ran after connect shoved the laptop aside (pristine home matched nothing); unaware-process DPI virtualization lags primary changes (mixed virtual/physical frames) | Pre-connect MigrationSeed record; settle-to-normal mover (no iconic skip, SetWindowPos lies on iconic windows); repair-until-quiet loops on both legs with off-screen-normal-rect override for missed victims; per-monitor awareness at process start (`vdd_selftest` migration math; `scratch/live-migration3.ps1` live PASS 22/22, witness home-exact) |
| 15 | Mid-session display-settings change broke the shutdown restore (BADMODE -2 on the laptop pin); later starts churned a full takeover then exited (glasses gone) | GDI device names renumber across churn, so name-keyed pins staged modes on the wrong display; snapshots can capture a transient/VRR rate; no pre-takeover render-target check | Pins resolve by stable monitor id with name fallback, BADMODE retries once without the rate, workspace mode pre-flights the glasses before touching anything, attach/home errors split (resolve/strip unit checks) |
| 16 | Every escape from workspace mode unhid the user's auto-hide taskbar | Display churn makes Explorer recreate the taskbar, which drops the auto-hide preference; the app never snapshotted it | Takeover captures ABM_GETSTATE into `WorkspaceTopology.taskbar_state`; the restore re-applies it (verify-and-retry, best-effort, never fatal) and the engine logs when it did (`vdd_selftest` read-only state check; live round-trip probe 1->0->1 PASS) |
| 17 | "Glasses display was not found" (exit 1) on every start after the user picked "Disconnect this display" on the glasses in Settings | The engine only scans *active* monitors, so a connected-but-detached display is invisible; the disconnect persists in the display database across starts | Pre-flight and post-takeover recovery: `find_detached_glasses_display` (EDID of detached adapters) + two-phase `reattach_detached_glasses` (attach at an overlap-free slot, then best-effort move home; the contract is attached, not placed). Unbranded-TCL-as-primary can never match the wrong panel. Measured live: /internal repro NOT-FOUND -> reattach OK -> FOUND at auto-placement, move home LANDED at (442,-1080); overlapping staged slot is auto-relocated by Windows ((0,0)->(-1920,0)), a free slot is honored. `vdd_selftest` matcher table + detached-really-detached invariant |
| 18 | Recovery still missed: takeover-time disappearance (pre-flight passed, glasses gone post-takeover, one-shot find silent); taskbar still unhid on escape | A recalled disconnect lands *asynchronously* after the takeover, so the single-shot recovery fired while the adapter was still flagged attached; Explorer recreates the tray seconds after the restore, flipping auto-hide after a set-and-check passed | Recovery runs on every pass of the 15 s post-takeover wait; both failure sites dump `describe_display_landscape` (GDI adapters + named inactive QDC paths: flap vs deactivation). Taskbar re-apply requires 3 s steady within 15 s and both shutdown paths log captured/before/after (`vdd_selftest` landscape check; dump verified live) |
| 19 | Landscape dump showed the deeper disconnect (no TCL EDID anywhere in GDI; SmartGlasses only as inactive CCD paths) and exposed two lifecycle holes: the engine's guard restore is terminated mid-run on quit (8 s grace vs 13-35 s restore), and the controller auto-recovers under a live foreign engine | Recalled disconnect nukes the GDI EDID association; controller quit path never re-applied the taskbar; Failed-branch recovery races a live process | `reactivate_glasses_path` (stored-then-preferred modes, endpoint-mapped strict verify, saved rollback) wired into pre-flight, wait loop and controller Start; controller owns taskbar last word (capture at start, track-while-Running, re-apply on recovery/exit + 10 s sticky poll); Failed-branch recovery gated on process exit with deferred retry; fallback moved outside the wait loop (review-verified: 8 BUGs + 7 CONCERNs triaged, 12 accepted; live 30s run: disappearance recurred and CCD reactivation fired mid-takeover, 6/6 criteria PASS; mid-session taskbar flip caught and re-applied, captured=1 before=0 after=1) |
| 20 | "Nodding only tilts the screens" (no visible pitch) | Two stacked causes, both telemetry-verified: the calibration file carries the wearer's ~4 deg habitual nod lean (M[1][0]=0.07, same magnitude across two calibrations but varying in direction, so no static file cancels it), coupling ~7% of every nod into roll; and the default centre screen exactly fills the view (46.0x26.8 deg vs 46x26.9 FOV), hiding the correct dominant pitch slide while the small roll glares | Default screens shrunk to 1.6x0.9 m (visible edges, per-user approval); fresh calibration aligns the file with current habit; tool prints pairwise axis quality (`[cal] quality:`); nod against a vertical edge. View/estimator math proven exact (10-agent review + log forensics). Flat-surface calibration explicitly rejected: with no head there are no head axes (Standing lesson 12) |
| 21 | "Engine failure" on every quit; engine.log ends mid-restore | Controller stop grace (8 s) shorter than the display restore it must wait out (13-35 s measured): quit always TerminateProcess'd the engine mid-restore (stranded virtual desktops + bogus Failed state). Bug 19 had only papered over the taskbar aftermath | Stop grace 8 -> 45 s, quit wait 12 -> 60 s (message-pumped; the kill stays as a last resort) |
| 22 | Centre screen off-centre after looking around (reported as ~5 min recenter need) | Adaptation starvation: the symmetric 0.5 s deviation EMA took ~2.5 s to drain after a pan, so 1-2 s look-around holds never reached eligibility; the bias sat frozen for 80 s while true bias wandered, walking the centre off ~1.5 deg per 20 look-arounds (scenario 18 failed 1.44 deg pre-fix) | Asymmetric dev EMA (rise 0.5 s, fall 0.08 s): eligibility recovers ~1 s after a pan, noise-flicker behaviour unchanged (scenario 18: 0.62 deg; scenario 16 nominal: 0.28 -> 0.04 deg; full suite green) |
| 23 | "Looking up tilts the screens right, looking down tilts them left" - only after a recenter, never at startup | `q * conj(q_ref)` with a reference carrying earth heading psi rotates every head axis by psi, so a nod became sin(psi) roll. Startup references come from the accelerometer (psi = 0), hence fine until the first recenter. Field telemetry 2026-09-25: nod-to-roll coupling 2 deg before recenter, 14.5 / 17.9 / 27.4 deg after successive recenters | Reference split `q_ref = H*T` (heading twist about earth Z, tilt remainder); publish `conj(H)*q*conj(T)`. Earth yaw stays pure yaw (bug 2 guard kept). `pose_selftest` nod after +40 deg headed recenter: axis error 39.98 -> 0.002 deg |
| 24 | Workspace drifts after turning the chair / slow pans (54 deg/min after one turn, replayed) | Rest was judged from gyro *fluctuation* only; a smooth steady turn at ~15 deg/s has low deviation, qualified as rest, and the escape walked the yaw bias 0.43 -> 1.5 (clamp) -> -0.49 deg/s | Raw-rate ceiling on rest (3 deg/s, motion above 4; the in-band bias norm bound 2.6 + margin, a RAW gate so it cannot lock out; exempt during startup calibration). `pose_selftest` two 360 deg turns out and back: 5.23 -> 0.52 deg; replay of the recording keeps bias at 0.43 |
| 25 | Small-text reading still swam with the 1-euro on; any sub-0.04 deg smoothing decision was blind | `2 * acos(w)` in float cannot resolve rotations below ~0.04 deg (w rounds to 1), so the 1-euro speed estimate moved in 2.4 deg/s steps at 60 Hz; separately the frame-start pose was rendered after the capture pass and the swapchain wait, and the smoother had no hold at all (31 px/s text motion while reading, replayed) | `quat_angle_deg` (atan2 of the vector part); late pose sample after `wait_for_frame()`; reading hold (soft hysteresis deadband, tanh knee, settle leak) with off/low/medium/high presets, default medium, Ctrl+Alt+S cycles live. `pose_scenarios` 19: 0.01 deg measures 0.010000; reading sway 0.654 -> 0.090 deg; off == plain 1-euro. Replay: 31.0 -> 6.7 px/s |

**Standing lesson:** the IMU path is where the subtle bugs live. Every gating change must be
accompanied by a synthetic scenario that fails before and passes after, and every claim in a commit
message must be reproducible from the test output.

---

## 4. Known limitations (state them, do not hide them)

- **Magnetometer (corrected 2026-09-25).** The old conclusion "heavily distorted field" was wrong.
  The mag axes are rotated 90 deg from the package (`package = (my, -mx, mz)`, `kMagToPackage`)
  and carry a ~25 uT hard-iron offset from the glasses themselves; the raw 71 uT was offset plus
  the ~51 uT earth field, and feeding unrotated raw axes caused the 4.8 deg/s spin. Calibrated
  (`orientation_calibrate --mag`, gyro-constrained linear fit) the field is 51.5 uT at 66.7 deg
  dip and its heading tracks the gyro within ~3 deg over 360 deg turns. `MagHeadingLock` (yaw-only,
  rate-limited 0.5 deg/s, disturbance-gated PI, tau 10 s) pins yaw to the local field when a mag
  calibration exists. Remaining limits: a fixed local distortion is harmless (only direction
  stability matters), but a field that changes while you work (a magnet, a moving laptop lid
  near the head) is gated out rather than corrected; a moving vehicle rotates the earth frame
  for gyro and mag alike. Without a mag calibration the lock is off and the paragraph below
  applies unchanged.
- **Gyro-only yaw (no mag calibration).** Consequence: with no absolute heading reference a *steady*
  slow yaw rotation is physically indistinguishable from a yaw bias. Every yaw gate is therefore a
  trade-off; the current choice favours the wearer's pan: the escape is slow enough that a 20 s /
  1.0 deg/s turn keeps >=80% of its travel, and its snapshot rollback removes the small absorbed
  part when the turn stops (no reverse slide). A genuinely stale estimate therefore converges over
  tens of seconds, not seconds. Startup tracking stays closed until one valid bias window is
  available; `R` / Ctrl+Alt+R recenters after an unavoidable ambiguous ultra-slow turn.
- **Bias band.** The estimator assumes |bias| <= 1.5 deg/s. A larger true bias cannot be corrected and
  would show as creep; better to recalibrate.
- **Virtual displays need the signed Parsec VDD driver**, which is not installed by default. Without
  it run with `--no-virtual-displays` (labelled test screens).
- The glasses monitor must be in **Extend** mode (duplicate/PC-screen-only makes it invisible to the
  app and breaks the geometry).

---

## 5. Drift, bias and the yaw ambiguity (read before touching the estimator)

The estimator has exactly **one** mechanism for steady error: the gyro-bias adaptation. Drift
absorption is opt-in and the published pose is always live. The parameters are not arbitrary - each
one was chosen against a measured failure, and the trade-offs below are **physical**, not
implementation bugs. Do not "fix" them by loosening them; if you change one, re-measure all of the
scenarios and update this table.

| Parameter | Value | Why this value |
|---|---|---|
| `warmup_s` | 4.0 | Live startup logs show the GT bias still settling after 2 s. Nothing reaches the rest detector or fusion during this discard period (a floor: `settle_samples` still applies). |
| `calibration_window_s` / `bias_samples` | 1.0 s / 600 | One *contiguous* high-confidence rest window. At ~476 Hz the 600-sample floor is binding (~1.26 s). A window broken by motion is discarded outright: a contaminated startup bias used to gate out the very adaptation that could correct it. |
| `calibration_timeout_s` | 10 | Diagnostic epoch only: on timeout the app keeps waiting and tracking stays closed. Publishing from a zero/fragmented bias caused visible startup settling, so invalid data is never accepted merely to start sooner. |
| `calibration_gyro_dev_degs` / `calibration_accel_dev_mps2` | 1.5 / 0.3 | The worn-glasses still capture measured 0.75 deg/s median and 1.37 deg/s p99 gyro deviation; the former 0.25 gate never completed on real hardware. Accelerometer deviation measured ~0.05 m/s2 p99, so its tighter gate remains valid. |
| `rest_gyro_dev_degs` / `rest_accel_dev_mps2` | 1.5 / 0.5 | Enter gates for continuous rest. Deviation is the Euclidean vector magnitude, so a proper sensor-to-head rotation cannot change the classification. The accelerometer half stops a quiet gyro on a shaken package from counting as rest. These noise gates only qualify rest; the separate 0.35 deg/s residual cap controls runtime bias authority. |
| `motion_dev_threshold_degs` / `motion_accel_dev_mps2` | 3.0 / 1.0 | Exit gates: crossing either resets the rest dwell and starts the motion hold-off. The gyro exit is above measured stationary peaks; real head-turn ramps and/or accelerometer motion still trip the refractory path. |
| `still_hold_s` | 0.5 | Continuous dwell. Between the enter and exit gates the dwell neither advances nor resets, so a mild periodic tremor can still accumulate rest. |
| `dev_ema_tau_s` | 0.5 | The raw deviation has to be smoothed: unfiltered it flickers on gyro noise and the dwell would never complete. |
| `adapt_residual_cap_degs` | 0.35 | Routine adaptation may only chase a residual this close to the estimate. The tighter 0.2 value failed the breathing-plus-bias envelope; larger errors go through the guarded slow escape. A steady yaw inside this band remains physically ambiguous without an absolute heading reference. |
| `bias_slew_degs_per_s` | 0.15 | Bounds how fast the estimate may move, so no update path can jump it. |
| `motion_holdoff_s` | 1.0 | Long enough to skip the ramp/settle of a deliberate movement, short enough that correction resumes promptly. |
| `bias_adapt_fast_tau_s` | 1.0 | Used by the routine residual update. |
| `adapt_escape_s` | 8 | Accumulated rest (across separate rest windows) with an unexplained residual before the slow escape opens; a wearer who moves periodically can no longer lock a stale estimate out. |
| `bias_adapt_slow_tau_s` | 40 | The escape must be slow enough that a 20 s / 1.0 deg/s ambiguous turn keeps >=80% of its travel (scenario 11). Cost: a genuinely stale runtime estimate converges over tens of seconds, not seconds. |
| `escape_return_tol_degs` / `escape_return_dwell_s` | 0.2 / 0.2 | The rollback trigger: the observed rate must return to the pre-escape snapshot's band for a continuous, dwell-qualified rest interval. Motion resets confirmation. While the excursion is armed the routine update is blocked, so the estimate can only return to the snapshot through this one-step rollback, not through a tau-1 unwind. |
| `drift_rate_cap_degs` | 0 | Absorption off: it was the source of the 3.87 deg/min workspace rotation. |

**The ambiguity, stated plainly.** With the magnetometer disabled there is *no* absolute heading
reference, so a steady 1.0 deg/s yaw rotation and a 1.0 deg/s yaw bias are the *same measurement*.
`pose_scenarios` scenario 6 ("a steady 0.5 deg/s rate for 30 s is a bias") and scenario 11 ("a
steady 1.0 deg/s rate for 20 s is motion") are therefore contradictory by construction; no estimator
can satisfy both. This design favours the pan, because the escape is deliberately slow: scenario 11
asserts that at least 80% of the 20 s turn is published, that post-stop reverse motion stays below
0.25 deg (the snapshot rollback), and that the final offset stays bounded. Scenario 13 asserts the
other half: a persistent +1.0 deg/s bias step is learned (not rolled back) and the pose stops
creeping once it converges.

**Measured envelopes (RelWithDebInfo, 2026-09-21).** The assertion is the enforced contract; the
measured value is printed by the current tests and must be refreshed whenever estimator constants
change.

| Behaviour | Measured | Assertion |
|---|---:|---:|
| Always-live micro adjustments, 0.5/1.0/2.0 deg (scenario 12) | 98.8% / 99.5% / 99.7% outbound; max sample step 0.0216 deg | >= 90% outbound and return; step < 0.05 deg |
| 1.0 deg/s for 20 s then hold (scenario 11) | 89.0% at stop; 0.125 deg reverse; 2.318 deg final offset; rollback fired | >= 80%; reverse < 0.25 deg; offset < 4 deg; rollback |
| Genuine +1.0 deg/s bias step (scenario 13) | estimate 0.915 after 90 s; zero rollbacks; last-10-s drift 1.153 deg | > 0.7; zero rollbacks; drift < 3.0 deg |
| Pan-and-return +/-60 deg (scenarios 1/2) | +0.082 / -0.182 deg final yaw | abs(final) < 0.3 deg |
| Slow pan 10 deg at 2 deg/s (scenario 4) | 99.9% registered | >= 80% |
| Fast pan 120 deg at 120 deg/s (scenario 5) | 100.0% registered | >= 95% |
| Residual 0.5 deg/s bias, 30 s (scenario 6) | 0.028 deg published drift | < 0.5 deg |
| Breathing + 0.5 deg/s bias, 30 s (scenario 7) | 0.232 deg half-envelope | < 0.4 deg |
| Band-limit 1.5 deg/s bias, moving wearer (scenario 10) | 29.2 deg bounded final offset | < 40 deg (pre-fix: 90 deg/min, unbounded) |
| 20 pan-and-settle cycles, 0.10 deg/s residual | 0.331 deg offset | < 2.0 deg |
| Absorption enabled (opt-in), 20 pan cycles | 0.927 deg with leak; 1.667 deg without | < 3.5 / 5.0 deg |
| Measured GT stationary noise at startup | calibration completes; bias error 0.002 deg/s; pose publishes | calibration and publication must open within 7 s synthetic run |
| Fragmented startup quiet (`pose_selftest`) | no completion, bias stays 0; one contiguous window completes and matches the truth |
| Timeout with only fragments | tracking stays closed, `calibrated()` false, bias stays 0; a later contiguous window completes |
| Accel shake 4 m/s^2 @ 2 Hz with a quiet gyro | rest blocked while shaking, returns after it stops |
| Bias clamp | abs(bias) <= 1.5 deg/s at startup and after runtime adaptation |
| Moving crossing of the escape snapshot band (scenario 14) | zero rollbacks; persistent estimate survives |
| >=50 ms IMU gap during startup | pre-gap and post-gap samples never form one calibration window |
| Diagonal 0.3/0.3/0.3 deg/s residual for 5 s | bias norm < 0.1 deg/s (vector gate, not per-axis gate) |
| Display 1-euro smoothing, 8 Hz tremor (scenario 15) | raw 0.124 deg p2p -> smoothed 0.040 deg; converge 0.004 deg; 30 deg travel survives | ratio < 0.5; residual < 0.06 deg; converge < 0.1 deg |
| 5-min computer use, net-zero yaw (scenario 16) | 0.038 deg drift (0.285 pre-fix); bias err 0.023 deg/s; adapt duty 77% | drift < 3.0 deg |
| 20 look-arounds and back (scenario 18) | 0.62 deg final (1.44 pre-fix) | abs(final) < 1.0 deg |
| Reading hold, medium preset (scenario 19) | 0.08 deg / 0.4 Hz sway: 0.654 -> 0.090 deg text motion; turn catch-up 0.017 deg; settle 0.029 deg | < 30% of 1-euro; < 0.05 deg; < 0.05 deg |
| Ultra reading hold (scenario 19 + worn session 2, 266 s of reading) | sway 0.3 deg: medium 0.536 -> ultra 0.058 deg; catch-up 0.028 deg; 60 s settle 0.065 deg. Replay: high 5.4 -> ultra 2.3 px/s, p95 step 0.009 -> 0.003 deg, turn lag 1.70 -> 2.59 deg rms. Past a 1.0 deg inner radius gains flatten (1.5/2.5: 1.7 px/s, 3.4 deg lag) | < 50% of medium; < 0.05; < 0.1 deg |
| Reading hold on the worn 2026-09-25 session (`imu_replay --smooth-eval --stabilise N`, 235 s of reading) | off/low/medium/high: 31.0 / 11.1 / 6.7 / 5.2 px/s text motion; p95 frame step 0.025 / 0.015 / 0.010 / 0.008 deg; turn lag rms 1.18 / 1.40 / 1.57 / 1.71 deg | measured, not asserted |
| Mag lock lag on the same session (10 s mean of the lock error) | tau 20: rms 1.07, p95 1.77 deg; tau 10: rms 0.59, p95 0.86 deg; tau 6: 0.44 / 0.55 deg; reading jitter 31.0 px/s at all three | measured, not asserted |
| Noiseless pan-return closure (scenario 17) | +/-0.025 deg final | abs(final) < 0.05 deg |

**Lessons from this round, in the order they were learned:**
1. A safety mechanism that is never *released* becomes a permanent error (bug 6).
2. Removing one mechanism's authority exposes the next one's limits - the pan loss only became
   visible (scenario 11) after absorption was disabled.
3. Fixing a lock-out by widening a gate can silently trade it for over-absorption somewhere else;
   the fix must be measured against *all* scenarios, not just the failing one.
4. A new algorithm is not "better" just because it fixes the case it targeted: the episode-gated
   scheme fixed scenario 11 and broke scenarios 6, 7 and the pan-and-settle accumulation, and was
   reverted. Keep the older, measured behaviour unless the replacement wins everywhere.
5. A plausible-looking regression test can encode a contradiction. Scenario 10 and scenario 11
   are the same measurement with opposite labels - that is a fact about the sensor, and it belongs
   in the output, not hidden behind a loosened limit.
6. Test assertions must name the property that matters: "sink during the hold" flagged an absorbed
   rotation unwinding (expected) instead of a permanent offset (the actual defect), so it was
   re-expressed as the net offset.
7. A guard that reacts to the *return* of a signal, not just its presence, can tell an episodic
   rotation from a persistent bias: snapshot the state before acting and roll back only after the
   observed rate has returned to the snapshot's band for a confirmation dwell.
8. A frozen pose hides sub-degree motion entirely. Always-live integration plus a rest detector that
   gates *adaptation only* is strictly better for micro-adjustments (scenario 12).
9. Calibration should discard data rather than use bad data: wait for one contiguous window even
   across diagnostic timeouts. Publishing from zero or averaging fragments can bake visible startup
   drift into the single bias owner.
10. "Locally bounded" is a property of the update, not just of the clamp: only chasing residuals
    close to the current estimate keeps a deliberate slow yaw out of the bias while still tracking
    thermal drift.
11. Rest evidence is invalid across missing samples or detected motion. Reset qualification and
    rollback confirmation at those boundaries; never stitch apparently quiet fragments together.
12. Smoothing has a direction: a symmetric tail that is correct for noise can still starve recovery after real motion. Make only the falling edge fast - the rising edge keeps the flicker protection.
13. A faithful renderer can still *look* wrong: a view-filling screen hides the dominant motion
    axis while a small cross-coupling glares. When the logs say the pose is right but the eyes
    disagree, check (a) the calibration file's off-axis terms against fresh telemetry and (b)
    whether the layout leaves any visual margin. Never calibrate off-head: head axes exist only
    on a head.

## 6. Test map and how to run

Private build directory per agent (never share `build/`, and never use `--preset default` when other
agents may be building — it uses the shared `build/`):

```
cmake -S . -B build/agent-X -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake"
cmake --build build/agent-X
ctest --test-dir build/agent-X --output-on-failure
```
Run inside the MSVC environment: `call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"`
(CMake's Visual Studio generator cannot find this Build Tools install; Ninja is required.)

| Suite | Covers |
|---|---|
| `camera_selftest` | Frame conversion, the sign triple, single-axis and combined orientation cases |
| `pose_selftest` | Tilted-recenter coupling, contiguous-vs-fragmented startup calibration and its timeout, accelerometer-blocked rest, bias clamp, reconfigure resets, swing/twist helpers, drift-absorption behaviour, 20-cycle pan-and-settle accumulation |
| `pose_scenarios` | Nineteen synthetic worn-head scenarios through the real estimator: pan-and-return both ways, small moves, slow and fast pans, residual bias, breathing, diagonal, recenter, the 1.0 deg/s ambiguous turn and its rollback guard (11), always-live micro adjustments (12), persistent bias step (13), and motion-gated rollback crossing (14), display-side smoothing (15), 5-min session drift (16), noiseless closure (17), repeated look-around walk-off (18), and the reading hold plus small-angle precision (19) |
| `orientation_calibration_selftest` | Guided-calibration maths, rejection gates, file round-trip, version handling |
| `protocol_selftest` | 66/99 framing and `99 65` decoding |
| `layout_selftest` | Layout parsing/validation, geometry, capture-policy constraints |
| `vdd_selftest` | Parsec VDD protocol, cleanup order, index parsing |
| `view_selftest` | Offline "virtual glasses": real layout + real camera maths projected to NDC, numeric assertions, and 640x360 PPM frames in `scratch/` |
| `app_selftest` | Controller app model: engine command contract, status protocol + freshness gate, preference validation/persistence, presets, field normalisation, 3D face selection and view-plane drag, depth slider scale, per-session log archive (10-min gate, rolling three, real tar round trip), telemetry tail, view comfort (ranges, engine switch round-trip, tint/focus/fade maths, app.json persistence) |
| `mag_heading_selftest` | Magnetometer heading lock closed loop (stale bias bounded, integral removes lag, rate limit, disturbance rejected with zero view motion, new environment re-acquired without a jump) and the gyro-constrained hard-iron fit (recovery + rejection) |

Every fix MUST come with a regression test that fails without it. Synthetic IMU scenarios live in
`pose_scenarios.cpp` (`HeadSim` helpers) and `pose_selftest.cpp` (local helpers).

---

## 7. Traps that have already cost time

- **hidapi write return**: never compare to `length`; use `send_command_verified` when a command must
  actually be confirmed.
- **Backgrounded app stdout is buffered away**: run `spatial_desk` in the foreground or use `--log`.
- **`;` is not a cmd separator** in `shell_command` (that runs under cmd.exe): use `&&`.
- **PDB conflicts**: concurrent `cl.exe` runs in the same build dir fail with C1041; that is why each
  agent needs its own `build/agent-*` directory.
- **The diagnostics CSV appends** (`--log`), so multiple runs concatenate; the `elapsed_s` column
  restarts per run.
- **PowerShell here is 5.1**: no `&&`, no ternary, no `call`; and the multi-line patch scripts under
  `scratch/` are the reliable way to make surgical edits with exact strings.
- **Permission model**: agents may write only inside this repository and may run builds/tests; do not
  install packages, drivers or system tooling, and do not modify anything outside the repo.
- **The glasses' display path is flaky**: it appears/disappears; `Win+P -> Extend` restores it.

- **File locking on Desktop**: `LNK1168` (cannot open exe), `LNK1201` (program database) and
  `C1083`/`C1041` (obj/pdb) errors here are a file scanner, not a code problem. `del` the exe and
  pdb and relink, or configure a build directory outside the Desktop tree
  (for example `-B %TEMP%/rayneo-build`), which has never failed.
- **Windows PowerShell 5.1 quoting in patch scripts**: a double backslash inside a single-quoted
  string is literal, so writing a C++ `\n` through `\\n` in a patch script produces a *literal*
  backslash-n in the output (this defect shipped twice). Verify by reading the compiled string back
  out of the test output.
- **The IMU suites are MSVC-only**: scenario 10 passes on MSVC (29.2 deg) but runs away under g++ (90.6 deg) - its escape/rollback thresholds are FP-chaotic across compilers, so g++ numbers are diagnostic only and the contract suite stays `RelWithDebInfo` on MSVC.
- **Replay real sessions**: with `--log` the engine writes `logs/imu_raw.csv` (every sample);
  `imu_replay FILE --orientation config/orientation.json [--observe] [--no-mag]` runs it through the
  real estimator. Measure estimator changes on recorded hardware data, not only on synthetic scenarios.
  `--stats` prints per-interval gate duty (rest/still/routine/escape); `--smooth-eval` replays the
  display smoother at 60 Hz (`--stabilise N`, `--hold inner,outer,tau`, `--min-cutoff`, `--beta`);
  `--lock-tau` / `--lock-ki` retune the heading lock. In observe mode the lock's `err` is exactly the
  gyro-only heading drift, which is how the 2026-09-25 session showed ~20 deg of thermal gyro drift the
  lock removed.
- **Measure small rotations with `quat_angle_deg`, never `2 * acos(w)`**: in float the latter reads 0 or
  0.0396 deg for anything below ~0.04 deg (bug 25).
- **Two tar.exe on this machine**: Git Bash's GNU tar is first on PATH and `tar -a -c -f x.zip`
  silently writes a *tar* stream under a .zip name. Windows' bsdtar (`%SystemRoot%\System32\tar.exe`)
  writes a real deflate zip (`--format zip`). The session archiver calls it by absolute path.
- **Session logs**: the controller clears `logs/` at each launch and archives sessions over 10 min
  into `logs/sessions/` (newest three). Replay an archived `imu_raw.csv` after extracting it.
- **Regexes over replay output**: `t=` also matches inside `adapt=0`; anchor on ` t=`. A lag metric that
  reads exactly 0.000 is a parser bug, not a perfect loop.
- **Shell heredocs eat backslashes here**: a C++ `\n` written through a bash heredoc into a Python
  patch script lands as a real newline and splits the string literal. Use the editor tool for any
  edit containing escapes.
- **Never trust a redirected build**: `cmake --build ... > nul` hides `FAILED:` lines and you end up
  testing stale binaries whose assertions no longer match the source. Always let the build print.

## 9. Improvement-loop log, 2026-09-23 (agent session)

Reversible session: backup branch/tag `backup/pre-improvement-2026-09-23` (verified) plus one local
commit per feature on `main`; `git revert` any commit to undo it. All builds MSVC Ninja
`RelWithDebInfo`, 9/9 `ctest` green at every commit; glasses mostly disconnected (heat), so all
verification is synthetic plus field-CSV forensics.

| Commit (short subject) | What changed | Evidence |
|---|---|---|
| fix-pitch-hold-freezes-tilt-instead-of-pitch | Pitch-hold twist axis `(0,1,0)` -> `(1,0,0)` (it froze tilt, not pitch) | scenario review + suite green |
| cal-tool-print-pairwise-axis-quality-metrics | Calibration tool prints axis quality | tool output |
| default-screens-visible-edges-plus-bug20-notes | Visible screen edges by default; nod-as-roll diagnosis notes | view_selftest |
| quit-grace-covers-display-restore-45s | Quit grace 8 s -> 45 s (restore needs 13-35 s) | bug-19 lifecycle finding |
| user-named-layout-presets-triple-plus-ultrawide | Named presets: `triple` + `ultrawide` ship, user can add more | layout_selftest + app_selftest |
| hover-help-bubbles-for-every-setting | `?` tooltip beside every setting (effect of increase/decrease) | app_selftest |
| fullscreen-orbitable-3d-arrangement-view | Fullscreen 3D monitor arrangement editor with mouse orbit | view_selftest + app_selftest |
| display-side-1-euro-view-smoothing-plus-scenario15 | `PoseSmoother` (rotational 1-euro) between pose and renderer; `--no-smoothing` opt-out; diagnostics stay raw; recenter resets | scenario 15: tremor 0.124 -> 0.040 deg, converge 0.004 deg |
| scenarios-16-17-five-minute-drift-plus-noiseless-closure | 5-min session bound (< 3.0 deg) + noiseless closure bound (< 0.05 deg); no estimator change | scenario 16: 0.285 deg; scenario 17: 0.025 deg |
| fix-look-around-walkoff-asymmetric-dev-ema-plus-scenario18 | Starvation fix (dev EMA fall 0.08 s) + scenario 18 | 18: 1.44 -> 0.62 deg; 16: 0.28 -> 0.04 deg |
| revert-screen-size-to-original-1.7m | User verdict: original screens better than shrunk 1.6 m | default + triple preset, suite green |
| ui-minimalist-help-marks-plus-roomy-spacing | 24 px help chips -> 16 px faint glyphs; wider spacing rhythm throughout | 9/9 green |
| ui-draggable-splitters-between-sections-persisted | 5 persisted splitters (4 row + column divider), live drag, minimums kept | selftest round-trip, 9/9 green |
| ui-splitter-edge-cases-plus-clamp-guards | Legacy/malformed/degenerate prefs tests; pin boundaries when shrunk below minimums | 9/9 green |

Pin-drift investigation (row 22 above): exonerated the estimator by measurement rather than shipping a
guess. Reverted fixes: shorter still/holdoff gates (harsh drift 16.0 -> 27.6 deg: learns motion into
the bias) and raw-rate routine target (tremor residual 0.040 -> 0.074 deg: couples tremor through the
flickering gate). Standing addition: the LPF routine target is load-bearing against tremor, and the
gates are load-bearing against motion pollution - both were re-proven tonight.

Pin-drift fix (row 22): the user confirmed centre-off-after-looking-around; reproduced in sim
(scenario 18, 1.44 deg), root-caused to adaptation starvation, fixed with the asymmetric dev EMA
(0.62 deg), and field-verified by the user ("so much better", routine duty 6% -> 21%).
Round 2 (no ship): holdoff 1.0 -> 0.7 (18: 0.62 -> 1.23 deg), smoother retune (bad lag trade),
and a routine fast-lane with raw-rate gate (scenario 7: 0.23 -> 2.71 deg) all failed - the tuning
sits exactly on the ridge; every authority widening was reverted. Estimator declared at its floor.
The earlier session attributed remaining field scatter to aim/recenter/slip without a live
confirmed event; the later observation below supersedes that certainty. Checkpoint branch
`checkpoint/verified-lookaround-fix-2026-09-23` marks the verified state.

### Later live drift observation (2026-09-23, 21:13 BST)

The wearer reported another centre/right shift and confirmed the recenter at engine elapsed
2362-2364 s. The live CSV was local, ignored evidence and has since been discarded; the figures below are what it showed.
During elapsed 2304-2359 s, view yaw changed about +1.21 deg while the glasses reported
dwell-qualified rest for about 98% of samples and routine bias adaptation for about 55%.
Projecting raw gyro minus the logged bias through this session's `sensor_to_head` matrix and
integrating at the CSV's sampled cadence predicts about +1.10 deg of camera yaw. No escape
rollback fired and drift absorption was zero. This rules out a separate accumulated offset in
the camera, renderer or display smoother: the view is following the integrated corrected gyro.
It does **not** prove whether that small corrected rate was real slow head movement, true gyro
bias change, or a changing glasses-to-head fit. With the magnetometer disabled, those causes
are observationally indistinguishable from this IMU stream alone. The wearer explicitly chose
preserving intentional slow head turns over treating every slow rate as drift. Do not claim a
software-only estimator retune can guarantee a permanently fixed yaw under that choice; seek
an independent heading or stationary reference before changing bias authority. A second
user-confirmed recenter occurred around elapsed 2797-2798 s.
The wearer then placed the still-running glasses on a stable surface from about 21:27 to
21:30 BST. In a trimmed stationary interval (elapsed 3222.6-3378.2 s, 9,289 sampled rows),
reported yaw changed only -0.033 deg, about -0.015 deg/min. Raw-minus-bias gyro projected
through the active head-up axis predicts -0.034 deg of camera yaw. Rest and routine adaptation
both qualified for 100% of this interval. The worn-event yaw slope was about 70 times larger.
This shows the estimator can hold this stationary device in this orientation, making a simple
constant free-standing sensor drift unlikely. It does not prove the same bias/fit while worn or
after head movement. The strongest current hypothesis is small real glasses-frame motion while
worn (head microturns or frame slip); wear-dependent gyro bias is still possible. The IMU stream
alone cannot distinguish these and cannot guarantee fixed yaw while preserving intentional
slow head turns. Compare worn and stationary conditions before attributing every recenter to
hardware thermal drift or declaring another estimator fix.

## 8. Definition of done for any change

1. Builds clean under `/W4 /permissive-` (no new warnings).
2. All ten test suites pass, and the change's own regression test fails without the change.
3. Numeric claims in the commit message are reproducible from printed test output.
4. Docs updated if behaviour or a trade-off changed (`docs/PROTOCOL-NOTES.md`, this file).
5. No new global state, no unbounded memory/GPU growth, no resource leaks (see `capture_smoketest`
   and the GDI object-count check).
