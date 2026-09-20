#include "imu/gt_hid.h"
#include "imu/gt_protocol.h"
#include "imu/orientation_calibration.h"

#include <hidapi.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

using SteadyClock = std::chrono::steady_clock;

struct Options {
    std::string output_path;
    std::string log_path;
};

const char* phase_name(gt::CalibrationPhase phase) {
    switch (phase) {
        case gt::CalibrationPhase::Still:
            return "still";
        case gt::CalibrationPhase::Yaw:
            return "yaw";
        case gt::CalibrationPhase::Nod:
            return "nod";
        case gt::CalibrationPhase::Tilt:
            return "tilt";
    }
    return "unknown";
}

void print_usage() {
    std::printf(
        "orientation_calibrate - measure RayNeo GT sensor-to-head alignment\n"
        "  --output FILE  calibration output (default config/orientation.json)\n"
        "  --log FILE     optional raw calibration CSV for diagnosis\n");
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            options.output_path = argv[++i];
        } else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            options.log_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return false;
        } else {
            std::printf("unknown or incomplete option: %s\n", argv[i]);
            print_usage();
            return false;
        }
    }
    return true;
}

std::string default_output_path() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return "config/orientation.json";
    }
    executable.resize(length);
    return (std::filesystem::path(executable).parent_path().parent_path() / "config" /
            "orientation.json")
        .string();
}

uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now().time_since_epoch())
            .count());
}

bool send_command(gt::GtHidDevice& device, uint8_t command) {
    return device.send_command_verified(command, 500, nullptr);
}

void drain(gt::GtHidDevice& device, int milliseconds) {
    const auto end = SteadyClock::now() + std::chrono::milliseconds(milliseconds);
    uint8_t buffer[64];
    while (SteadyClock::now() < end && device.read_report(buffer, sizeof(buffer), 10) > 0) {
    }
}

class StreamGuard {
public:
    explicit StreamGuard(gt::GtHidDevice& device) : device_(device) {}
    ~StreamGuard() {
        if (started_) {
            send_command(device_, gt::kCmdStreamOff);
        }
    }
    void mark_started() { started_ = true; }

private:
    gt::GtHidDevice& device_;
    bool started_ = false;
};

bool wait_for_enter(const char* prompt) {
    std::printf("\n%s\nPress ENTER when ready, or type Q then ENTER to cancel: ", prompt);
    std::fflush(stdout);
    std::string response;
    if (!std::getline(std::cin, response)) {
        return false;
    }
    return response != "q" && response != "Q";
}

void ready_countdown() {
    std::printf("Ready");
    std::fflush(stdout);
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::printf(".");
        std::fflush(stdout);
    }
    std::printf(" GO\n");
}

bool capture_phase(gt::GtHidDevice& device, gt::CalibrationPhase phase, double seconds,
                   gt::CalibrationPhaseData& output, std::ofstream& csv, std::string& error) {
    output.phase = phase;
    output.samples.clear();
    const auto end = SteadyClock::now() + std::chrono::duration<double>(seconds);
    while (SteadyClock::now() < end) {
        uint8_t buffer[64]{};
        const int bytes = device.read_report(buffer, sizeof(buffer), 100);
        if (bytes < 0) {
            error = "the glasses disconnected while recording";
            return false;
        }
        if (bytes == 0) {
            continue;
        }
        gt::Report report = gt::decode_report(buffer, static_cast<size_t>(bytes));
        if (report.kind != gt::ReportKind::Imu) {
            continue;
        }
        report.imu.host_time_us = now_us();
        output.samples.push_back(report.imu);
        if (csv) {
            csv << phase_name(phase) << ',' << report.imu.host_time_us << ',' << report.imu.tick_100us
                << ',' << report.imu.accel_mps2.x << ',' << report.imu.accel_mps2.y << ','
                << report.imu.accel_mps2.z << ',' << report.imu.gyro_degs.x << ','
                << report.imu.gyro_degs.y << ',' << report.imu.gyro_degs.z << ','
                << report.imu.mag_ut.x << ',' << report.imu.mag_ut.y << ',' << report.imu.mag_ut.z
                << ',' << report.imu.temp_c << '\n';
        }
    }
    if (output.samples.size() < 100) {
        error = "too few IMU samples were received; reconnect the glasses and retry";
        return false;
    }
    std::printf("captured %zu samples (%.1f Hz)\n", output.samples.size(),
                static_cast<double>(output.samples.size()) / seconds);
    return true;
}

void print_axis(const char* name, const gt::CalibrationAxisDiagnostics& axis) {
    std::printf("[cal] phase=%s active=%.2fs excursion=%.1fdeg dominance=%.1f perp_rms=%.3fdeg/s\n",
                name, axis.active_seconds, axis.excursion_deg, axis.dominance,
                axis.perpendicular_rms_degs);
    std::printf("[cal] phase=%s axis_B=(%+.5f %+.5f %+.5f)\n", name, axis.axis_body.x,
                axis.axis_body.y, axis.axis_body.z);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        return 2;
    }
    if (options.output_path.empty()) {
        options.output_path = default_output_path();
    }

    if (hid_init() != 0) {
        std::printf("hidapi initialization failed\n");
        return 1;
    }
    struct HidExitGuard {
        ~HidExitGuard() { hid_exit(); }
    } hid_exit_guard;

    const auto devices = gt::GtHidDevice::enumerate();
    if (devices.empty()) {
        std::printf("no RayNeo GT HID device found; connect the glasses and retry\n");
        return 1;
    }
    for (const auto& device : devices) {
        std::printf("found: %s\n", gt::GtHidDevice::describe(device).c_str());
    }

    gt::GtHidDevice device;
    if (!device.open_first()) {
        std::printf("failed to open the RayNeo GT HID interface\n");
        return 1;
    }
    StreamGuard stream_guard(device);
    if (!send_command(device, gt::kCmdStreamOff)) {
        std::printf("warning: the stream-off command was not acknowledged\n");
    }
    drain(device, 100);
    std::string stream_error;
    if (!device.send_command_verified(gt::kCmdStreamOn, 1000, &stream_error)) {
        std::printf("failed to start the IMU stream: %s\n", stream_error.c_str());
        std::printf("check that no other app is using the glasses, then retry\n");
        return 1;
    }
    stream_guard.mark_started();
    drain(device, 100);

    std::ofstream csv;
    if (!options.log_path.empty()) {
        csv.open(options.log_path, std::ios::out | std::ios::trunc);
        if (!csv) {
            std::printf("could not open raw log: %s\n", options.log_path.c_str());
            return 1;
        }
        csv << "phase,host_us,tick_100us,ax,ay,az,gx,gy,gz,mx,my,mz,temp_c\n";
    }

    std::printf("\nWear the glasses normally and sit upright. Keep your torso facing forward.\n"
                "Each motion goes away from centre first, then returns to centre.\n");

    gt::CalibrationPhaseData still;
    gt::CalibrationPhaseData yaw;
    gt::CalibrationPhaseData nod;
    gt::CalibrationPhaseData tilt;
    std::string error;

    if (!wait_for_enter("Look straight ahead and hold completely still for 3 seconds.")) {
        return 2;
    }
    std::printf("Recording stillness now...\n");
    if (!capture_phase(device, gt::CalibrationPhase::Still, 3.0, still, csv, error)) {
        std::printf("calibration failed: %s\n", error.c_str());
        return 1;
    }

    if (!wait_for_enter("Slowly turn your head LEFT about 30 degrees, then return to centre. "
                        "Do not tilt or nod. Slow and smooth is fine.")) {
        return 2;
    }
    ready_countdown();
    if (!capture_phase(device, gt::CalibrationPhase::Yaw, 6.0, yaw, csv, error)) {
        std::printf("calibration failed: %s\n", error.c_str());
        return 1;
    }

    if (!wait_for_enter("Slowly nod DOWN toward your chest, then return to centre. Do not tilt. "
                        "A small nod is enough.")) {
        return 2;
    }
    ready_countdown();
    if (!capture_phase(device, gt::CalibrationPhase::Nod, 6.0, nod, csv, error)) {
        std::printf("calibration failed: %s\n", error.c_str());
        return 1;
    }

    if (!wait_for_enter("Slowly tilt your head toward your RIGHT shoulder as far as is "
                        "comfortable, then return to centre. About 15-20 degrees is plenty.")) {
        return 2;
    }
    ready_countdown();
    if (!capture_phase(device, gt::CalibrationPhase::Tilt, 6.0, tilt, csv, error)) {
        std::printf("calibration failed: %s\n", error.c_str());
        return 1;
    }

    const gt::OrientationCalibrationResult result =
        gt::calibrate_orientation(still, yaw, nod, tilt);
    std::printf("[cal] begin v1 mag=off\n");
    std::printf("[cal] phase=still n=%zu gyro_rms=%.3fdeg/s\n", still.samples.size(),
                result.still_gyro_rms_degs);
    print_axis("yaw", result.yaw);
    print_axis("nod", result.nod);
    print_axis("tilt", result.tilt);
    if (!result.ok) {
        std::printf("[cal] FAIL %s: %s\n", result.code.c_str(), result.message.c_str());
        std::printf("No calibration file was changed. Repeat the tool and follow the named step carefully.\n");
        return 1;
    }

    std::printf("[cal] head_basis_B right=(%+.5f %+.5f %+.5f) forward=(%+.5f %+.5f %+.5f) "
                "up=(%+.5f %+.5f %+.5f)\n",
                result.right_body.x, result.right_body.y, result.right_body.z, result.forward_body.x,
                result.forward_body.y, result.forward_body.z, result.up_body.x, result.up_body.y,
                result.up_body.z);
    std::printf("[cal] sensor_to_head rows (right, forward, up):\n");
    for (int row = 0; row < 3; ++row) {
        std::printf("[cal]   {%+.7f, %+.7f, %+.7f}\n", result.sensor_to_head[row * 3],
                    result.sensor_to_head[row * 3 + 1], result.sensor_to_head[row * 3 + 2]);
    }

    const std::filesystem::path output_path(options.output_path);
    if (!output_path.parent_path().empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(output_path.parent_path(), directory_error);
        if (directory_error) {
            std::printf("could not create calibration directory: %s\n",
                        directory_error.message().c_str());
            return 1;
        }
    }
    if (!gt::save_orientation_calibration(options.output_path, result, error)) {
        std::printf("calibration succeeded but could not be saved: %s\n", error.c_str());
        return 1;
    }
    std::printf("[cal] result ok\nSaved %s\n", options.output_path.c_str());
    return 0;
}
