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
   the leak (`drift_leak_tau_s`) and the hard clamp (`drift_limit_degs`).
5. **The bias is bounded.** `bias_limit_degs = 1.5` (the documented plausible bias band) is clamped
   after every adaptation and after calibration. Nothing in the estimator may be able to spin away.
6. **Stale estimates must always be able to recover.** After `adapt_escape_s` without motion the
   adaptation re-opens even if the rate gate would block it. Any change to the gates must preserve
   that property (a stale estimate that locks adaptation out produces an unbounded creep).
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
| 4 | Micro head movements felt unregistered, then jumped | A hard **freeze while still** discarded everything below its release threshold | Replaced with drift absorption (now opt-in, see 6) |
| 5 | Centre screen off-centre after panning out and back | The bias adaptation gated on the **raw** rate with a 5 deg/s cap, so the slow ramp/settle of a deliberate pan was absorbed into the bias | Raw-rate gates (calibration 1.5, adaptation 0.5 deg/s), post-motion hold-off, deviation gate on the absorption |
| 6 | The whole workspace slowly rotated until the left screen was almost centred (~10 min) | The drift **correction** and the bias adaptation removed the same error; the correction (owning the integral) was never released, and it also swallowed post-pan accelerometer settling (3.87 deg/min measured in the field) | Single owner (bias); absorption off by default; leak (tau 20 s) + 2 deg clamp if re-enabled |
| 7 | Gating bug: my "corrected-rate" gate was mathematically a *lag* test; a gently ramping rotation passed it, inflated the bias, then locked adaptation out (runaway to -117 deg) | Gate on RAW rate, hard bias clamp, escape valve |
| 8 | Calibration tool aborted at stream-on although the glasses had accepted the command | hidapi on Windows returns `length + 1` for a successful write to this device; the code compared it to `length` | `write_report` returns the raw value; command flows use `send_command_verified` (waits for the `99 c8` ack) |
| 9 | Calibration could not complete ("SMALL_EXCURSION", "HOLD_STILL") | 25 deg excursion gate + 5 deg/s motion detection + a motionless-window still requirement that a worn head cannot satisfy; failures also hid their own diagnostics | 12 deg excursion, 2.5 deg/s detection, quietest-1.5-s still window, diagnostics populated on failure, per-step retries |
| 10 | Capture policy implemented nowhere, GDI fallback churning objects, transient errors killing the app, VDD rollback gaps | See `docs/PROTOCOL-NOTES.md`; fixed in commit `caeef11` |

**Standing lesson:** the IMU path is where the subtle bugs live. Every gating change must be
accompanied by a synthetic scenario that fails before and passes after, and every claim in a commit
message must be reproducible from the test output.

---

## 4. Known limitations (state them, do not hide them)

- **No magnetometer.** It is off because the field in the test environment is heavily distorted
  (71 uT dominated by one axis) and feeding it caused a constant ~4.8 deg/s yaw spin; the official
  RayNeo runtime does not use it either. Consequence: with no absolute heading reference a *steady*
  slow yaw rotation is physically indistinguishable from a yaw bias. Every yaw gate is therefore a
  trade-off; the current choice favours stability (slow steady pans may lose part of their travel).
  `R` / Ctrl+Alt+R recenters.
- **Bias band.** The estimator assumes |bias| <= 1.5 deg/s. A larger true bias cannot be corrected and
  would show as creep; better to recalibrate.
- **Virtual displays need the signed Parsec VDD driver**, which is not installed by default. Without
  it run with `--no-virtual-displays` (labelled test screens).
- The glasses monitor must be in **Extend** mode (duplicate/PC-screen-only makes it invisible to the
  app and breaks the geometry).

---

## 5. Test map and how to run

Private build directory per agent (never share `build/`, and never use `--preset default` when other
agents may be building — it uses the shared `build/`):

```
cmake -S . -B build/agent-X -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="C:/Users/aksha/Desktop/GitHub/tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build/agent-X
ctest --test-dir build/agent-X --output-on-failure
```
Run inside the MSVC environment: `call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"`
(CMake's Visual Studio generator cannot find this Build Tools install; Ninja is required.)

| Suite | Covers |
|---|---|
| `camera_selftest` | Frame conversion, the sign triple, single-axis and combined orientation cases |
| `pose_selftest` | Tilted-recenter coupling, contaminated calibration, reconfigure resets, swing/twist helpers, drift-absorption behaviour, 20-cycle pan-and-settle accumulation |
| `pose_scenarios` | Nine synthetic worn-head scenarios through the real estimator: pan-and-return both ways, small moves, slow and fast pans, residual bias, breathing, diagonal, recenter |
| `orientation_calibration_selftest` | Guided-calibration maths, rejection gates, file round-trip, version handling |
| `protocol_selftest` | 66/99 framing and `99 65` decoding |
| `layout_selftest` | Layout parsing/validation, geometry, capture-policy constraints |
| `vdd_selftest` | Parsec VDD protocol, cleanup order, index parsing |
| `view_selftest` | Offline "virtual glasses": real layout + real camera maths projected to NDC, numeric assertions, and 640x360 PPM frames in `scratch/` |

Every fix MUST come with a regression test that fails without it. Synthetic IMU scenarios live in
`pose_scenarios.cpp` (`HeadSim` helpers) and `pose_selftest.cpp` (local helpers).

---

## 6. Traps that have already cost time

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

---

## 7. Definition of done for any change

1. Builds clean under `/W4 /permissive-` (no new warnings).
2. All eight test suites pass, and the change's own regression test fails without the change.
3. Numeric claims in the commit message are reproducible from printed test output.
4. Docs updated if behaviour or a trade-off changed (`docs/PROTOCOL-NOTES.md`, this file).
5. No new global state, no unbounded memory/GPU growth, no resource leaks (see `capture_smoketest`
   and the GDI object-count check).
