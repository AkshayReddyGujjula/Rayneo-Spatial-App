#pragma once

// Read-only health checks for the dashboard.
//
// Everything here is observation only: the controller never installs a driver,
// never calls SetDisplayConfig, and never touches the display topology. Display
// and VDD checks answer "what is there", calibration and HID checks answer "can
// head tracking start", and the telemetry/engine-log readers expose the last
// diagnostics the engine wrote.

#include "app_shell/paths.h"
#include "app_shell/telemetry.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gt {

struct DisplayInfo {
    std::wstring device_name;
    std::wstring description;
    int width = 0;
    int height = 0;
    bool primary = false;
    bool glasses = false;
    bool virtual_display = false;
};

struct DiagnosticsReport {
    std::vector<DisplayInfo> displays;
    bool glasses_found = false;
    std::wstring glasses_detail;

    bool calibration_present = false;
    bool calibration_valid = false;
    std::wstring calibration_detail;

    int hid_device_count = 0;
    std::wstring hid_detail;

    std::wstring vdd_status_text;
    bool vdd_ready = false;

    bool layout_ok = false;
    std::wstring layout_detail;

    bool telemetry_present = false;
    TelemetrySample telemetry;
    std::wstring telemetry_detail;
    std::wstring telemetry_path;

    std::wstring engine_log_path;
    std::wstring engine_log_tail;
};

// Display topology plus the Parsec VDD driver state (reads the display class
// registry key through VddClient::driver_status; no device is created).
void collect_display_and_vdd(DiagnosticsReport& report);

// Orientation calibration file: present, loadable, and its mounting basis.
void collect_calibration(const Paths& paths, DiagnosticsReport& report);

// RayNeo GT HID interface discovery (initialises hidapi on first use).
void collect_hid(DiagnosticsReport& report);

// Layout file: present and loadable, with its screen count.
void collect_layout_file(const Paths& paths, DiagnosticsReport& report);

// Newest row of the engine's telemetry CSV.
void collect_telemetry(const Paths& paths, DiagnosticsReport& report);

// Last lines of the captured engine console log.
void collect_engine_log(const Paths& paths, DiagnosticsReport& report);

void shutdown_hid();

bool read_text_tail(const std::filesystem::path& path, uint64_t max_bytes, std::string& text,
                    std::string& error);
bool read_text_file(const std::filesystem::path& path, std::string& text, std::string& error);
std::wstring file_timestamp_text(const std::filesystem::path& path);
std::wstring format_bytes_wide(uint64_t bytes);

}  // namespace gt
