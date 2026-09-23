# RayNeo Spatial

A Windows C++20 / Direct3D 11 spatial workspace for the RayNeo GT AR glasses: several real Windows
virtual monitors (default three, at yaw −45° / 0° / +45°) rendered as world-locked screens on the
glasses, head-tracked in 3DoF from the glasses' own IMU, edited and controlled from a native
dashboard.

```
RayNeo Spatial.exe   controller: dashboard, visual layout editor, tray, diagnostics  (WIN32 subsystem)
spatial_desk.exe     renderer engine: D3D11 swapchain, Parsec virtual desktops, capture, IMU
orientation_calibrate.exe   guided sensor-to-head calibration (writes config/orientation.json)
gt_imu_probe.exe     protocol probe
```

The controller never renders and never changes system state on its own: it launches the engine,
sends it private window messages, reads a small status file, and reports what the machine actually
has (RayNeo display, HID interface, Parsec VDD driver, calibration file, telemetry).

## Quick start

```cmd
:: build (private Ninja directory per developer/agent; MSVC environment required)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build\agent-app -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE="C:/Users/aksha/Desktop/GitHub/tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build\agent-app
ctest --test-dir build\agent-app --output-on-failure
```

```powershell
# portable folder (controller + engine + config + notices), then run the GUI from there
powershell -ExecutionPolicy Bypass -File scripts\stage-portable.ps1 -BuildDir build\agent-app
```

1. Put the glasses in **Extend** mode (`Win+P` → Extend).
2. Start `RayNeo Spatial.exe`.
3. If orientation calibration is missing, press **Run calibration** and follow the console prompts.
4. **Start workspace** for real Parsec virtual desktops, or **Start preview** for a renderer-only
   check without the VDD driver. The two actions are separate on purpose; the app never falls back
   from one to the other silently.

Full details — setup, pinning, Extend mode, calibration, the VDD limitation, preview mode, recovery
and the manual hardware checks — are in [docs/WINDOWS-APP.md](docs/WINDOWS-APP.md).

## What the controller does

| Area | Behaviour |
|---|---|
| Engine lifecycle | Launches `spatial_desk.exe` with the resolved layout/calibration/log/status paths, captures its console to `logs/engine.log`, polls it at ≤ 2 Hz, stops it with a graceful quit message |
| Commands | Recenter, yaw toggle, pitch toggle, layout reload, quit — private window messages, plus a state query so the UI shows the engine's real tracking state |
| Layout editing | Presets (single, triple arc, quad arc, five arc, wide arc), drag editor for yaw/pitch, sliders for yaw/pitch/roll/distance/width/height/FOV/capture rates/hysteresis, 1–8 screens with unique ids and vdd indices |
| Persistence | `config/layouts/default.json` through `gt::validate_layout` + `gt::save_layout` (atomic), `config/app.json` for preferences (validated, atomic) |
| Diagnostics | Engine state, orientation calibration, Parsec VDD availability, RayNeo HID discovery, latest telemetry CSV row, engine log, bounded log rotation |
| Shell integration | Stable AppUserModelID (`RayNeo.Spatial.Desktop`), notification-area icon with Show / Start / Stop / Recenter / Quit, manual pinning from the taskbar or a Start Menu shortcut helper |
| Efficiency | Event-driven idle UI, one health timer floored at 500 ms, an animation timer that exists only during a transition, one off-screen buffer per window size |

## Repository map

```
src/app/            spatial_desk.cpp (engine), engine_protocol.h (controller contract)
src/app_shell/      controller: window, tray, engine client, diagnostics, app model, theme
src/imu/            HID protocol, fusion, pose estimator, orientation calibration
src/render/         camera frame maths, renderer, screen geometry
src/capture/        DXGI Desktop Duplication (+ GDI fallback)
src/vdd/            Parsec VDD client and display configuration
src/layout/         layout schema, validation, atomic save
src/tools/          probes and the test binaries (see below)
config/             app.json, layouts/*.json, orientation.json (per device, not committed)
resources/          version resource + DPI manifest (the icon is drawn at runtime)
scripts/            portable staging, Start Menu shortcut helper
docs/               WINDOWS-APP.md, PROTOCOL-NOTES.md, orientation-calibration.md, notices
```

## Tests

| Suite | Covers |
|---|---|
| `camera_selftest` | frame conversion, the load-bearing sign triple |
| `pose_selftest` | pose regression, startup calibration, drift absorption, pan-and-settle |
| `pose_scenarios` | fourteen synthetic worn-head scenarios through the real estimator |
| `orientation_calibration_selftest` | guided calibration maths, rejection gates, file round-trip |
| `protocol_selftest` | 66/99 framing and `99 65` decoding |
| `layout_selftest` | layout parsing/validation, geometry, capture-policy constraints |
| `vdd_selftest` | Parsec VDD protocol, cleanup order, index parsing |
| `view_selftest` | offline "virtual glasses" projection with numeric assertions and PPM frames |
| `app_selftest` | controller model: config validation/round-trip, atomic save, presets, screen add/remove invariants, field normalisation, log rotation, engine command constants, status-file and telemetry parsing |

Invariants, bug history and the estimator trade-offs live in `AGENTS.md`; protocol details live in
`docs/PROTOCOL-NOTES.md`. The controller does not change pose fusion, camera-frame handling,
calibration maths or drift behaviour, and `app_selftest` is the only new test suite.
