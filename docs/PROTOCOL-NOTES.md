# RayNeo GT protocol notes (live-verified)

Verified against a RayNeo GT (board id 0x40, firmware build "Sep  7 2026") on Windows 11,
September 2026, via hidapi and the throwaway Python probes in `tools/`.

## USB identity

| Item | Value |
|---|---|
| Runtime VID:PID | `3941:AF50` |
| Board id (ACK byte 2) | `0x40` = GT, `0x41` = GT Max |
| HID interface | vendor-defined, usage page `0xFF00`, usage `1`, product "RayNeo AR Glasses" |
| DFU mode device | `3941:AF51` |

In the currently observed (display-active) configuration, the glasses expose a single
vendor HID interface with `interface_number = 0`. Cached composite configurations from
earlier sessions showed CDC control/data (`MI_00` -> COM3), vendor HID at interface 5
(`MI_05`) and a runtime DFU interface (`MI_06`), matching the ar-glass-lib firmware notes.

## HID framing (Windows)

Outgoing commands are 64-byte output reports:

```
[0x00 report id] [0x66 command tag] [cmd byte] [0x00 x 61]
```

Writing the frame without the leading `0x00` report id is ignored by the device.
Incoming reports are 64 bytes and start with the `0x99` tag.

## Opcodes

| Command | Meaning | Response |
|---|---|---|
| `66 00` | device info | `99 c8` ack, echo `0x00` at byte 8; build date ASCII at byte 23 |
| `66 01` | stream on | `99 c8` ack, echo `0x01` at byte 8 |
| `66 02` | stream off | `99 c8` ack, echo `0x02` at byte 8 |
| `66 3c` | factory transform + gyro bias | **no reply observed on this firmware** |
| `66 3e` | 81-point gyro temperature bias table | **no reply observed on this firmware** |

`99 c8` ACK layout: `[0]=0x99 [1]=0xc8 [2]=board id [3]=0x00 [4..7]=device counter (LE, increasing)
[8]=echoed opcode`.

Streaming persists after the host process exits; send `66 02` to stop it (or re-send `66 01`).

## 99 65 nine-axis report (64 bytes)

| Offset | Contents |
|---|---|
| 0..1 | `99 65` |
| 4 | accel X, Y, Z (float32 LE, m/s²) |
| 16 | gyro X, Y, Z (float32 LE, deg/s) |
| 28 | temperature (float32 LE, °C) |
| 32 / 36 | magnetometer X / Y (float32 LE, µT) |
| 40 | device tick (uint32 LE, 100 µs units) |
| 52 | magnetometer Z (float32 LE, µT) |

## Observed behaviour

- Stream rate: ~476 Hz (device tick advances 21 x 100 µs per sample; both host- and
  device-measured rates agree).
- At rest: |accel| ~= 9.84 m/s², gyro bias within +-1.5 deg/s, |mag| ~= 71 µT, die temp ~= 39.4 °C.
- Gyro reports in deg/s (a resting value of ~1.4 would be implausible in rad/s).
- No factory gyro-bias reply on this firmware; use runtime bias estimation instead.
- Sensor-to-head axes are supplied by the proper rotation measured by
  `orientation_calibrate.exe` and stored in `config/orientation.json`; runtime fusion has no
  hard-coded legacy axis mapping.

## Open questions

- Whether `66 3c` / `66 3e` respond in the composite (interface 5) configuration.
- Whether magnetometer samples are fresh every report or cached.
- Long-session thermal bias behaviour on the final worn-glasses build.

## Fusion notes (live-verified)

- **Yaw has no absolute reference.** With the magnetometer off, a very slow steady
  yaw rotation and a yaw bias are physically indistinguishable, so the estimator is
  deliberately conservative and every gate is a trade-off. The current design:
  startup calibration captures the bias from ONE contiguous high-confidence rest
  window after a 4 s warmup (fragments are never averaged; diagnostic timeouts do
  not open tracking); a VQF-inspired rest detector watches the low-passed gyro AND
  accelerometer with a continuous dwell; the estimate is hard-clamped to +-1.5 deg/s;
  routine updates may only chase a residual within 0.35 deg/s of the current estimate,
  so a deliberate slow yaw is not learned wholesale; and a residual the routine path
  cannot explain opens a deliberately slow escape after 8 s of accumulated rest. The
  escape snapshots the pre-escape bias and rolls back to it when the observed rate
  returns to the snapshot's band for 0.2 s of dwell-qualified rest; detected motion
  resets that confirmation (the routine update cannot undo an armed
  escape excursion - only the rollback or the escape's own convergence ends it), so
  an ambiguous slow turn keeps >=80% of its travel and leaves no post-stop reverse
  slide, while a genuinely persistent bias step is kept (it converges over tens of
  seconds). Consequence: a steady sub-0.35 deg/s
  yaw rotation is treated as drift (use `R`/recenter after such a movement), while
  every normal head turn registers fully. `pose_scenarios` covers both sides of this
  trade-off and the rollback motion gate (scenarios 11, 13 and 14).
- **Rest detection:** the pose path is always live - every sample integrates the
  corrected gyro, and rest only gates *adaptation* (no freeze, deadband or snap).
  Rest needs the gyro deviation below 1.5 deg/s AND the accelerometer deviation below
  0.5 m/s^2 continuously for 0.5 s; crossing either exit gate (3.0 gyro / 1.0 accel) resets the dwell
  and starts a 1 s motion hold-off. A shaken package with a quiet gyro is therefore
  motion. The console and `--log` CSV expose this as `rest` (instantaneous),
  `still` (dwell-qualified), `adapt_state` (0 idle / 1 routine / 2 escape /
  3 calibrating), `corrected_rate_degs` and the cumulative `escape_rollbacks`.
  All gyro and accelerometer deviation/residual gates use Euclidean vector magnitude,
  so rotating between the calibrated sensor and head frames cannot change a gate result.
  An IMU timestamp gap >=50 ms invalidates the current calibration window, rest dwell,
  rollback dwell and drift-absorption increment.
  The 1.5 deg/s gyro noise gate is measured rather than synthetic: the guided
  worn-glasses still capture produced 0.75 deg/s median and 1.37 deg/s p99 EMA
  deviation. The runtime bias update remains separately limited to a 0.35 deg/s
  residual, so accepting real sensor noise at the rest detector does not grant the
  bias estimator authority over ordinary head motion.
- **Magnetometer: off by default.** In the test environment the field reads |B| ~= 71 uT
  dominated by the package X axis (laptop/desk interference; Earth's field here should be
  ~50 uT with a strong inclination). Feeding it into the filter produced a constant
  ~4.8 deg/s yaw spin. The official RayNeo runtime also does not feed `99 65` magnetic
  fields into its fusion. The mag path stays available behind a flag.
- **Gyro bias transient:** the first samples after `66 01` are contaminated. Nothing
  reaches the rest detector or the fusion for the first 4 s (and at least 150 samples);
  then one contiguous high-confidence rest window (at least 1.0 s and 600 samples,
  so about 1.26 s at 476 Hz)
  is averaged for the bias. A window broken by motion is discarded, never stitched
  together from fragments. The 10 s timeout resets only the diagnostic epoch: tracking
  remains closed until a valid window completes rather than publishing a contaminated
  or zero estimate.
- **Reference attitude** is initialised from gravity, so the relative yaw/pitch/roll start
  at exactly zero with no convergence transient.
- **Measured stationary drift (60 s, mag off, pre-2026-09 estimator):** ~6.7 deg/min overall,
  decaying to < 0.5 deg/min once bias refinement settles (~40 s); last 20 s effectively flat.
  Re-measure after changing the calibration/rest gates; the bounds the suites now enforce are
  listed in AGENTS.md section 5.
- Pitch/roll wander is ~+-2 deg over a minute (accelerometer noise and residual bias).
- Package -> head mapping comes only from the measured proper rotation in
  `config/orientation.json`; there is no hard-coded legacy axis assumption.

## Frame conventions (learned the hard way)

Three different frames meet in this project and mixing them up produces chaos
(the first M3 build had head yaw acting as a roll around the view axis):

1. **Sensor package frame** - as reported in the `99 65` report.
2. **Fused/earth frame** - right-handed, **Z up** (Madgwick's convention; the filter's
   gravity reference is +Z, and the quaternion is a sensor -> earth rotation).
3. **Render frame** - Y up, left-handed (DirectXMath default `XMMatrixPerspectiveFovLH`).

**Rule:** never feed the fused quaternion into the renderer as if it lived in the render
frame. After orientation calibration, head coordinates are X=right, Y=forward, Z=up.
Direct3D render coordinates are X=right, Y=up, Z=forward. The camera pose is the
head-relative rotation conjugated by that head->render basis, and
`q_rel = q * q_ref^-1` is the relative rotation expressed in the earth frame.

Using the body-frame order `q_ref^-1 * q` instead mixes yaw into pitch/roll whenever the head
was tilted when it was recentered: measured leakage was up to 16 deg of lost pan and 26-34 deg
of spurious tilt at a 30 deg tilt / 90 deg yaw, and it reproduced the observed hand-turn log
(yaw 57 / pitch -15 / roll -7) to 0.2 deg. `pose_selftest.exe` locks the corrected behaviour in.

The basis swap is a reflection. Quaternion axial components therefore transform as
`(x,y,z) -> (-x,-z,-y)`, represented by `CameraSigns(-1,-1,-1)`. The renderer builds its
camera matrix directly from that transformed quaternion, avoiding Euler reconstruction and
its combined-rotation ordering hazards. `camera_selftest.exe` verifies the exact basis
transform over 200 combined orientations (maximum vector error below `2e-5`) in addition to
the anatomical single-axis cases.

The diagnostic Euler labels are turn/nod/tilt only; the view matrix itself remains quaternion-exact.
