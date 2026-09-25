# RayNeo Spatial

### A head-tracked Windows desktop for RayNeo GT glasses

RayNeo Spatial turns multiple Windows desktops into a spatial workspace on the RayNeo GT. The glasses' IMU tracks your head in 3DoF; a Direct3D 11 renderer places the screens around you, while a native Windows dashboard controls the layout and engine.

> **Platform:** Windows 10 (1809+) or Windows 11, x64. This is a source-built project; the repository does not include a driver or a prebuilt release.

## What it does

| | |
|---|---|
| **Spatial screens** | Arrange 1–8 screens with configurable yaw, pitch, roll, distance, size, and field of view. The default layout is a three-screen arc at −45°, 0°, and +45°. |
| **Real Windows desktops** | Use the separately installed Parsec Virtual Display Driver (VDD) to create monitors that Windows apps can use. |
| **Head tracking** | Decode the GT's IMU stream, calibrate the sensor-to-head orientation, estimate pose, and recenter from the dashboard or hotkey. |
| **Drift-free heading** | An optional one-time magnetometer calibration lets the engine lock yaw to the local magnetic field, so the workspace stays put over hours of use. |
| **Reading stabilisation** | A soft hold keeps small text steady while your head is still (Off, Low, Medium, High, Ultra) without making deliberate turns feel sticky. |
| **View comfort** | Optional per-screen dimming (manual, or dim the screens you are not looking at), a warm night tint, and a hotkey that moves the cursor to the centre screen. |
| **Desktop capture** | Capture each virtual monitor with DXGI Desktop Duplication, with a GDI fallback. |
| **Native controller** | Edit layouts, start and stop the workspace, use the tray controls, and inspect device and engine diagnostics. |
| **Preview mode** | Check the renderer without creating virtual monitors. Preview screens are labelled test screens, not Windows desktops. |

### How the pieces fit

```text
RayNeo GT IMU ── HID protocol ── calibration + pose estimation ──┐
                                                                │
Windows apps ── Parsec VDD monitors ── DXGI / GDI capture ──────┼── D3D11 renderer ── GT display
                                                                │
Native dashboard ── layout, commands, status, diagnostics ──────┘
```

`RayNeo Spatial.exe` is the controller. It launches and steers `spatial_desk.exe`, the rendering engine. `orientation_calibrate.exe` creates the per-device calibration file; `gt_imu_probe.exe` is a protocol diagnostic tool.

## Get started

### 1. Prepare the machine

- Connect RayNeo GT glasses and set Windows projection to **Extend** (`Win+P`). The glasses need to be an active, separate display.
- Install Visual Studio 2022 Build Tools with the C++ toolchain, CMake 3.25+, Ninja, and [vcpkg](https://github.com/microsoft/vcpkg). The vcpkg manifest installs `hidapi` and `nlohmann-json` during configuration.
- To use **Start workspace**, install the signed Parsec VDD separately. The driver is not bundled or installed by this project. You can use **Start preview** without it.

### 2. Build and test

From a **Developer Command Prompt for VS 2022**, set `VCPKG_ROOT` to your vcpkg checkout, then run:

```cmd
set "VCPKG_ROOT=C:\path\to\vcpkg"
cmake -S . -B build\rayneo -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
cmake --build build\rayneo
ctest --test-dir build\rayneo --output-on-failure
```

Use a dedicated Ninja build directory if another developer or agent is building this repository. The checked-in CMake preset also reads `VCPKG_ROOT` if you prefer `cmake --preset default` for a solo build.

### 3. Run the app

Stage the controller, engine, config, and required DLL in one portable folder:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\stage-portable.ps1 -BuildDir build\rayneo
```

Run `dist\RayNeoSpatial\RayNeo Spatial.exe`. Follow the dashboard's **Run calibration** action while wearing the glasses, then choose **Start workspace** for real virtual desktops or **Start preview** for a renderer check. For drift-free heading, run `orientation_calibrate.exe --mag` once from the same folder (turn fully around and nod while wearing the glasses). Calibration is saved as `config/orientation.json` in the running folder and is intentionally excluded from Git.

| Hotkey | Action |
|---|---|
| `Ctrl+Shift+R` | Recenter the workspace in front of you |
| `Ctrl+Alt+S` | Cycle reading stabilisation (Off / Low / Medium / High / Ultra) |
| `Ctrl+Alt+F` | Move the cursor to the middle of the centre screen |
| `Ctrl+Alt+Y` / `Ctrl+Alt+P` | Toggle yaw / pitch tracking (off holds the view on that axis) |
| `Ctrl+Shift+\` | Exit workspace mode |
| `Ctrl+Alt+Q` | Quit the engine |

See [Windows app guide](docs/WINDOWS-APP.md) for controls, layout editing, diagnostics, recovery, and manual hardware checks.

## Design notes

The sensor quaternion is right-handed with Z up; the Direct3D scene is left-handed with Y up. The camera converts between those frames and uses an earth-frame relative rotation so recentering while tilted does not mix yaw into pitch and roll. The sensor mounting comes from a measured calibration file rather than a hard-coded axis guess.

The pose path remains live even during tiny head movements. Rest detection controls gyro-bias adaptation, not whether the pose is published. With a magnetometer calibration, a rate-limited heading lock removes the gyro's slow thermal yaw drift; without one, a very slow steady yaw and gyro bias cannot always be distinguished, so the estimator favors preserving deliberate pans and exposes recentering when needed. Display-side smoothing and the reading hold run after the estimator and never feed back into it. The measured scenarios, the bug history and the tuning trade-offs are documented in [AGENTS.md](AGENTS.md).

## Repository guide

| Path | Purpose |
|---|---|
| `src/app_shell/` | Native Win32 dashboard, tray, lifecycle, diagnostics |
| `src/app/` | Renderer engine and controller command protocol |
| `src/imu/` | GT HID protocol, calibration, fusion, pose estimator |
| `src/render/` | Camera transform, screen geometry, D3D11 renderer |
| `src/capture/` | DXGI capture and GDI fallback |
| `src/vdd/` | Parsec VDD client and display topology |
| `src/layout/` | Layout schema, validation, persistence |
| `src/tools/` | Calibration, probe, and regression executables |
| `docs/` | [Windows app guide](docs/WINDOWS-APP.md), [protocol notes](docs/PROTOCOL-NOTES.md), [calibration](docs/orientation-calibration.md), [third-party notices](docs/THIRD_PARTY_NOTICES.md) |

Ten CTest suites cover camera frames, pose behavior, synthetic head-motion scenarios, calibration, protocol decoding, layout, VDD logic, projection, the controller model, and the magnetometer heading lock. They are repeatable offline checks; actual display, driver, and worn-glasses behavior requires the hardware setup above.

## Dependencies and attribution

The app uses [hidapi](https://github.com/libusb/hidapi), [nlohmann/json](https://github.com/nlohmann/json), Windows APIs, and the protocol of the separately installed [Parsec VDD](https://github.com/nomi-san/parsec-vdd). See [third-party notices](docs/THIRD_PARTY_NOTICES.md). RayNeo Spatial is an independent project and is not an official RayNeo product.

## License

Copyright 2026 Akshay Reddy Gujjula. Licensed under the [Apache License 2.0](LICENSE).
