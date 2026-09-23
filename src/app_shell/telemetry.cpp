#include "app_shell/telemetry.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace gt {
namespace {

struct ColumnMap {
    int elapsed = -1;
    int tick = -1;
    int gyro[3] = {-1, -1, -1};
    int bias[3] = {-1, -1, -1};
    int yaw = -1;
    int pitch = -1;
    int roll = -1;
    int still = -1;
    int rest = -1;
    int adapt = -1;
    int corrected = -1;
    int stillness = -1;
    int accel = -1;
    int rollbacks = -1;
};

std::vector<std::string> split_fields(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    for (const char character : line) {
        if (character == ',') {
            fields.push_back(current);
            current.clear();
        } else if (character != '\r') {
            current.push_back(character);
        }
    }
    fields.push_back(current);
    return fields;
}

bool parse_double(const std::string& text, double& value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || end == nullptr) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        ++end;
    }
    if (*end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_uint(const std::string& text, uint32_t& value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || end == nullptr) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        ++end;
    }
    if (*end != '\0') {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

ColumnMap canonical_columns() {
    ColumnMap map;
    map.elapsed = 0;
    map.tick = 1;
    map.gyro[0] = 2;
    map.gyro[1] = 3;
    map.gyro[2] = 4;
    map.bias[0] = 5;
    map.bias[1] = 6;
    map.bias[2] = 7;
    map.yaw = 8;
    map.pitch = 9;
    map.roll = 10;
    map.still = 11;
    map.rest = 12;
    map.adapt = 13;
    map.corrected = 14;
    map.stillness = 15;
    map.accel = 16;
    map.rollbacks = 17;
    return map;
}

bool header_looks_like_header(const std::string& line) {
    // A data row never contains a column name, so a substring test is enough and
    // it keeps working when the engine reorders or extends its columns.
    return line.find("elapsed_s") != std::string::npos;
}

ColumnMap columns_from_header(const std::string& header) {
    ColumnMap map;
    const std::vector<std::string> names = split_fields(header);
    for (size_t index = 0; index < names.size(); ++index) {
        const std::string& name = names[index];
        const int column = static_cast<int>(index);
        if (name == "elapsed_s") {
            map.elapsed = column;
        } else if (name == "tick_100us") {
            map.tick = column;
        } else if (name == "gx_raw") {
            map.gyro[0] = column;
        } else if (name == "gy_raw") {
            map.gyro[1] = column;
        } else if (name == "gz_raw") {
            map.gyro[2] = column;
        } else if (name == "bias_x") {
            map.bias[0] = column;
        } else if (name == "bias_y") {
            map.bias[1] = column;
        } else if (name == "bias_z") {
            map.bias[2] = column;
        } else if (name == "view_yaw_deg") {
            map.yaw = column;
        } else if (name == "view_pitch_deg") {
            map.pitch = column;
        } else if (name == "view_roll_deg") {
            map.roll = column;
        } else if (name == "still") {
            map.still = column;
        } else if (name == "rest") {
            map.rest = column;
        } else if (name == "adapt_state") {
            map.adapt = column;
        } else if (name == "corrected_rate_degs") {
            map.corrected = column;
        } else if (name == "stillness_degs") {
            map.stillness = column;
        } else if (name == "accel_dev_mps2") {
            map.accel = column;
        } else if (name == "escape_rollbacks") {
            map.rollbacks = column;
        }
    }
    return map;
}

float float_at(const std::vector<std::string>& fields, int column) {
    if (column < 0 || static_cast<size_t>(column) >= fields.size()) {
        return 0.0f;
    }
    double value = 0.0;
    if (!parse_double(fields[static_cast<size_t>(column)], value)) {
        return 0.0f;
    }
    return static_cast<float>(value);
}

bool parse_with_columns(const std::string& line, const ColumnMap& map, TelemetrySample& sample,
                        std::string& error) {
    const std::vector<std::string> fields = split_fields(line);
    if (fields.size() < 2) {
        error = "row has fewer than two columns";
        return false;
    }
    if (map.elapsed < 0 || static_cast<size_t>(map.elapsed) >= fields.size()) {
        error = "row is missing the elapsed_s column";
        return false;
    }
    int final_declared_column = map.elapsed;
    final_declared_column = std::max(final_declared_column, map.tick);
    for (int axis = 0; axis < 3; ++axis) {
        final_declared_column = std::max(final_declared_column, map.gyro[axis]);
        final_declared_column = std::max(final_declared_column, map.bias[axis]);
    }
    final_declared_column = std::max(final_declared_column, map.yaw);
    final_declared_column = std::max(final_declared_column, map.pitch);
    final_declared_column = std::max(final_declared_column, map.roll);
    final_declared_column = std::max(final_declared_column, map.still);
    final_declared_column = std::max(final_declared_column, map.rest);
    final_declared_column = std::max(final_declared_column, map.adapt);
    final_declared_column = std::max(final_declared_column, map.corrected);
    final_declared_column = std::max(final_declared_column, map.stillness);
    final_declared_column = std::max(final_declared_column, map.accel);
    final_declared_column = std::max(final_declared_column, map.rollbacks);
    if (final_declared_column < 0 ||
        static_cast<size_t>(final_declared_column) >= fields.size()) {
        error = "row is truncated before its last declared column";
        return false;
    }
    TelemetrySample parsed;
    if (!parse_double(fields[static_cast<size_t>(map.elapsed)], parsed.elapsed_s)) {
        error = "elapsed_s is not a number";
        return false;
    }
    if (map.tick >= 0 && static_cast<size_t>(map.tick) < fields.size()) {
        uint32_t tick = 0;
        if (parse_uint(fields[static_cast<size_t>(map.tick)], tick)) {
            parsed.tick_100us = tick;
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        parsed.gyro_raw[axis] = float_at(fields, map.gyro[axis]);
        parsed.bias[axis] = float_at(fields, map.bias[axis]);
    }
    parsed.yaw_deg = float_at(fields, map.yaw);
    parsed.pitch_deg = float_at(fields, map.pitch);
    parsed.roll_deg = float_at(fields, map.roll);
    parsed.still = float_at(fields, map.still) != 0.0f;
    parsed.rest = float_at(fields, map.rest) != 0.0f;
    parsed.adapt_state = static_cast<int>(std::lround(float_at(fields, map.adapt)));
    parsed.corrected_rate_degs = float_at(fields, map.corrected);
    parsed.stillness_degs = float_at(fields, map.stillness);
    parsed.accel_dev_mps2 = float_at(fields, map.accel);
    const float rollbacks = float_at(fields, map.rollbacks);
    parsed.escape_rollbacks = rollbacks > 0.0f ? static_cast<uint32_t>(rollbacks) : 0u;
    parsed.valid = true;
    sample = parsed;
    return true;
}

}  // namespace

const char* telemetry_csv_header() {
    return "elapsed_s,tick_100us,gx_raw,gy_raw,gz_raw,bias_x,bias_y,bias_z,"
           "view_yaw_deg,view_pitch_deg,view_roll_deg,still,"
           "rest,adapt_state,corrected_rate_degs,stillness_degs,"
           "accel_dev_mps2,escape_rollbacks";
}

bool parse_telemetry_row(const std::string& line, TelemetrySample& sample, std::string& error) {
    if (line.empty()) {
        error = "row is empty";
        return false;
    }
    if (header_looks_like_header(line)) {
        error = "row is the CSV header";
        return false;
    }
    return parse_with_columns(line, canonical_columns(), sample, error);
}

bool parse_telemetry_tail(const std::string& csv_text, TelemetrySample& sample, std::string& error) {
    if (csv_text.empty()) {
        error = "telemetry file is empty";
        return false;
    }
    ColumnMap map = canonical_columns();
    bool have_header = false;
    std::vector<std::string> lines;
    std::string current;
    for (const char character : csv_text) {
        if (character == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    for (const std::string& line : lines) {
        if (header_looks_like_header(line)) {
            map = columns_from_header(line);
            have_header = true;
            break;
        }
    }
    if (!have_header) {
        map = canonical_columns();
    }
    std::string last_error = "telemetry file has no data rows yet";
    for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
        if (line->empty() || header_looks_like_header(*line)) {
            continue;
        }
        std::string row_error;
        if (parse_with_columns(*line, map, sample, row_error)) {
            return true;
        }
        last_error = row_error;
    }
    error = last_error;
    return false;
}

const wchar_t* adapt_state_label(int adapt_state) {
    switch (adapt_state) {
        case 0:
            return L"idle";
        case 1:
            return L"routine";
        case 2:
            return L"escape";
        case 3:
            return L"calibrating";
        default:
            return L"unknown";
    }
}

}  // namespace gt
