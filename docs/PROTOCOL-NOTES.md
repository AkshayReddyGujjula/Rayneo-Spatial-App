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

## Workspace display reload

The Parsec add IOCTL returns a driver index before Windows is guaranteed to expose
an active GDI output. Startup and layout reload wait up to 5000 ms for the full
set of active, named outputs before positioning. Rollback repeats that wait
against the restored indices. Positioning uses names from the successful wait.
The `VDD wait` log reports when each index first became active relative to the
start of that wait. On timeout it prints the last device name, adapter and monitor
flags, and active state for every expected index. Fatal startup or reload errors
include the Windows display landscape.

A failed grow gives newly added indices another bounded wait before removing
them. Removal failures log the IOCTL error on each attempt. The selftest exercises
one transient failure (250 ms backoff), three failures (250, 750 and 1500 ms) and
exhausted retries (four attempts).
After a successful rollback the engine keeps the previous layout. A new save or
an explicit reload request can retry a rejected edit. If rollback itself fails,
the engine reports the failure and exits through the normal display restore path.

A layout edit keeps the capture and its last displayed frame for any output whose
identity, position and mode still match. A changed output can require releasing
its old duplication before opening a replacement. If the edit then fails, the
rollback rebinds that output. DXGI fallbacks log the failing operation and HRESULT.

The renderer's D3D11 device keeps the DXGI adapter object from its creation, and
that object's `EnumOutputs` list is a snapshot: a virtual display added later by a
reload is missing from it. Capture therefore falls back to a fresh
`CreateDXGIFactory1` and re-enumerates the adapter with the same LUID before
giving up ("the capture output is not owned by the renderer's D3D11 adapter").

The remove IOCTL timeout is 5000 ms in the shutdown disconnect and 1000 ms (plus
the 4-attempt retry) for mid-session removals, which hold the IOCTL lock and so
delay the keepalive ping. Measured live on 2026-11-18: a mid-session removal completes in under 250 ms, but each removal
during the shutdown restore takes about 3.0 s, so the old 1000 ms timeout failed
with Windows error 258 and left displays alive while the laptop panel was being
restored. Removals taking 250 ms or more are logged as `VDD remove <n> took <ms> ms`.

**Stacked desktops.** Screens whose yaw is within half a screen width of each other,
and whose pitch differs by at least half a screen height, form a column. The column
nearest straight ahead holds the centre, the primary desktop at (0,0). Higher pitch
gets a lower desktop y: a screen above the centre is at (0,-1080) and one below it
is at (0,1080). Each column's base row is its most level screen. The glasses' home
slot (442,-1080) overlaps a desktop stacked above the centre, so both the takeover
and a reload move the glasses right of the workspace. The render window follows
its monitor on every 0.5 s layout tick.

**Laptop re-attach on reload (measured live 2026-11-18).** Every mid-session VDD add
or remove made Windows re-attach the detached laptop panel at the right end of the
arrangement, which pushed the glasses further right. The panel stayed attached until
shutdown. For 15 s after any reload that changes the screen count, the layout tick
checks for an active internal panel. If one is active, the engine moves the panel's
windows to the centre desktop and detaches the panel again through CCD (not
persisted). It then repairs windows minimized by the detach and records them for the
shutdown restore, with their homes mapped to the pristine laptop rect. Finally it
re-pins the virtual desktops, which is a no-op when nothing moved. Grow and shrink
were each verified on hardware for 3->4 (stacked above) and 3->5 (above and below).
In those runs the panel was absent after the reload, the set of minimized windows
was unchanged, and the shutdown restore was correct.

**Per-monitor display scale (measured live 2026-11-19).** The Settings > Display > Scale
value has no documented API. `DisplayConfigGetDeviceInfo` with header type `-3` fills
`{header; int32 minScaleRel; int32 curScaleRel; int32 maxScaleRel}` and
`DisplayConfigSetDeviceInfo` with type `-4` takes `{header; int32 scaleRel}`; the header's
`adapterId`/`id` are the active path's *source* (QDC_ONLY_ACTIVE_PATHS, matched to the GDI
name with `DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME`). Values index the step table
{100,125,150,175,200,225,250,300,350,400,450,500} relative to the recommended step, whose
index is `-minScaleRel`. On the 1920x1080 VDDs: min 0, cur 0, max 3 (recommended 100%, max
175%); 175 then 100 set and read back (168 dpi at 175%). **Trap:** writing a rel above max
(200%) returns 0 and reads back 200%, but the effective DPI stays clamped at 175%, so
`plan_display_scale` (`src/vdd/display_scale.*`) clamps to max itself and the engine logs it.
Windows persists the scale per monitor, so a VDD keeps the last value unless restored: the
engine records each original by monitor id at first touch and restores it before the VDD is
removed (normal shutdown, the topology guard, and a reload that shrinks the screen count).
Engine log lines start with `display scale:`.

**Portrait virtual desktops (2026-11-19).** The Parsec VDD offers no portrait modes
(measured live: only landscape, 1920x1080 at 24/30/60/144/240, 2560x1440, 3840x2160,
...). A layout screen with `portrait = true` therefore gets the 1920x1080 mode
rotated like a real monitor: `dmDisplayOrientation = DMDO_90` with
`dmPelsWidth = 1080`, `dmPelsHeight = 1920` and `DM_DISPLAYORIENTATION` in
`dmFields` (`build_virtual_desktop_mode`). Windows persists a monitor's rotation
across sessions, so every landscape slot states `DMDO_DEFAULT` explicitly;
otherwise a VDD rotated in an earlier session comes back rotated. The refresh-rate
query uses `EDS_ROTATEDMODE` and accepts the base mode in either dimension order
because a display that is already rotated lists its modes rotated. The readback
(`display_layout_matches`) and the reload's `needs_change` compare orientation, so
a rotation change is a mode change. `ConfiguredDisplay` stores the rotated extent
plus the orientation, so capture reuse rebinds only the rotated output.
Arrangement: the planner gives each desktop its own extent. A column is as wide
as its widest member, and members are centred on the column's base screen. Base
rows are vertically centred on the centre desktop's centre line, so a portrait
screen beside a landscape centre sits at y = -420 and landscape neighbours of a
portrait centre sit at y = +420. Stacks use cumulative heights. Planned rects:
portrait left `(-1080,-420)`, centre `(0,0)`, right `(1920,0)`; portrait centre:
left `(-1920,420)`, centre `(0,0)` 1080x1920, right `(1080,420)`. A column wider
than its base, such as a landscape screen above a portrait centre, would leave a
gap in the base row. Windows would pack that gap, so `plan_workspace_for_layout`
refuses such a plan through `desktops_tile_seamlessly` (which requires no area
overlap and an edge-connected set, and neighbouring column bases sharing an edge,
so a stacked row cannot hide the base-row gap; gaps between two stacked screens
are accepted). The same pure check, `workspace_layout_is_arrangeable`, refuses
the layout in the controller's Save and in the engine before any VDD is added
or moved. After a commit the readback tolerates only a position shift; another
extent, rotation or primary fails into the rollback. Desktop Duplication returns a rotated output
unrotated, with `DXGI_OUTDUPL_DESC.Rotation` set; the capture/render side
uprights it.

## Rotated (portrait) capture

A `portrait` screen's virtual display runs the 1920x1080 mode rotated with
DMDO_90, so its desktop is 1080x1920. Desktop Duplication does not rotate for
you: `AcquireNextFrame` returns the image in the output's unrotated scan-out
orientation (a 1920x1080 surface), and `DXGI_OUTDUPL_DESC.Rotation` says how to
turn it upright. The capture stays on DXGI (it used to drop rotated outputs to the
GDI fallback) and reports the rotation with each frame. The renderer applies it
as a per-screen UV transform in the pixel shaders, so the copy stays a single
`CopyResource` with no extra pass or texture. The transform is the pure function
`surface_uv_transform` in `src/render/desktop_rotation.h`: 90 -> (v, 1-u),
180 -> (1-u, 1-v), 270 -> (1-v, u). `view_selftest` checks it pixel by pixel
against the destination-rect formulas in Microsoft's desktop duplication sample
(`DisplayManager::SetDirtyVert`, checked verbatim against
microsoft/Windows-classic-samples). That sample offsets the pointer position by
`DesktopCoordinates` and never rotates the pointer position or shape.

- **Sizes.** The upright desktop size is the duplicated surface size, with the
  axes swapped for 90 and 270. It is not taken from `DesktopCoordinates`, which
  can disagree with the surface. capture_smoketest once saw 1646x1029 coordinates
  for a 2880x1800 surface. That was not reproduced and its cause is not
  established; DPI virtualisation is the likely candidate (2880/1646 is about 1.75).
  The size is recomputed every frame. The capture texture, the GDI surface, the
  cursor mapping and the per-screen label texture (288x512 for a tall quad) all
  follow the actual sizes, so capture and render do not assume 16:9.
- **Pointer.** The engine draws the pointer itself from the duplication pointer
  info. Windows does not composite it into the duplicated image. Its position is
  in upright desktop pixels and its shape is upright, which is how the sample uses
  them. Only the masked-colour and monochrome cursors read the desktop under them,
  and that read goes through the same UV transform.
- **Dirty and move rects** come in surface space. They are unused here because the
  whole frame is copied.
- **Runtime change.** A rotation change is a mode change, so the duplication gets
  `DXGI_ERROR_ACCESS_LOST`. The duplication is then recreated and re-reads
  `Rotation`. A reload that changes a display's mode also rebinds its capture. The
  GDI fallback has no ACCESS_LOST, so it re-reads its monitor rectangle once a
  second and resizes its surface. GDI images are always upright.
- **Log lines for a live check.** Look for
  `capture output \\.\DISPLAYn: DXGI duplication, rotation 90 deg (DXGI_MODE_ROTATION 2), mode WxH, desktop coordinates 1080x1920`.
  It is printed whenever the rotation differs from the last one logged for that
  capture. If the surface ever contradicts the rotation, a
  `WARNING duplicated surface ... gives a ... desktop` line appears. GDI resizes
  print `GDI desktop changed A -> B`.
- **Not live-verified.** Pointer orientation on a rotated output is the main
  open item. The engine log does not show it, so look at the cursor on the glasses.

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
