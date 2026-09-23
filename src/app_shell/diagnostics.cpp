#include "app_shell/diagnostics.h"

#include "app_shell/app_layout.h"
#include "imu/gt_hid.h"
#include "imu/orientation_calibration.h"
#include "layout/layout.h"
#include "vdd/vdd_client.h"

#include <hidapi.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>

namespace gt {
namespace {

std::wstring lower_copy(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& character : out) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return out;
}

BOOL CALLBACK monitor_enum_proc(HMONITOR handle, HDC, LPRECT, LPARAM data) {
    auto* list = reinterpret_cast<std::vector<DisplayInfo>*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) {
        return TRUE;
    }
    DisplayInfo entry;
    entry.device_name = info.szDevice;
    entry.width = info.rcMonitor.right - info.rcMonitor.left;
    entry.height = info.rcMonitor.bottom - info.rcMonitor.top;
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    DISPLAY_DEVICEW monitor{};
    monitor.cb = sizeof(monitor);
    if (EnumDisplayDevicesW(info.szDevice, 0, &monitor, 0)) {
        entry.description = monitor.DeviceString;
        const std::wstring searchable = lower_copy(entry.description + L" " + monitor.DeviceID);
        entry.glasses = searchable.find(L"smartglasses") != std::wstring::npos ||
                        searchable.find(L"rayneo") != std::wstring::npos ||
                        (!entry.primary && searchable.find(L"tcl") != std::wstring::npos);
        entry.virtual_display = searchable.find(L"parsec") != std::wstring::npos ||
                                searchable.find(L"psccdd") != std::wstring::npos ||
                                searchable.find(L"vda") != std::wstring::npos;
    }
    list->push_back(std::move(entry));
    return TRUE;
}

std::wstring join_rows(const std::array<float, 9>& matrix) {
    wchar_t buffer[256];
    std::swprintf(buffer, std::size(buffer),
                  L"right (%+.3f, %+.3f, %+.3f)  forward (%+.3f, %+.3f, %+.3f)  "
                  L"up (%+.3f, %+.3f, %+.3f)",
                  static_cast<double>(matrix[0]), static_cast<double>(matrix[1]),
                  static_cast<double>(matrix[2]), static_cast<double>(matrix[3]),
                  static_cast<double>(matrix[4]), static_cast<double>(matrix[5]),
                  static_cast<double>(matrix[6]), static_cast<double>(matrix[7]),
                  static_cast<double>(matrix[8]));
    return std::wstring(buffer);
}

}  // namespace

bool read_text_file(const std::filesystem::path& path, std::string& text, std::string& error) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) {
        error = "could not open file";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    text = buffer.str();
    return true;
}

bool read_text_tail(const std::filesystem::path& path, uint64_t max_bytes, std::string& text,
                    std::string& error) {
    std::error_code size_error;
    const uint64_t size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        error = "could not read file size";
        return false;
    }
    const uint64_t start = size > max_bytes ? size - max_bytes : 0;
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) {
        error = "could not open file";
        return false;
    }
    if (start > 0) {
        input.seekg(static_cast<std::streamoff>(start), std::ios::beg);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    text = buffer.str();
    return true;
}

std::wstring file_timestamp_text(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(path, error);
    if (error) {
        return L"unknown time";
    }
    const std::filesystem::file_time_type::duration age =
        std::filesystem::file_time_type::clock::now() - stamp;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(age).count();
    if (seconds < 0) {
        return L"in the future";
    }
    wchar_t buffer[96];
    if (seconds < 90) {
        std::swprintf(buffer, std::size(buffer), L"%lld s ago",
                      static_cast<long long>(seconds));
    } else if (seconds < 5400) {
        std::swprintf(buffer, std::size(buffer), L"%lld min ago",
                      static_cast<long long>(seconds / 60));
    } else if (seconds < 172800) {
        std::swprintf(buffer, std::size(buffer), L"%lld h ago",
                      static_cast<long long>(seconds / 3600));
    } else {
        std::swprintf(buffer, std::size(buffer), L"%lld d ago",
                      static_cast<long long>(seconds / 86400));
    }
    return std::wstring(buffer);
}

std::wstring format_bytes_wide(uint64_t bytes) {
    wchar_t buffer[64];
    if (bytes >= (1u << 20)) {
        std::swprintf(buffer, std::size(buffer), L"%.1f MiB",
                      static_cast<double>(bytes) / static_cast<double>(1u << 20));
    } else if (bytes >= (1u << 10)) {
        std::swprintf(buffer, std::size(buffer), L"%.1f KiB",
                      static_cast<double>(bytes) / static_cast<double>(1u << 10));
    } else {
        std::swprintf(buffer, std::size(buffer), L"%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return std::wstring(buffer);
}

void collect_display_and_vdd(DiagnosticsReport& report) {
    report.displays.clear();
    EnumDisplayMonitors(nullptr, nullptr, monitor_enum_proc,
                        reinterpret_cast<LPARAM>(&report.displays));
    report.glasses_found = false;
    for (const DisplayInfo& display : report.displays) {
        if (display.glasses) {
            report.glasses_found = true;
            wchar_t buffer[256];
            std::swprintf(buffer, std::size(buffer), L"%s  %dx%d", display.device_name.c_str(),
                          display.width, display.height);
            report.glasses_detail = buffer;
            break;
        }
    }
    if (!report.glasses_found) {
        if (report.displays.empty()) {
            report.glasses_detail = L"no displays reported";
        } else {
            report.glasses_detail = L"no RayNeo display; check Extend mode (Win+P)";
        }
    }

    const VddDriverStatus status = VddClient::driver_status();
    report.vdd_status_text = wide_from_utf8(vdd_driver_status_text(status));
    report.vdd_ready = status == VddDriverStatus::Ready;
    if (!report.vdd_ready && status != VddDriverStatus::NotInstalled) {
        report.vdd_status_text += L" (restart Windows if the driver was just installed)";
    }
}

void collect_calibration(const Paths& paths, DiagnosticsReport& report) {
    std::error_code exists_error;
    report.calibration_present = std::filesystem::exists(paths.calibration_path, exists_error) &&
                                 !exists_error;
    std::array<float, 9> matrix{};
    std::string error;
    if (gt::load_orientation_calibration(narrow_utf8(paths.calibration_path), matrix, error)) {
        report.calibration_valid = true;
        report.calibration_detail = L"valid, saved " + file_timestamp_text(paths.calibration_path) +
                                    L" | " + join_rows(matrix);
    } else if (report.calibration_present) {
        report.calibration_valid = false;
        report.calibration_detail = L"unreadable: " + wide_from_utf8(error);
    } else {
        report.calibration_valid = false;
        report.calibration_detail = L"missing; run orientation_calibrate.exe";
    }
}

void collect_hid(DiagnosticsReport& report) {
    if (hid_init() != 0) {
        report.hid_device_count = 0;
        report.hid_detail = L"hidapi could not start";
        return;
    }
    const std::vector<HidDeviceInfo> devices = GtHidDevice::enumerate();
    report.hid_device_count = static_cast<int>(devices.size());
    if (devices.empty()) {
        report.hid_detail = L"no RayNeo GT interface (VID 3941, PID AF50)";
        return;
    }
    const HidDeviceInfo& first = devices.front();
    wchar_t buffer[256];
    std::swprintf(buffer, std::size(buffer), L"%llu interface(s): %s usage %04x/%04x",
                  static_cast<unsigned long long>(devices.size()),
                  wide_from_utf8(first.product).c_str(), first.usage_page, first.usage);
    report.hid_detail = buffer;
}

void collect_layout_file(const Paths& paths, DiagnosticsReport& report) {
    Layout layout;
    std::string error;
    if (gt::load_layout(narrow_utf8(paths.layout_path), layout, error)) {
        report.layout_ok = true;
        report.layout_detail = L"valid, " + wide_from_utf8(layout_summary(layout)) + L", saved " +
                              file_timestamp_text(paths.layout_path);
        return;
    }
    report.layout_ok = false;
    std::error_code exists_error;
    const bool present = std::filesystem::exists(paths.layout_path, exists_error) && !exists_error;
    if (present) {
        report.layout_detail = L"invalid: " + wide_from_utf8(error);
    } else {
        report.layout_detail = L"missing; save a layout to create it";
    }
}

void collect_telemetry(const Paths& paths, DiagnosticsReport& report) {
    report.telemetry_path = paths.telemetry_log.wstring();
    report.telemetry_present = false;
    std::error_code exists_error;
    if (!std::filesystem::exists(paths.telemetry_log, exists_error) || exists_error) {
        report.telemetry_detail = L"no telemetry yet; start the engine with logging enabled";
        return;
    }
    std::string tail;
    std::string error;
    if (!read_text_tail(paths.telemetry_log, 64u << 10, tail, error)) {
        report.telemetry_detail = L"could not read telemetry: " + wide_from_utf8(error);
        return;
    }
    TelemetrySample sample;
    if (!parse_telemetry_tail(tail, sample, error)) {
        report.telemetry_detail = L"no telemetry rows: " + wide_from_utf8(error);
        return;
    }
    report.telemetry = sample;
    report.telemetry_present = true;
    wchar_t buffer[256];
    std::swprintf(buffer, std::size(buffer),
                  L"yaw %+.2f  pitch %+.2f  roll %+.2f | bias (%+.2f, %+.2f, %+.2f) deg/s | %s | "
                  L"adapt %s  dev %.2f deg/s",
                  static_cast<double>(sample.yaw_deg), static_cast<double>(sample.pitch_deg),
                  static_cast<double>(sample.roll_deg), static_cast<double>(sample.bias[0]),
                  static_cast<double>(sample.bias[1]), static_cast<double>(sample.bias[2]),
                  sample.rest ? L"rest" : (sample.still ? L"still" : L"moving"),
                  adapt_state_label(sample.adapt_state),
                  static_cast<double>(sample.stillness_degs));
    report.telemetry_detail = buffer;
}

void collect_engine_log(const Paths& paths, DiagnosticsReport& report) {
    report.engine_log_path = paths.engine_log.wstring();
    std::error_code exists_error;
    if (!std::filesystem::exists(paths.engine_log, exists_error) || exists_error) {
        report.engine_log_tail.clear();
        return;
    }
    std::string tail;
    std::string error;
    if (!read_text_tail(paths.engine_log, 16u << 10, tail, error)) {
        report.engine_log_tail = L"could not read the engine log: " + wide_from_utf8(error);
        return;
    }
    std::vector<std::wstring> lines;
    std::string current;
    for (const char character : tail) {
        if (character == '\n') {
            lines.push_back(wide_from_utf8(current));
            current.clear();
        } else if (character != '\r') {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        lines.push_back(wide_from_utf8(current));
    }
    const size_t keep = 8;
    const size_t start = lines.size() > keep ? lines.size() - keep : 0;
    std::wstring joined;
    for (size_t index = start; index < lines.size(); ++index) {
        if (!joined.empty()) {
            joined += L"\n";
        }
        joined += lines[index];
    }
    report.engine_log_tail = joined;
}

void shutdown_hid() {
    hid_exit();
}

}  // namespace gt
