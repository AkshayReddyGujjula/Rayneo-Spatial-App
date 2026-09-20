#include "vdd/display_config.h"
#include "vdd/vdd_client.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

}  // namespace

int main() {
    std::printf("vdd_selftest: protocol serialization\n");
    const auto remove = gt::vdd_remove_payload(7);
    check(remove[0] == 0 && remove[1] == 7, "remove index uses the driver's two-byte big-endian form");
    bool remaining_zero = true;
    for (size_t i = 2; i < remove.size(); ++i) remaining_zero = remaining_zero && remove[i] == 0;
    check(remaining_zero, "remove payload padding is zero-filled");

    const std::vector<int> order = gt::vdd_cleanup_order({2, 5, 1});
    check(order == std::vector<int>({1, 5, 2}), "display cleanup reverses addition order");

    const gt::VddDriverStatus status = gt::VddClient::driver_status();
    std::printf("  detected driver status: %s\n", gt::vdd_driver_status_text(status));
    check(status != gt::VddDriverStatus::Inaccessible, "Windows display class is accessible");

    std::printf("vdd_selftest: display identity and mode planning\n");
    check(gt::parse_vdd_driver_index(
              LR"(\\?\DISPLAY#PSCCDD0#5&abc&UID263#{e6f07b5f-ffff})") == 7,
          "monitor UID maps back to driver index");
    check(gt::parse_vdd_driver_index(
              LR"(\\?\display#psccdd0#5&abc&uid263#{e6f07b5f-ffff})") == 7,
          "monitor UID parsing is case-insensitive");
    check(gt::choose_refresh_rate({24, 60, 120, 144}, 120) == 120,
          "preferred refresh rate wins when available");
    check(gt::choose_refresh_rate({24, 60, 144}, 120) == 60,
          "refresh selection falls back without exceeding the preference");

    std::printf("vdd_selftest: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
