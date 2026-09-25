#include "imu/gt_protocol.h"

#include <cstring>
#include <cmath>

namespace gt {
namespace {

constexpr size_t kReportLength = 64;
constexpr uint8_t kTag0 = 0x99;
constexpr uint8_t kTagAck = 0xc8;
constexpr uint8_t kTagImu = 0x65;

float read_f32(const uint8_t* p) {
    float v = 0.0f;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t read_u32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

Vec3 read_vec3(const uint8_t* p) {
    return Vec3{read_f32(p), read_f32(p + 4), read_f32(p + 8)};
}

}  // namespace

void build_command(uint8_t cmd, uint8_t out[64]) {
    std::memset(out, 0, kReportLength);
    out[0] = 0x00;
    out[1] = 0x66;
    out[2] = cmd;
}

Report decode_report(const uint8_t* data, size_t length) {
    Report r;

    // Some hidapi builds return the HID report id as the leading byte.
    if (length >= 2 && data[0] == 0x00 && data[1] == kTag0) {
        ++data;
        --length;
    }

    r.length = length > sizeof(r.raw) ? sizeof(r.raw) : length;
    std::memcpy(r.raw, data, r.length);

    if (length < 4 || data[0] != kTag0) {
        return r;
    }

    r.board_id = data[2];
    if (data[1] == kTagAck) {
        r.kind = ReportKind::Ack;
        r.ack_cmd = length > 8 ? data[8] : 0;
        return r;
    }
    if (data[1] == kTagImu && length >= kReportLength) {
        r.imu.accel_mps2 = read_vec3(data + 4);
        r.imu.gyro_degs = read_vec3(data + 16);
        r.imu.temp_c = read_f32(data + 28);
        r.imu.mag_ut.x = read_f32(data + 32);
        r.imu.mag_ut.y = read_f32(data + 36);
        r.imu.tick_100us = read_u32(data + 40);
        r.imu.mag_ut.z = read_f32(data + 52);
        const auto finite_vec = [](const Vec3& value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        };
        if (finite_vec(r.imu.accel_mps2) && finite_vec(r.imu.gyro_degs) &&
            finite_vec(r.imu.mag_ut) && std::isfinite(r.imu.temp_c)) {
            r.kind = ReportKind::Imu;
        }
    }
    return r;
}

std::string to_hex(const uint8_t* data, size_t length) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(length * 3);
    for (size_t i = 0; i < length; ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

}  // namespace gt
