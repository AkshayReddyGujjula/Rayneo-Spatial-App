#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gt {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ImuSample {
    Vec3 accel_mps2;
    Vec3 gyro_degs;
    Vec3 mag_ut;
    float temp_c = 0.0f;
    uint32_t tick_100us = 0;
    uint64_t host_time_us = 0;
};

enum class ReportKind {
    Imu,
    Ack,
    Other,
};

struct Report {
    ReportKind kind = ReportKind::Other;
    uint8_t board_id = 0;
    uint8_t ack_cmd = 0;
    ImuSample imu;
    uint8_t raw[65] = {};
    size_t length = 0;
};

// RayNeo GT HID framing on Windows: a 64-byte output report carries the
// report id (0x00), the command tag (0x66) and a single opcode byte.
void build_command(uint8_t cmd, uint8_t out[64]);

// Input reports: 99 c8 = command ack (opcode echoed at byte 8),
// 99 65 = 64-byte nine-axis carrier.
Report decode_report(const uint8_t* data, size_t length);

std::string to_hex(const uint8_t* data, size_t length);

inline constexpr uint8_t kCmdDeviceInfo = 0x00;
inline constexpr uint8_t kCmdStreamOn = 0x01;
inline constexpr uint8_t kCmdStreamOff = 0x02;
inline constexpr uint8_t kCmdCalibration = 0x3c;
inline constexpr uint8_t kCmdGyroTempTable = 0x3e;

inline constexpr uint8_t kBoardGt = 0x40;
inline constexpr uint8_t kBoardGtMax = 0x41;

}  // namespace gt
