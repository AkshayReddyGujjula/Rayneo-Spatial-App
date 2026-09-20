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

add_library(rayneo_core STATIC
  src/imu/gt_protocol.cpp
  src/imu/gt_hid.cpp
  src/imu/fusion.cpp
  src/imu/orientation_calibration.cpp
  src/imu/pose_estimator.cpp
)
target_include_directories(rayneo_core PUBLIC src)
target_link_libraries(rayneo_core PUBLIC hidapi::hidapi)

add_executable(gt_imu_probe src/tools/gt_imu_probe.cpp)
target_link_libraries(gt_imu_probe PRIVATE rayneo_core)

add_executable(spatial_desk
  src/app/spatial_desk.cpp
  src/imu/imu_source.cpp
  src/render/camera.cpp
  src/render/renderer.cpp
)
target_link_libraries(spatial_desk PRIVATE rayneo_core rayneo_layout d3d11 dxgi d3dcompiler user32 gdi32)

add_executable(camera_selftest
  src/tools/camera_selftest.cpp
  src/render/camera.cpp
)
target_link_libraries(camera_selftest PRIVATE rayneo_core)
add_test(NAME camera_selftest COMMAND camera_selftest)

add_executable(pose_selftest src/tools/pose_selftest.cpp)
target_link_libraries(pose_selftest PRIVATE rayneo_core)
add_test(NAME pose_selftest COMMAND pose_selftest)

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
