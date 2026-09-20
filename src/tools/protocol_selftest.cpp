#include "imu/gt_protocol.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) {
        ++failures;
    }
}

void put_float(std::array<uint8_t, 64>& report, size_t offset, float value) {
    std::memcpy(report.data() + offset, &value, sizeof(value));
}

std::array<uint8_t, 64> valid_imu_report() {
    std::array<uint8_t, 64> report{};
    report[0] = 0x99;
    report[1] = 0x65;
    report[2] = gt::kBoardGt;
    put_float(report, 4, 0.1f);
    put_float(report, 8, -0.2f);
    put_float(report, 12, 9.81f);
    put_float(report, 16, 1.0f);
    put_float(report, 20, 2.0f);
    put_float(report, 24, 3.0f);
    put_float(report, 28, 39.5f);
    put_float(report, 32, 11.0f);
    put_float(report, 36, 12.0f);
    const uint32_t tick = 1234567;
    std::memcpy(report.data() + 40, &tick, sizeof(tick));
    put_float(report, 52, 13.0f);
    return report;
}

}  // namespace

int main() {
    std::printf("protocol_selftest: command framing\n");
    uint8_t command[64];
    gt::build_command(gt::kCmdStreamOn, command);
    check(command[0] == 0x00 && command[1] == 0x66 && command[2] == gt::kCmdStreamOn,
          "output report includes report ID, tag, and opcode");
    bool tail_zero = true;
    for (size_t i = 3; i < sizeof(command); ++i) {
        tail_zero = tail_zero && command[i] == 0;
    }
    check(tail_zero, "command tail is zero-filled");

    std::printf("protocol_selftest: IMU validation\n");
    auto bytes = valid_imu_report();
    const gt::Report valid = gt::decode_report(bytes.data(), bytes.size());
    check(valid.kind == gt::ReportKind::Imu && valid.imu.tick_100us == 1234567,
          "finite 99 65 report decodes");
    check(std::fabs(valid.imu.accel_mps2.z - 9.81f) < 1e-5f &&
              std::fabs(valid.imu.mag_ut.z - 13.0f) < 1e-5f,
          "documented sensor offsets decode correctly");

    put_float(bytes, 16, std::numeric_limits<float>::quiet_NaN());
    const gt::Report invalid = gt::decode_report(bytes.data(), bytes.size());
    check(invalid.kind == gt::ReportKind::Other, "non-finite sensor report is rejected");

    std::array<uint8_t, 65> with_report_id{};
    std::memcpy(with_report_id.data() + 1, valid_imu_report().data(), 64);
    const gt::Report prefixed = gt::decode_report(with_report_id.data(), with_report_id.size());
    check(prefixed.kind == gt::ReportKind::Imu, "optional leading HID report ID is accepted");

    std::printf("protocol_selftest: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL",
                failures);
    return failures == 0 ? 0 : 1;
}
