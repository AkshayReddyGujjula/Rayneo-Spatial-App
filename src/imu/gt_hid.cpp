#include "imu/gt_hid.h"

#include <hidapi.h>

#include <cstdio>

namespace gt {
namespace {

std::string narrow(const char* s) {
    return s != nullptr ? std::string(s) : std::string();
}

[[maybe_unused]] std::string narrow(const wchar_t* s) {
    if (s == nullptr) {
        return {};
    }
    std::string out;
    for (const wchar_t* p = s; *p != 0; ++p) {
        const unsigned long c = static_cast<unsigned long>(*p);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xe0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        }
    }
    return out;
}

}  // namespace

std::vector<HidDeviceInfo> GtHidDevice::enumerate() {
    std::vector<HidDeviceInfo> out;
    hid_device_info* list = hid_enumerate(kVendorId, kProductId);
    for (hid_device_info* it = list; it != nullptr; it = it->next) {
        HidDeviceInfo info;
        info.path = narrow(it->path);
        info.vendor_id = it->vendor_id;
        info.product_id = it->product_id;
        info.interface_number = it->interface_number;
        info.usage_page = it->usage_page;
        info.usage = it->usage;
        info.release_number = it->release_number;
        info.product = narrow(it->product_string);
        info.manufacturer = narrow(it->manufacturer_string);
        info.serial = narrow(it->serial_number);
        out.push_back(std::move(info));
    }
    hid_free_enumeration(list);
    return out;
}

std::string GtHidDevice::describe(const HidDeviceInfo& info) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "vid=%04x pid=%04x iface=%d usage_page=%04x usage=%04x product=\"%s\" serial=\"%s\" path=%s",
                  info.vendor_id, info.product_id, info.interface_number, info.usage_page, info.usage,
                  info.product.c_str(), info.serial.c_str(), info.path.c_str());
    return std::string(buf);
}

GtHidDevice::~GtHidDevice() {
    close();
}

bool GtHidDevice::open(const std::string& path) {
    close();
    device_ = hid_open_path(path.c_str());
    return device_ != nullptr;
}

bool GtHidDevice::open_first() {
    const auto devices = enumerate();
    if (devices.empty()) {
        return false;
    }
    return open(devices.front().path);
}

void GtHidDevice::close() {
    if (device_ != nullptr) {
        hid_close(device_);
        device_ = nullptr;
    }
}

bool GtHidDevice::write_report(const uint8_t* data, size_t length) {
    if (device_ == nullptr) {
        return false;
    }
    return hid_write(device_, data, length) >= 0;
}

int GtHidDevice::read_report(uint8_t* data, size_t length, int timeout_ms) {
    if (device_ == nullptr) {
        return -1;
    }
    return hid_read_timeout(device_, data, length, timeout_ms);
}

}  // namespace gt
