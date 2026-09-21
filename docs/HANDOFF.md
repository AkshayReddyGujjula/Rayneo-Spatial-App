# RayNeo GT Spatial Workspace — Handoff

Author: previous agent session (Command Code), 2026-09-20.
Audience: the next coding agent (Codex) + Akshay.
Everything below is either **VERIFIED** (with evidence) or explicitly marked **ASSUMED**.

---

## 0. Current state in one paragraph

A C++20/Direct3D 11 app that renders a head-tracked, world-locked scene on RayNeo GT AR glasses
exists and runs on this machine. The IMU is read directly from the glasses over HID (no vendor SDK),
fused with our own Madgwick filter, and drives the camera. Two serious bugs were found and fixed
today with numerical evidence and regression tests. **One bug remains**: while the head is still the
world is rock-solid, but when the head moves or tilts the world rotates *around* the head instead of
staying fixed in space. The most likely cause is the **never-measured sensor-to-head mounting
alignment** (see §7), and the repo now contains a validated design to measure it
(`docs/orientation-calibration.md`). Everything needed to reproduce, diagnose and continue is below.

**Codex continuation, 2026-09-20:** the guided calibration is now implemented as
`orientation_calibrate.exe`, persists a validated proper rotation to `config/orientation.json`,
and is required by `spatial_desk` before IMU tracking starts. The design's row-order bug was
corrected: head coordinates are `(right, forward, up)`, keeping gravity on Madgwick `+Z`.
The camera now applies the exact quaternion basis transform rather than rebuilding combined
motion through Euler angles. These changes pass offline regressions but still require the live
worn-glasses acceptance test in §7.

**Codex continuation, 2026-09-21:** the hard freeze-when-still path was removed after the user
confirmed that it discarded sub-degree head adjustments. Pose integration is now always live;
gyro bias is the sole steady-error owner. Startup uses a 4 s discard followed by one contiguous
rest-qualified bias window, and runtime recovery uses gyro-plus-accelerometer rest detection with
a guarded slow escape/rollback. All eight offline suites pass; this revision still needs the next
worn-glasses acceptance run before it is packaged as the daily-use app.

---

## 1. The goal (user's words, condensed)

Build a RayNeo "app" for the GT on Windows that turns the glasses into a spatial workspace:
several **real Windows virtual monitors** (default 3: left/centre/right), each individually
positionable (yaw / pitch / roll / distance / size), all controlled live from a visual UI, so you
look around by turning your head and the screens stay put in space.

Evaluation gate (already satisfied): VertoXR was trialled and **rejected** by the user (too limited,
didn't like it), and it was fully uninstalled. **We are building our own.** The approved milestone
plan is at `C:\Users\aksha\.commandcode\plans\rayneo-gt-spatial-workspace.md` (M0-M8, risks,
downloads list).

---

## 2. Hardware, host and environment (VERIFIED / INSTALLED TODAY)

| Item | Detail |
|---|---|
| Glasses | RayNeo GT (board id `0x40`; GT Max would be `0x41`), firmware build string "Sep  7 2026" |
| Glasses USB | runtime `VID:PID 3941:AF50`, DFU `3941:AF51`; HID usage page `0xFF00`, usage `1`, product "RayNeo AR Glasses" |
| Glasses display | appears in Windows as `Generic Monitor (SmartGlasses)` (TCL panel), native 1920x1080. **Currently runs at 60 Hz** (should be set to 120 Hz) |
| Host GPU | Intel Arc 140V (single adapter - no hybrid-GPU/cross-adapter capture problems) |
| Host panel | Lenovo DisplayHDR 2880x1800 (primary) |
| Compiler | VS Build Tools 2022 17.14.25, MSVC 14.44.35207 (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`) |
| CMake | 4.4.3 (`C:\Program Files\CMake\bin\cmake.exe`) |
| Ninja | installed via winget (`%LOCALAPPDATA%\Microsoft\WinGet\Links`) |
| vcpkg | `C:\Users\aksha\Desktop\GitHub\tools\vcpkg` (bootstrapped; manifest mode; only dependency is hidapi 0.15.0) |
| Python (helpers only) | `C:\Python314\python.exe`; probe venv at `%TEMP%\commandcode\...\scratchpad\venv` (hidapi installed) |

**Build (the only verified invocation - VS is not on PATH and CMake's VS generator does NOT find the
Build Tools install via vswhere on this machine, so use Ninja + vcvars):**

```cmd
cd C:\Users\aksha\Desktop\GitHub\RayNeo-Spatial
set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Links;%PATH%" ^
 && call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" ^
 && "C:\Program Files\CMake\bin\cmake.exe" --build build
```

First-time configure (already done once; the `build/` dir exists):

```cmd
... (same vcvars preamble) ...
"C:\Program Files\CMake\bin\cmake.exe" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE="C:/Users/aksha/Desktop/GitHub/tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

Binaries land in `build\`: `spatial_desk.exe`, `gt_imu_probe.exe`, `pose_selftest.exe`,
`camera_selftest.exe`. `hidapi.dll` is copied next to them automatically.

---

## 3. Repository layout

```
C:\Users\aksha\Desktop\GitHub\RayNeo-Spatial\
├─ CMakeLists.txt            C++20, /W4 /permissive-, UNICODE+_UNICODE, targets below
├─ CMakePresets.json         Ninja + vcpkg toolchain (see §2 for the verified command line)
├─ vcpkg.json                dependency: hidapi (manifest mode)
├─ src\
│  ├─ imu\
│  │  ├─ gt_protocol.h/.cpp  GT 66/99 framing, 99 65 decode, opcodes, board ids
│  │  ├─ gt_hid.h/.cpp       hidapi transport (enumerate/open/write/read), Windows wchar handling
│  │  ├─ fusion.h/.cpp       own Madgwick AHRS (9-axis capable), quat helpers, quat_to_euler (ZYX)
│  │  ├─ pose_estimator.h/.cpp   bias calibration + stillness detection + frame handling + recenter
│  │  └─ imu_source.h/.cpp   worker thread: device retry loop, stream control, publishes pose+bias+raw
│  ├─ render\
│  │  ├─ camera.h/.cpp       CameraSigns + camera_view_matrix (see §5 - the sign triple is LOAD-BEARING)
│  │  └─ renderer.h/.cpp     D3D11 device/swapchain (waitable flip model), scene, depth states
│  ├─ app\spatial_desk.cpp   window/monitor selection, main loop, R recenter, --log CSV, stats line
│  └─ tools\
│     ├─ gt_imu_probe.cpp    M1 probe: enumerate, commands, stream, CSV, --pose mode
│     ├─ camera_selftest.cpp offline camera verification (yaw→pan, pitch→vertical, roll→in-plane)
│     └─ pose_selftest.cpp   offline pose regression (tilted recenter coupling + contaminated bias)
├─ docs\
│  ├─ PROTOCOL-NOTES.md      LIVE-VERIFIED protocol, fusion notes, frame conventions (authoritative)
│  ├─ orientation-calibration.md   validated design for measuring the sensor→head mounting
│  └─ HANDOFF.md             this file
├─ tools\gt_probe.py, tools\gt_probe_cal.py   original Python protocol probes (throwaway spike)
├─ config\layouts\default.json   planned layout file (not yet consumed by code)
├─ references\               MIT reference clones: ar-glass-lib, RayNeo-Air-3S-Pro-OpenVR, ar-drivers-rs, parsec-vdd
└─ scratch\                  (gitignored) agent analysis scripts + test logs - see §9
```

Not under version control yet (no `git init` was run). Suggest doing that first as a safety net.

---

## 4. Hardware protocol - LIVE VERIFIED (see docs/PROTOCOL-NOTES.md for the full table)

* **Framing**: 64-byte HID output report `[0x00 report id][0x66 opcode][0x00 x61]`.
  A 64-byte write **without** the leading `0x00` is silently ignored (verified both ways).
* **Replies**: `99 c8` = command ACK: `[2]=board id, [4..7]=device counter (LE, increments),
  [8]=echoed opcode`. `99 65` = 64-byte nine-axis report.
* **Opcodes**: `0x00` device info (ASCII build date at byte 23), `0x01` stream on, `0x02` stream off.
  `0x3c` and `0x3e` (factory calibration / gyro temperature table) get **NO REPLY on this firmware**
  (tested with the stream stopped; the ar-glass-lib notes on them come from Air-family firmware).
  Runtime gyro-bias estimation is used instead (see §5).
* **`99 65` layout** (little-endian): accel @4 (3×f32, m/s²), gyro @16 (3×f32, **deg/s**),
  temperature @28 (f32, °C), magX @32, magY @36, tick @40 (u32, **100 µs** units), magZ @52.
* **Rate**: 475.3-476 Hz sustained; host-measured and device-tick-measured agree to 0.1 Hz.
* **Stream persists after the host process exits** - always send `0x02` before closing.
* Rest values observed (glasses on desk): |accel| ≈ 9.84 m/s², gyro bias ~0.1-1.2 deg/s,
  |mag| ≈ 71 µT dominated by one axis (indoor/laptop distortion), die temp ≈ 39-40 °C.

**Magnetometer is OFF by default** (mag_weight 0): feeding it into the filter produced a constant
~4.8 deg/s yaw spin because the field is heavily distorted here; the official RayNeo runtime also
does not feed the `99 65` magnetic fields into its fusion (ar-glass-lib notes).

---

## 5. Pipeline & conventions (READ THIS BEFORE TOUCHING THE MATH)

```
HID (475 Hz) -> gt_protocol decode -> PoseEstimator -> ImuSource (worker thread)
                                   -> Renderer (D3D11, waitable swapchain) -> GT monitor
```

* **Filter**: own Madgwick implementation (`fusion.cpp`); quaternion is **sensor → earth**, earth is
  right-handed with **Z up** (the filter's gravity reference is +Z).
* **Axis mapping**: package → head comes only from the proper rotation measured by
  `orientation_calibrate.exe` and loaded from `config/orientation.json`. Do not reintroduce the
  legacy hard-coded `[x, -z, y]` assumption.
* **Bias handling** (this is what killed the "creeps while still" complaint):
  - discard the first 4 s, then average one **contiguous** high-confidence rest window of at least
    600 samples (~1.26 s at 476 Hz). Motion discards the whole candidate; diagnostic timeouts never
    open tracking with an unqualified estimate.
  - rest uses smoothed gyro deviation **and** accelerometer deviation, a 0.5 s dwell and a 1 s
    post-motion hold-off. Rest gates bias adaptation only; it never gates pose integration.
  - routine adaptation may chase only a <=0.35 deg/s residual. A larger residual must persist for
    8 s of accumulated rest before the tau-40 s escape follows it. The estimate is always clamped
    to +-1.5 deg/s.
  - escape snapshots the prior bias. If the measured rate later returns to that snapshot band, it
    rolls back so an ambiguous slow turn is not unwound after the head stops.
* **Recenter** (`R` key): `q_ref = current`, and all published poses are relative to it.
* **Relative rotation is EARTH-frame**: `q_rel = q * q_ref^-1`. The body-frame order
  (`q_ref^-1 * q`) mixed yaw into pitch/roll whenever the head was tilted at recenter
  (measured: up to 16° lost pan, 26-34° spurious tilt; reproduced the user's hand-turn log within
  0.2°). `pose_selftest.exe` locks the correct behaviour in.
* **Camera**: after calibration, head axes are X=right, Y=forward, Z=up. The render basis is
  X=right, Y=up, Z=forward, so quaternion axial components transform as `(-x,-z,-y)` with
  the fixed sign triple **(-1, -1, -1)**. `camera_view_matrix` uses the transformed quaternion
  directly; `camera_selftest` verifies the exact combined-orientation basis transform.
  **The signs are load-bearing**: flipping any one mirrors the world.
  (An earlier build exposed them as live I/K/L toggles - that was removed, correctly.)
* **Always-live pose**: freeze-when-still was removed because it suppressed micro-movements and
  released them as jumps. `--freeze-still` and `--no-freeze-still` are obsolete compatibility
  flags; both are accepted and ignored.
* **Renderer**: D3D11 device, `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, 2 buffers,
  flip-discard, `SetMaximumFrameLatency(1)`, vsync `Present(1,0)`. Explicit depth-stencil states;
  the crosshair (head-locked) is drawn with depth disabled. Scene: grid (LINELIST) + 4 quads
  (blue 0°, red -90° = left, green +90° = right, yellow 180°) at 2-3 m.

---

## 6. Bugs found and fixed today (with evidence)

| # | Bug | Evidence | Fix |
|---|---|---|---|
| 1 | Fused Z-up quaternion fed into a Y-up renderer as if frames matched → head **yaw acted as a roll** around the view axis ("monitors moving everywhere, crosshair centred") | first M3 user test | camera rebuilt in render convention (`camera.cpp`); `camera_selftest` |
| 2 | Relative rotation built in the **body** frame | agent simulation: up to 16° lost pan, 26-34° spurious tilt at 30°/90°; fit the user's hand-turn log (57/-15/-7) to 0.2° with a 17° tilt | earth-frame relative rotation; `pose_selftest` (tilted 20° recenter + 90° yaw → yaw -89.95°, pitch -0.005°, roll -0.010°) |
| 3 | Bias **catch-22**: refinement gated on the *corrected* signal, so a large initial error (motion during calibration) closed the gate forever → constant spin ("spins even when I'm still") | agent Monte-Carlo: up to 379 deg/min, gate open 0% of the time | variation-based stillness + rate cap + still-gated calibration + continuous adaptation; `pose_selftest` (contaminated calibration converges exactly, residual 0.00 deg/min) |
| 4 | `I`/`K`/`L` sign toggles + `--invert` | audit: flipping any sign mirrors the world up to 180°, it does not reverse a direction | removed from the app |
| 5 | Crosshair drawn into the shared depth buffer with no explicit depth state; implicit default depth state | audit | explicit depth states; crosshair depth-disabled |

Also fixed along the way: hidapi include is `<hidapi.h>` (vcpkg layout), `UNICODE/_UNICODE` defines
(`IDC_ARROW`), CMake VS-generator can't find the Build Tools install (use Ninja + vcvars), and
`;` is not a separator in cmd (use `&&`).

---

## 7. OPEN ISSUE - the remaining bug (start here)

**User's report after the fixes:** *"it only moves around my head when I move or tilt my head; when
I stay still it also stays still"*. So freeze/creep is solved (still ⇒ still), but during head
motion the world rotates around the head instead of staying world-locked.

### Ranked hypotheses

1. **Unknown sensor→head mounting alignment - most likely.**
   The pipeline assumes the filter's earth frame maps to the head as
   *earth X = head forward, earth Y = head left, earth Z = up* (baked into the fixed `C` in
   `render/camera.cpp`). But the filter's horizontal axes are defined by the sensor package's x/y
   axes (after the `[x,-z,y]` mapping), whose physical relationship to the head's forward/left has
   **never been measured on GT hardware** - the mapping was taken from Air-family documentation.
   A constant azimuth error (e.g. the sensor "x" really being the head's left) mixes nod and tilt
   and makes the world rotate about the head whenever the head moves - exactly the symptom.
   *Fix*: measure it. `docs/orientation-calibration.md` contains a complete, validated design
   (4 phases: hold still / turn / nod / tilt; PCA of the gyro vectors per phase; builds a
   sensor→head 3x3; ≤0.03° recovery over 200 random simulated mounts; failure messages; exact log
   format). Apply the result in `PoseEstimator::map_gyro/map_accel` (replacing the hard-coded
   mapping) and relabel euler channels if the axes come out permuted.
   *Fast diagnostic without implementing calibration*: run `spatial_desk.exe --log scratch\run.csv`,
   have the user perform three clean single-axis motions (turn left-right, nod up-down, tilt
   left-right) and inspect which raw gyro components move in each phase - that identifies the
   mapping directly from data.
2. **Latency / motion-to-photon** (world "swims" behind the head during motion) - possible but
   unlikely to be described as "moves around my head"; and it is currently untestable at 120 Hz
   because the glasses run at **60 Hz** (set 1920x1080 @ 120 Hz in Windows display settings first).
3. **Camera transform** - ruled out: the audit verified the shipped camera is exact to 2e-6°
   (single-axis and combined), so this is the lowest priority.

### What is VERIFIED vs ASSUMED

* Verified: protocol bytes, rate, stream control, bias behaviour (offline + live), frame handling in
  the camera and pose math (offline), freeze-when-still, app runs and renders on the GT monitor.
* Assumed: the `[x,-z,y]` package→body mapping for the GT, the worn-pose axis correspondence
  (forward/left/up), the camera's `C` azimuth, and the glasses' 60 Hz mode being a settings issue
  rather than an EDID limitation.

---

## 8. Next steps, in priority order

1. **Capture motion data** (30 lines of user effort): run with `--log`, ask for three clean
   single-axis motions, analyse the CSV, and determine the mounting (pure permutation/mirror or a
   general rotation). The CSV columns are:
   `elapsed_s,tick_100us,gx_raw,gy_raw,gz_raw,bias_x,bias_y,bias_z,yaw_deg,pitch_deg,roll_deg,still`
2. **Apply the alignment**: either the guided calibration from `docs/orientation-calibration.md`
   (preferred, self-service and repeatable) or the fixed mapping indicated by the CSV.
3. **Set the glasses to 1920x1080 @ 120 Hz** (Windows display settings), then re-test
   motion-to-photon feel.
4. Continue the approved plan: **M4** (three test quads at configured yaw/pitch/roll - mostly the
   existing scene code + config), **M5** (Parsec VDD virtual monitors + `SetDisplayConfig`
   arrangement + keepalive + cleanup), **M6** (DXGI Desktop Duplication per output + cursor
   compositing + capture policy with hysteresis), **M7** (config/hotkeys/diagnostics),
   **M8** (local web control UI + WebView2 tray shell).
5. Housekeeping: `git init` + first commit; optionally move the useful agent scripts from
   `scratch/` into `docs/analysis/` (scratch is gitignored and can be cleaned).

---

## 9. Reference material and artifacts

* `docs/PROTOCOL-NOTES.md` - authoritative, live-verified protocol + fusion + frame conventions.
* `docs/orientation-calibration.md` - the calibration design (phases, algorithm, validation gates,
  log format, bake-in snippet). Prototype: `scratch\calib_design.py` (+ `calib_run_output.txt`).
* `scratch\` - analysis scripts and logs from three investigation agents:
  - `h1_frames.py`, `h1_logfit.py`, `h2_bias.py` (coupling + bias Monte-Carlo; the numbers in §6)
  - `camera_chain_audit3.py`, `final_check.py` (camera exactness proof, 2e-6°)
  - `calib_design.py`, `CALIBRATION-DESIGN.md`, `calib_run_output.txt`
  - `session.log` (empty - see traps), `diag_test.csv` (logger verification)
* `references/ar-glass-lib` (MIT) - the GT protocol documentation; read the sections
  "RayNeo Air 3/4-family protocol notes" and "RayNeo nine-axis calibration notes".
* `references/` also holds verncat's RayNeo SDK (MIT, Windows/libusb precedent), `ar-drivers-rs`
  (MIT, cross-check for packet layouts) and `parsec-vdd` (MIT, the VDD API for M5).
* Approved milestone plan: `C:\Users\aksha\.commandcode\plans\rayneo-gt-spatial-workspace.md`.

---

## 10. Traps and gotchas (each of these cost time today)

1. **Backgrounding swallows stdout.** A backgrounded `spatial_desk.exe` with `> file` produced an
   **empty** log even on clean exit (CRT buffering + task capture). Run it in the foreground or use
   `--log`. Do not trust the task log for this exe.
2. **`;` is not a cmd separator.** `shell_command` runs under cmd.exe: use `&&`.
3. **No VS instance is discoverable by CMake's VS generator** on this machine (vswhere returns
   nothing even though Build Tools are installed) - always build with Ninja inside the vcvars
   environment (§2).
4. **The glasses' display path is flaky.** It has appeared/disappeared repeatedly during the
   session; `Win+P -> Extend` is needed. Windows' own enumeration may report the glasses as
   1280x720 while a DPI-aware app correctly sees 1920x1080 - trust the app's output.
5. **Ghost PnP nodes are normal**: `MI_00` CDC/COM3, `MI_05` vendor HID, `MI_06` DFU, plus an
   `AF51` DFU device - these are cached configurations, not errors.
6. **VertoXR leftovers were cleaned** (two `ROOT\DISPLAY\0000/0001` "Virtual Display" devices +
   the `oem158.inf` driver package, removed with `pnputil`, admin/UAC).
7. **Frame conventions are the #1 source of bugs here.** Read §5 before touching anything spatial;
   run `pose_selftest.exe` and `camera_selftest.exe` after any change.
8. **The user is strict about consent**: ask before installs, driver changes, device removals or
   anything that changes system state. Explain before acting; keep answers concise.
9. **Code style**: C++20, `/W4`, comments only where knowledge is non-obvious (protocol bytes,
   frame conventions). No comments on self-evident code.
