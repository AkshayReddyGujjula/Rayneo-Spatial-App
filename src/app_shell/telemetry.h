#pragma once

// Latest-row reader for the engine's diagnostics CSV (spatial_desk.exe --log).
//
// The controller only ever shows the newest sample, so this parses the tail of
// the file: the header (when present) is mapped by column name, which keeps the
// reader working if the engine appends columns later. No CSV library and no
// allocations beyond the tail read.

#include <cstdint>
#include <string>

namespace gt {

struct TelemetrySample {
    bool valid = false;
    double elapsed_s = 0.0;
    uint32_t tick_100us = 0;
    float gyro_raw[3] = {0.0f, 0.0f, 0.0f};
    float bias[3] = {0.0f, 0.0f, 0.0f};
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
    bool still = false;
    bool rest = false;
    int adapt_state = 0;
    float corrected_rate_degs = 0.0f;
    float stillness_degs = 0.0f;
    float accel_dev_mps2 = 0.0f;
    uint32_t escape_rollbacks = 0;
};

// Canonical header written by spatial_desk.exe when it creates a new CSV.
const char* telemetry_csv_header();

// Parses one CSV line using the canonical column order.
bool parse_telemetry_row(const std::string& line, TelemetrySample& sample, std::string& error);

// Parses the newest usable row out of a CSV tail (last rows only, any length).
bool parse_telemetry_tail(const std::string& csv_text, TelemetrySample& sample, std::string& error);

const wchar_t* adapt_state_label(int adapt_state);

}  // namespace gt
