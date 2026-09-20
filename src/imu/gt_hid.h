#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct hid_device_;

namespace gt {

struct HidDeviceInfo {
    std::string path;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    int interface_number = -1;
    uint16_t usage_page = 0;
    uint16_t usage = 0;
    uint16_t release_number = 0;
    std::string product;
    std::string manufacturer;
    std::string serial;
};

class GtHidDevice {
public:
    static constexpr uint16_t kVendorId = 0x3941;
    static constexpr uint16_t kProductId = 0xAF50;

    static std::vector<HidDeviceInfo> enumerate();
    static std::string describe(const HidDeviceInfo& info);

    GtHidDevice() = default;
    ~GtHidDevice();

    GtHidDevice(const GtHidDevice&) = delete;
    GtHidDevice& operator=(const GtHidDevice&) = delete;

    bool open(const std::string& path);
    bool open_first();
    void close();
    bool is_open() const { return device_ != nullptr; }

    bool write_report(const uint8_t* data, size_t length);
    int read_report(uint8_t* data, size_t length, int timeout_ms);

private:
    hid_device_* device_ = nullptr;
};

}  // namespace gt
