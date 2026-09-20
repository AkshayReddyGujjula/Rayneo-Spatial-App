#include "vdd/vdd_client.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <memory>

namespace gt {
namespace {

constexpr GUID kAdapterInterfaceGuid = {0x00b41627,
                                        0x04c4,
                                        0x429e,
                                        {0xa2, 0x6e, 0x02, 0x65, 0xcf, 0x50, 0xc8, 0xfa}};
constexpr GUID kDisplayClassGuid = {0x4d36e968,
                                    0xe325,
                                    0x11ce,
                                    {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr wchar_t kHardwareId[] = L"Root\\Parsec\\VDA";
constexpr uint32_t kIoctlAdd = 0x0022e004;
constexpr uint32_t kIoctlRemove = 0x0022a008;
constexpr uint32_t kIoctlUpdate = 0x0022a00c;
constexpr uint32_t kIoctlVersion = 0x0022e010;
// The Parsec driver can create up to 16 displays per adapter, but the
// reference API caps at 8 to avoid plugging lag.
constexpr int kMaximumDisplays = 8;

HANDLE native_handle(void* handle) {
    return static_cast<HANDLE>(handle);
}

bool equals_hardware_id(const wchar_t* multi_string, DWORD byte_count) {
    if (multi_string == nullptr) {
        return false;
    }
    const wchar_t* end = reinterpret_cast<const wchar_t*>(
        reinterpret_cast<const uint8_t*>(multi_string) + byte_count);
    for (const wchar_t* item = multi_string; item < end && *item != L'\0';
         item += std::wcslen(item) + 1) {
        if (_wcsicmp(item, kHardwareId) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

const char* vdd_driver_status_text(VddDriverStatus status) {
    switch (status) {
        case VddDriverStatus::Ready:
            return "ready";
        case VddDriverStatus::NotInstalled:
            return "not installed";
        case VddDriverStatus::Disabled:
            return "disabled";
        case VddDriverStatus::RestartRequired:
            return "restart required";
        case VddDriverStatus::DriverError:
            return "driver error";
        case VddDriverStatus::Inaccessible:
            return "inaccessible";
        case VddDriverStatus::Unknown:
            return "unknown";
    }
    return "unknown";
}

std::array<uint8_t, 32> vdd_remove_payload(int driver_index) {
    std::array<uint8_t, 32> payload{};
    if (driver_index >= 0 && driver_index <= 255) {
        payload[1] = static_cast<uint8_t>(driver_index);
    }
    return payload;
}

std::vector<int> vdd_cleanup_order(const std::vector<int>& addition_order) {
    return std::vector<int>(addition_order.rbegin(), addition_order.rend());
}

VddClient::~VddClient() {
    disconnect();
}

VddDriverStatus VddClient::driver_status() {
    HDEVINFO devices = SetupDiGetClassDevsW(&kDisplayClassGuid, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) {
        return VddDriverStatus::Inaccessible;
    }

    VddDriverStatus result = VddDriverStatus::NotInstalled;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &info); ++index) {
        DWORD required = 0;
        SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_HARDWAREID, nullptr, nullptr, 0,
                                          &required);
        if (required == 0) {
            continue;
        }
        std::vector<uint8_t> buffer(required);
        DWORD type = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_HARDWAREID, &type,
                                               buffer.data(), required, nullptr) ||
            (type != REG_SZ && type != REG_MULTI_SZ) ||
            !equals_hardware_id(reinterpret_cast<const wchar_t*>(buffer.data()), required)) {
            continue;
        }

        ULONG status = 0;
        ULONG problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) != CR_SUCCESS) {
            result = VddDriverStatus::Unknown;
        } else if ((status & (DN_DRIVER_LOADED | DN_STARTED)) != 0) {
            result = VddDriverStatus::Ready;
        } else if ((status & DN_HAS_PROBLEM) != 0) {
            if (problem == CM_PROB_NEED_RESTART) {
                result = VddDriverStatus::RestartRequired;
            } else if (problem == CM_PROB_DISABLED || problem == CM_PROB_HARDWARE_DISABLED ||
                       problem == CM_PROB_DISABLED_SERVICE) {
                result = VddDriverStatus::Disabled;
            } else {
                result = VddDriverStatus::DriverError;
            }
        } else {
            result = VddDriverStatus::Unknown;
        }
        break;
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

bool VddClient::open(std::string& error) {
    HDEVINFO devices = SetupDiGetClassDevsW(&kAdapterInterfaceGuid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        error = "could not enumerate the Parsec VDD interface";
        return false;
    }
    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &kAdapterInterfaceGuid, 0, &interface_data)) {
        SetupDiDestroyDeviceInfoList(devices);
        error = "Parsec VDD interface is not present";
        return false;
    }
    DWORD detail_size = 0;
    SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0, &detail_size, nullptr);
    if (detail_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
        SetupDiDestroyDeviceInfoList(devices);
        error = "Parsec VDD interface path is unavailable";
        return false;
    }
    std::vector<uint8_t> detail_storage(detail_size);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_storage.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, detail_size, nullptr,
                                          nullptr)) {
        SetupDiDestroyDeviceInfoList(devices);
        error = "could not read the Parsec VDD interface path";
        return false;
    }
    HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING |
                                    FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH,
                                nullptr);
    SetupDiDestroyDeviceInfoList(devices);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        error = "could not open the Parsec VDD device (Windows error " +
                std::to_string(GetLastError()) + ")";
        return false;
    }
    handle_ = handle;
    return true;
}

void VddClient::close() {
    if (handle_ != nullptr) {
        CloseHandle(native_handle(handle_));
        handle_ = nullptr;
    }
}

bool VddClient::ioctl(uint32_t code, const std::array<uint8_t, 32>& input, uint32_t timeout_ms,
                      uint32_t* output, std::string* error, bool require_output_size) {
    std::lock_guard<std::mutex> lock(ioctl_mutex_);
    if (handle_ == nullptr) {
        if (error != nullptr) *error = "Parsec VDD handle is not open";
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        if (error != nullptr) *error = "could not create VDD IO event";
        return false;
    }
    uint32_t result = 0;
    DWORD transferred = 0;
    DeviceIoControl(native_handle(handle_), code, const_cast<uint8_t*>(input.data()),
                    static_cast<DWORD>(input.size()), output != nullptr ? &result : nullptr,
                    output != nullptr ? sizeof(result) : 0, nullptr, &overlapped);
    const BOOL completed = GetOverlappedResultEx(native_handle(handle_), &overlapped, &transferred,
                                                 timeout_ms, FALSE);
    const DWORD completion_error = completed ? ERROR_SUCCESS : GetLastError();
    if (!completed) {
        CancelIoEx(native_handle(handle_), &overlapped);
        GetOverlappedResult(native_handle(handle_), &overlapped, &transferred, TRUE);
    }
    CloseHandle(overlapped.hEvent);
    if (!completed) {
        if (error != nullptr) {
            *error = "VDD IOCTL 0x";
            char code_text[16];
            std::snprintf(code_text, sizeof(code_text), "%08x", code);
            *error += code_text;
            *error += " failed (Windows error " + std::to_string(completion_error) + ")";
        }
        return false;
    }
    if (output != nullptr && require_output_size && transferred != sizeof(result)) {
        if (error != nullptr) {
            *error = "VDD IOCTL returned " + std::to_string(transferred) +
                     " bytes; expected 4";
        }
        return false;
    }
    if (output != nullptr) {
        *output = result;
    }
    return true;
}

bool VddClient::query_version(int& version) {
    uint32_t value = 0;
    std::array<uint8_t, 32> input{};
    if (!ioctl(kIoctlVersion, input, 1000, &value, nullptr)) {
        return false;
    }
    version = static_cast<int>(value);
    return true;
}

bool VddClient::ping() {
    std::array<uint8_t, 32> input{};
    uint32_t response = 0;
    return ioctl(kIoctlUpdate, input, 1000, &response, nullptr, false);
}

bool VddClient::add_display(int& index, std::string& error) {
    uint32_t value = 0;
    std::array<uint8_t, 32> input{};
    if (!ioctl(kIoctlAdd, input, 5000, &value, &error) || value >= kMaximumDisplays) {
        if (error.empty()) error = "Parsec VDD returned an invalid display index";
        return false;
    }
    index = static_cast<int>(value);
    ping();
    return true;
}

bool VddClient::remove_display(int index) {
    const auto input = vdd_remove_payload(index);
    uint32_t response = 0;
    const bool removed = ioctl(kIoctlRemove, input, 1000, &response, nullptr, false);
    ping();
    return removed;
}

void VddClient::keepalive_loop() {
    while (keepalive_running_.load(std::memory_order_acquire)) {
        const auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        if (!ping()) {
            keepalive_failures_.fetch_add(1, std::memory_order_relaxed);
            consecutive_keepalive_failures_.fetch_add(1, std::memory_order_relaxed);
        } else {
            consecutive_keepalive_failures_.store(0, std::memory_order_relaxed);
        }
        std::this_thread::sleep_until(next);
    }
}

bool VddClient::connect(size_t display_count, std::string& error) {
    if (display_count == 0 || display_count > static_cast<size_t>(kMaximumDisplays)) {
        error = "VDD display count must be between 1 and " + std::to_string(kMaximumDisplays);
        return false;
    }
    disconnect();
    const VddDriverStatus status = driver_status();
    if (status != VddDriverStatus::Ready) {
        error = std::string("Parsec VDD is ") + vdd_driver_status_text(status);
        return false;
    }
    if (!open(error)) {
        return false;
    }
    if (!query_version(driver_version_)) {
        error = "could not query the Parsec VDD version";
        close();
        return false;
    }
    keepalive_failures_.store(0);
    consecutive_keepalive_failures_.store(0);
    keepalive_running_.store(true, std::memory_order_release);
    keepalive_thread_ = std::thread(&VddClient::keepalive_loop, this);

    if (!resize(display_count, error)) {
        disconnect();
        return false;
    }
    return true;
}

bool VddClient::resize(size_t display_count, std::string& error) {
    if (handle_ == nullptr) {
        error = "Parsec VDD is not connected";
        return false;
    }
    if (display_count == 0 || display_count > static_cast<size_t>(kMaximumDisplays)) {
        error = "VDD display count must be between 1 and " + std::to_string(kMaximumDisplays);
        return false;
    }
    const size_t original_count = display_indices_.size();
    while (display_indices_.size() > display_count) {
        const int index = display_indices_.back();
        if (!remove_display(index)) {
            error = "failed to remove Parsec virtual display " + std::to_string(index);
            while (display_indices_.size() < original_count) {
                int replacement = -1;
                std::string rollback_error;
                if (!add_display(replacement, rollback_error)) {
                    error += "; rollback also failed: " + rollback_error;
                    break;
                }
                display_indices_.push_back(replacement);
            }
            return false;
        }
        display_indices_.pop_back();
    }
    while (display_indices_.size() < display_count) {
        int index = -1;
        if (!add_display(index, error)) {
            while (display_indices_.size() > original_count) {
                const int added_index = display_indices_.back();
                if (!remove_display(added_index)) {
                    error += "; failed to remove partially added display " +
                             std::to_string(added_index);
                    break;
                }
                display_indices_.pop_back();
            }
            return false;
        }
        display_indices_.push_back(index);
    }
    return true;
}

void VddClient::disconnect() {
    if (handle_ != nullptr) {
        for (int index : vdd_cleanup_order(display_indices_)) {
            remove_display(index);
        }
    }
    display_indices_.clear();
    keepalive_running_.store(false, std::memory_order_release);
    if (keepalive_thread_.joinable()) {
        keepalive_thread_.join();
    }
    close();
    driver_version_ = -1;
}

}  // namespace gt
