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
- Sensor-to-body axis mapping is not yet verified; the official legacy path maps package
  vectors as `[x, -z, y]` and the direction will be confirmed empirically at M3.

## Open questions

- Whether `66 3c` / `66 3e` respond in the composite (interface 5) configuration.
- Whether magnetometer samples are fresh every report or cached.
- Axis mapping and sign conventions for the head pose (signs to be pinned down visually at M3).

## Fusion notes (live-verified)

- **Magnetometer: off by default.** In the test environment the field reads |B| ~= 71 uT
  dominated by the package X axis (laptop/desk interference; Earth's field here should be
  ~50 uT with a strong inclination). Feeding it into the filter produced a constant
  ~4.8 deg/s yaw spin. The official RayNeo runtime also does not feed `99 65` magnetic
  fields into its fusion. The mag path stays available behind a flag.
- **Gyro bias transient:** the first samples after `66 01` are contaminated. Calibration
  runs after a 150-sample settle window over 1500 samples, then keeps refining slowly
  while the device is still (gate 2 deg/s, gain 2e-4 per sample, ~476 Hz).
- **Reference attitude** is initialised from gravity, so the relative yaw/pitch/roll start
  at exactly zero with no convergence transient.
- **Measured stationary drift (60 s, mag off):** ~6.7 deg/min overall, decaying to
  < 0.5 deg/min once bias refinement settles (~40 s); last 20 s effectively flat.
- Pitch/roll wander is ~+-2 deg over a minute (accelerometer noise and residual bias).
- Package -> body axis mapping currently `[x, -z, y]` (official legacy fusion convention);
  signs still to be confirmed against physical motion at M3.

## Frame conventions (learned the hard way)

Three different frames meet in this project and mixing them up produces chaos
(the first M3 build had head yaw acting as a roll around the view axis):

1. **Sensor package frame** - as reported in the `99 65` report.
2. **Fused/earth frame** - right-handed, **Z up** (Madgwick's convention; the filter's
   gravity reference is +Z, and the quaternion is a sensor -> earth rotation).
3. **Render frame** - Y up, left-handed (DirectXMath default `XMMatrixPerspectiveFovLH`).

**Rule:** never feed the fused quaternion into the renderer as if it lived in the render
frame. The camera pose is the head-relative rotation conjugated by the earth->render basis:
`camera = C^T * R(q_rel) * C`, with `C` mapping earth X -> render +Z, earth Y -> render -X,
earth Z -> render +Y (`det(C) = -1` supplies the right-handed-earth / left-handed-render
reflection), and `q_rel = q * q_ref^-1` - the relative rotation expressed in the EARTH frame.

Using the body-frame order `q_ref^-1 * q` instead mixes yaw into pitch/roll whenever the head
was tilted when it was recentered: measured leakage was up to 16 deg of lost pan and 26-34 deg
of spurious tilt at a 30 deg tilt / 90 deg yaw, and it reproduced the observed hand-turn log
(yaw 57 / pitch -15 / roll -7) to 0.2 deg. `pose_selftest.exe` locks the corrected behaviour in.

The code implements the conjugation via euler extraction plus
`XMMatrixRotationRollPitchYaw(pitch, yaw, roll)` with the fixed sign triple `(-1, +1, -1)`.
That combination is algebraically identical to the conjugation above (verified numerically to
2e-6 deg over 20,000 orientations). Note `XMMatrixRotationRollPitchYaw` composes
`Rz(roll) * Rx(pitch) * Ry(yaw)`, which is NOT the inverse of a ZYX extraction - the exactness
comes from the basis change, not from the names of the arguments. The signs are therefore
load-bearing: they are not cosmetic direction switches, and flipping any one of them mirrors
the world by up to 180 deg.

`camera_selftest.exe` verifies the pipeline offline: yaw input must move a world point
horizontally, pitch vertically, roll about the view axis, with no cross-coupling.
