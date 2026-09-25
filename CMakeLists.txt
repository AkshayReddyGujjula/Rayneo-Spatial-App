cmake_minimum_required(VERSION 3.25)
project(rayneo_spatial LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:preprocessor)
endif()

if(WIN32)
  add_compile_definitions(UNICODE _UNICODE)
endif()

find_package(hidapi CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(rayneo_layout STATIC
  src/layout/layout.cpp
  src/render/screen_geometry.cpp
)
target_include_directories(rayneo_layout PUBLIC src)
target_link_libraries(rayneo_layout PUBLIC nlohmann_json::nlohmann_json)

add_library(rayneo_vdd STATIC
  src/vdd/display_config.cpp
  src/vdd/vdd_client.cpp
)
target_include_directories(rayneo_vdd PUBLIC src)
target_link_libraries(rayneo_vdd PUBLIC setupapi cfgmgr32)

add_library(rayneo_capture STATIC
  src/capture/desktop_duplication.cpp
)
target_include_directories(rayneo_capture PUBLIC src)
target_link_libraries(rayneo_capture PUBLIC d3d11 dxgi)

add_library(rayneo_core STATIC
  src/imu/gt_protocol.cpp
  src/imu/gt_hid.cpp
  src/imu/fusion.cpp
  src/imu/orientation_calibration.cpp
  src/imu/pose_estimator.cpp
  src/imu/pose_smoother.cpp
  src/imu/mag_heading.cpp
)
target_include_directories(rayneo_core PUBLIC src)
target_link_libraries(rayneo_core PUBLIC hidapi::hidapi)

add_executable(imu_replay src/tools/imu_replay.cpp)
target_link_libraries(imu_replay PRIVATE rayneo_core)

add_executable(gt_imu_probe src/tools/gt_imu_probe.cpp)
target_link_libraries(gt_imu_probe PRIVATE rayneo_core)

add_executable(spatial_desk
  src/app/spatial_desk.cpp
  src/app/engine_protocol.cpp
  src/imu/imu_source.cpp
  src/render/camera.cpp
  src/render/renderer.cpp
)
target_link_libraries(spatial_desk PRIVATE rayneo_core rayneo_layout rayneo_vdd rayneo_capture d3d11 dxgi d3dcompiler user32 gdi32)

# Controller application model: pure logic, no windows, covered by app_selftest.
# It owns the preference file, the layout editing rules, the engine command
# contract and the telemetry tail reader.
add_library(rayneo_app_model STATIC
  src/app/engine_protocol.cpp
  src/app_shell/app_config.cpp
  src/app_shell/app_layout.cpp
  src/app_shell/engine_commands.cpp
  src/app_shell/telemetry.cpp
)
target_include_directories(rayneo_app_model PUBLIC src)
target_link_libraries(rayneo_app_model PUBLIC nlohmann_json::nlohmann_json rayneo_layout)

# Controller shell: Win32 window, tray lifecycle, engine process owner and the
# read-only health checks. Inbox Windows libraries only.
add_library(rayneo_app_ui STATIC
  src/app_shell/app_state.cpp
  src/app_shell/app_window.cpp
  src/app_shell/diagnostics.cpp
  src/app_shell/engine_client.cpp
  src/app_shell/paths.cpp
  src/app_shell/tray_icon.cpp
  src/app_shell/ui_paint.cpp
  src/app_shell/ui_theme.cpp
  src/app_shell/ui_widgets.cpp
)
target_include_directories(rayneo_app_ui PUBLIC src)
target_link_libraries(rayneo_app_ui PUBLIC rayneo_app_model rayneo_core rayneo_vdd user32 gdi32 shell32)
target_compile_definitions(rayneo_app_ui PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)

# The controller is a GUI application; spatial_desk.exe stays the renderer engine.
# The resource script is compiled only when a resource compiler is available
# (rc.exe inside the MSVC environment), so a toolchain without RC still builds.
if(WIN32)
  enable_language(RC OPTIONAL)
endif()

add_executable(rayneo_spatial_app WIN32
  src/app_shell/main.cpp
)
if(WIN32 AND CMAKE_RC_COMPILER)
  target_sources(rayneo_spatial_app PRIVATE resources/rayneo_spatial.rc)
  set_source_files_properties(resources/rayneo_spatial.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest;${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico"
  )
endif()
set_target_properties(rayneo_spatial_app PROPERTIES
  OUTPUT_NAME "RayNeo Spatial"
  WIN32_EXECUTABLE TRUE
)
# The .rc already embeds the application manifest (ID 1); suppress the
# linker's auto-generated manifest to avoid a CVT1100 duplicate resource.
target_link_options(rayneo_spatial_app PRIVATE "/MANIFEST:NO")
target_link_libraries(rayneo_spatial_app PRIVATE rayneo_app_ui)
# On the resource compiler's include path so the manifest resolves from the
# source tree rather than from the build directory.
target_include_directories(rayneo_spatial_app PRIVATE resources)

add_executable(camera_selftest
  src/tools/camera_selftest.cpp
  src/render/camera.cpp
)
target_link_libraries(camera_selftest PRIVATE rayneo_core)
add_test(NAME camera_selftest COMMAND camera_selftest)

add_executable(pose_selftest src/tools/pose_selftest.cpp)
target_link_libraries(pose_selftest PRIVATE rayneo_core)
add_test(NAME pose_selftest COMMAND pose_selftest)

add_executable(mag_heading_selftest src/tools/mag_heading_selftest.cpp)
target_link_libraries(mag_heading_selftest PRIVATE rayneo_core)
add_test(NAME mag_heading_selftest COMMAND mag_heading_selftest)

add_executable(pose_scenarios src/tools/pose_scenarios.cpp)
target_link_libraries(pose_scenarios PRIVATE rayneo_core)
add_test(NAME pose_scenarios COMMAND pose_scenarios)

add_executable(orientation_calibration_selftest src/tools/orientation_calibration_selftest.cpp)
target_link_libraries(orientation_calibration_selftest PRIVATE rayneo_core)
add_test(NAME orientation_calibration_selftest COMMAND orientation_calibration_selftest)

add_executable(orientation_calibrate src/tools/orientation_calibrate.cpp)
target_link_libraries(orientation_calibrate PRIVATE rayneo_core)

add_executable(protocol_selftest src/tools/protocol_selftest.cpp)
target_link_libraries(protocol_selftest PRIVATE rayneo_core)
add_test(NAME protocol_selftest COMMAND protocol_selftest)

add_executable(layout_selftest src/tools/layout_selftest.cpp)
target_link_libraries(layout_selftest PRIVATE rayneo_layout)
add_test(NAME layout_selftest COMMAND layout_selftest)

add_executable(vdd_selftest src/tools/vdd_selftest.cpp)
target_link_libraries(vdd_selftest PRIVATE rayneo_vdd)
add_test(NAME vdd_selftest COMMAND vdd_selftest)

add_executable(capture_smoketest src/tools/capture_smoketest.cpp)
target_link_libraries(capture_smoketest PRIVATE rayneo_capture)

add_executable(view_selftest
  src/tools/view_selftest.cpp
  src/render/camera.cpp
)
target_link_libraries(view_selftest PRIVATE rayneo_core rayneo_layout)
add_test(NAME view_selftest COMMAND view_selftest "${CMAKE_SOURCE_DIR}/scratch")

add_executable(app_selftest src/tools/app_selftest.cpp)
target_link_libraries(app_selftest PRIVATE rayneo_app_model)
add_test(NAME app_selftest COMMAND app_selftest)
