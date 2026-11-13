#include "imu/calibration_live.h"
#include "imu/gt_hid.h"
#include "imu/gt_protocol.h"
#include "imu/orientation_calibration.h"
#include "util/utf8_path.h"

#include <hidapi.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using SteadyClock = std::chrono::steady_clock;

// Set by Ctrl+C / Ctrl+Break / console close so the recording loops stop and
// the stream-off guard still runs.
std::atomic<bool> g_cancel{false};
// Set by an empty line on a piped stdin in --auto mode: skip the countdown.
std::atomic<bool> g_start_now{false};

// --auto with a piped stdin (the in-app wizard): a detached watcher reads
// lines with ReadFile (no CRT stream lock held at exit) and turns "q" or the
// pipe closing into the normal cancel path, so the stream-off guard runs
// before the process exits. A console stdin is left alone: Ctrl+C works there.
void start_stdin_watcher() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE || GetFileType(input) != FILE_TYPE_PIPE) {
        return;
    }
    std::thread([input]() {
        std::string pending;
        char buffer[256];
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(input, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
                g_cancel.store(true);  // the front end closed stdin or went away
                return;
            }
            pending.append(buffer, read);
            size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                const gt::CalibrationStdinCommand command =
                    gt::parse_calibration_stdin_line(pending.substr(0, newline));
                pending.erase(0, newline + 1);
                if (command == gt::CalibrationStdinCommand::Cancel) {
                    g_cancel.store(true);
                } else if (command == gt::CalibrationStdinCommand::StartNow) {
                    g_start_now.store(true);
                }
            }
            if (pending.size() > 4096) {
                pending.clear();
            }
        }
    }).detach();
}

// Manual-reset event set when wmain has unwound past the stream-off guard
// (on every exit path), so the console-close handler can wait for it.
HANDLE g_cleanup_done = nullptr;

BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_cancel.store(true);
        return TRUE;
    }
    if (type == CTRL_CLOSE_EVENT) {
        // Windows ends the process as soon as this handler returns (and after
        // about 5 s regardless), so only setting g_cancel would skip the
        // stream-off. Sending it from this thread is not safe: the main thread
        // may be inside hid_read on the same handle, and the verified command
        // reads its own ack. Instead cancel, then hold the close here while
        // the main thread runs its normal cancel path, StreamGuard included.
        g_cancel.store(true);
        if (g_cleanup_done != nullptr) {
            WaitForSingleObject(g_cleanup_done, 4000);
        }
        return TRUE;
    }
    return FALSE;
}

// Declared first in wmain, destroyed last: after the stream-off guard, the
// device close and hid_exit.
struct CleanupDoneSignal {
    ~CleanupDoneSignal() {
        if (g_cleanup_done != nullptr) {
            SetEvent(g_cleanup_done);
        }
    }
};

struct Options {
    std::string output_path;
    std::string log_path;
    bool mag_only = false;
    // Front-end mode: machine-readable progress lines on stdout.
    bool progress_json = false;
    // No keyboard: each step starts by itself after auto_delay_s.
    bool auto_advance = false;
    double auto_delay_s = 4.0;
    // Recorded with the earbuds (or other head-worn magnets) in place.
    bool earbuds = false;
    // Gestures per motion step (1 = the original single gesture).
    int repeats = 1;
};

// Display-only live guide (--progress-json): "@@live {json}" lines at about
// 22 Hz while a step waits or records. Never feeds the calibration maths.
class LiveReporter {
public:
    LiveReporter(bool enabled, int repeats) : enabled_(enabled), repeats_(repeats) {}
    bool enabled() const { return enabled_; }
    void set_forward_hint(const gt::Vec3& forward_body, const char* source) {
        forward_hint_ = forward_body;
        forward_source_ = source;
        if (frame_.valid) {
            frame_ = gt::make_live_frame(frame_.up_body, forward_hint_, forward_source_);
        }
    }
    // After the still step: its gyro bias and measured gravity.
    void set_still(const gt::Vec3& bias_body, const gt::CalibrationPhaseData& still) {
        bias_ = bias_body;
        gt::Vec3 sum;
        for (const gt::ImuSample& sample : still.samples) {
            const gt::Vec3 a = gt::live_package_to_body(sample.accel_mps2);
            sum = gt::Vec3{sum.x + a.x, sum.y + a.y, sum.z + a.z};
        }
        frame_ = gt::make_live_frame(sum, forward_hint_, forward_source_);
    }
    // After the nod step: the measured head-right axis refines forward
    // (forward = up x right).
    void set_measured_right(const gt::Vec3& right_body) {
        if (!frame_.valid) {
            return;
        }
        const gt::Vec3& u = frame_.up_body;
        const gt::Vec3 forward{u.y * right_body.z - u.z * right_body.y, u.z * right_body.x - u.x * right_body.z,
                               u.x * right_body.y - u.y * right_body.x};
        set_forward_hint(forward, "measured");
    }
    void begin(gt::LiveStepKind kind, int step, const char* state, double seconds) {
        if (!enabled_) {
            return;
        }
        tracker_.begin(kind, step, state, repeats_, bias_, frame_);
        start_ = SteadyClock::now();
        seconds_ = seconds;
        last_emit_ = start_ - std::chrono::seconds(1);
        active_ = true;
    }
    void end() { active_ = false; }
    void add(const gt::ImuSample& sample) {
        if (!enabled_ || !active_) {
            return;
        }
        tracker_.add(sample);
        emit_if_due();
    }
    void emit_if_due() {
        if (!enabled_ || !active_) {
            return;
        }
        const auto now = SteadyClock::now();
        if (now - last_emit_ < std::chrono::milliseconds(45)) {
            return;
        }
        last_emit_ = now;
        const double elapsed = std::chrono::duration<double>(now - start_).count();
        const double remaining = seconds_ < 0.0 ? -1.0 : (std::max)(0.0, seconds_ - elapsed);
        const std::string line = gt::format_calibration_live(
            tracker_.snapshot(static_cast<float>(elapsed), static_cast<float>(remaining)));
        std::puts(line.c_str());
        std::fflush(stdout);
    }

private:
    bool enabled_ = false;
    bool active_ = false;
    int repeats_ = 1;
    gt::Vec3 bias_;
    gt::Vec3 forward_hint_ = gt::live_nominal_forward_body();
    const char* forward_source_ = "nominal";
    gt::CalibrationLiveFrame frame_;
    gt::CalibrationLiveTracker tracker_;
    SteadyClock::time_point start_{};
    SteadyClock::time_point last_emit_{};
    double seconds_ = -1.0;
};

gt::LiveStepKind live_kind(gt::CalibrationPhase phase) {
    switch (phase) {
        case gt::CalibrationPhase::Yaw:
            return gt::LiveStepKind::Turn;
        case gt::CalibrationPhase::Nod:
            return gt::LiveStepKind::Nod;
        case gt::CalibrationPhase::Tilt:
            return gt::LiveStepKind::Tilt;
        case gt::CalibrationPhase::Still:
            break;
    }
    return gt::LiveStepKind::Still;
}

// Front-end reporting for one calibration flow.
class Progress {
public:
    Progress(const Options& options, const char* flow, int steps, LiveReporter* live = nullptr)
        : options_(options), flow_(flow), steps_(steps), live_(live) {}

    void emit(int step, const char* state, const std::string& prompt, const std::string& detail = {},
              float quality = -1.0f) const {
        if (!options_.progress_json) {
            return;
        }
        gt::CalibrationProgress p;
        p.flow = flow_;
        p.step = step < 1 ? 1 : (step > steps_ ? steps_ : step);
        p.steps = steps_;
        p.state = state;
        p.prompt = prompt;
        p.detail = detail;
        p.quality = quality;
        const std::string line = gt::format_calibration_progress(p);
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    }

    int steps() const { return steps_; }
    LiveReporter* live() const { return live_ != nullptr && live_->enabled() ? live_ : nullptr; }

private:
    const Options& options_;
    const char* flow_;
    int steps_;
    LiveReporter* live_;
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
        "  --output FILE      calibration output (default config/orientation.json)\n"
        "  --log FILE         optional raw calibration CSV for diagnosis\n"
        "  --mag              only calibrate the magnetometer (needs an existing orientation\n"
        "                     calibration); enables the drift-free heading lock\n"
        "  --earbuds          (with --mag) record that in-ear earbuds were worn; calibrate\n"
        "                     the way you normally work so their magnets are absorbed\n"
        "  --repeats N        do each turn / nod / tilt N times in one recording (1-5,\n"
        "                     default 1); the axes are averaged with outlier rejection\n"
        "                     and their consistency is scored (3 recommended)\n"
        "  --progress-json    also print one '@@progress {json}' line per state change\n"
        "  --auto             no keyboard: each step starts after --auto-delay seconds\n"
        "  --auto-delay S     seconds to read each instruction in --auto mode (default 4);\n"
        "                     with a piped stdin, an empty line starts the step now and\n"
        "                     'q' (or closing stdin) cancels like Ctrl+C\n"
        "Exit code 0 = calibration saved, 1 = failed or cancelled, 2 = bad arguments.\n");
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            options.output_path = argv[++i];
        } else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            options.log_path = argv[++i];
        } else if (std::strcmp(argv[i], "--mag") == 0) {
            options.mag_only = true;
        } else if (std::strcmp(argv[i], "--earbuds") == 0) {
            options.earbuds = true;
        } else if (std::strcmp(argv[i], "--repeats") == 0 && i + 1 < argc) {
            options.repeats = std::atoi(argv[++i]);
            if (options.repeats < 1 || options.repeats > 5) {
                std::printf("--repeats must be between 1 and 5\n");
                return false;
            }
        } else if (std::strcmp(argv[i], "--progress-json") == 0) {
            options.progress_json = true;
        } else if (std::strcmp(argv[i], "--auto") == 0) {
            options.auto_advance = true;
        } else if (std::strcmp(argv[i], "--auto-delay") == 0 && i + 1 < argc) {
            options.auto_delay_s = std::atof(argv[++i]);
            if (!(options.auto_delay_s >= 0.0 && options.auto_delay_s <= 60.0)) {
                std::printf("--auto-delay must be between 0 and 60 seconds\n");
                return false;
            }
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

// Locate the repository root by walking up from the executable until a config
// directory is found, so nested build directories keep working. Falls back to
// the historical "two levels below the root" assumption if no marker is found.
std::string default_output_path() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return "config/orientation.json";
    }
    executable.resize(length);
    const std::filesystem::path executable_directory =
        std::filesystem::path(executable).parent_path();
    for (std::filesystem::path candidate = executable_directory;; candidate = candidate.parent_path()) {
        std::error_code exists_error;
        if (std::filesystem::exists(candidate / "config" / "layouts" / "default.json",
                                    exists_error)) {
            return gt::utf8_from_path(candidate / "config" / "orientation.json");
        }
        exists_error.clear();
        if (std::filesystem::exists(candidate / "config", exists_error)) {
            return gt::utf8_from_path(candidate / "config" / "orientation.json");
        }
        if (candidate == candidate.parent_path()) {
            break;
        }
    }
    return gt::utf8_from_path(executable_directory.parent_path() / "config" / "orientation.json");
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

// Shows one instruction and waits until the wearer is ready: ENTER on the
// console, or (--auto) a fixed reading delay. Emits "waiting" either way.
// Returns false when the wearer cancelled.
bool wait_ready(const Options& options, const Progress& progress, int step, const std::string& prompt,
                gt::GtHidDevice* device = nullptr, gt::CalibrationPhase phase = gt::CalibrationPhase::Still) {
    progress.emit(step, "waiting", prompt);
    LiveReporter* const live = device != nullptr ? progress.live() : nullptr;
    if (options.auto_advance && live != nullptr) {
        // The same countdown, but the samples are read so the guide can show
        // the head live (display only; they are never recorded).
        std::printf("\n%s\nStarting in %.0f s...\n", prompt.c_str(), options.auto_delay_s);
        std::fflush(stdout);
        g_start_now.store(false);
        live->begin(live_kind(phase), step, "waiting", options.auto_delay_s);
        const auto end = SteadyClock::now() + std::chrono::duration<double>(options.auto_delay_s);
        while (SteadyClock::now() < end && !g_start_now.load()) {
            if (g_cancel.load()) {
                live->end();
                return false;
            }
            uint8_t buffer[64]{};
            const int bytes = device->read_report(buffer, sizeof(buffer), 20);
            if (bytes < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (bytes > 0) {
                const gt::Report report = gt::decode_report(buffer, static_cast<size_t>(bytes));
                if (report.kind == gt::ReportKind::Imu) {
                    live->add(report.imu);
                }
            }
            live->emit_if_due();
        }
        live->end();
        return !g_cancel.load();
    }
    if (options.auto_advance) {
        std::printf("\n%s\nStarting in %.0f s...\n", prompt.c_str(), options.auto_delay_s);
        std::fflush(stdout);
        g_start_now.store(false);
        const auto end = SteadyClock::now() + std::chrono::duration<double>(options.auto_delay_s);
        while (SteadyClock::now() < end && !g_start_now.load()) {
            if (g_cancel.load()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return !g_cancel.load();
    }
    std::printf("\n%s\nPress ENTER when ready, or type Q then ENTER to cancel: ", prompt.c_str());
    std::fflush(stdout);
    std::string response;
    if (!std::getline(std::cin, response) || g_cancel.load()) {
        return false;
    }
    if (response == "q" || response == "Q") {
        return false;
    }
    // Interactive mode keeps the short "Ready... GO" so the wearer can settle
    // after reaching for the keyboard.
    std::printf("Ready");
    std::fflush(stdout);
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::printf(".");
        std::fflush(stdout);
    }
    std::printf(" GO\n");
    return !g_cancel.load();
}

bool capture_phase(gt::GtHidDevice& device, gt::CalibrationPhase phase, double seconds,
                   gt::CalibrationPhaseData& output, std::ofstream& csv, std::string& error,
                   LiveReporter* live = nullptr, int step = 1) {
    output.phase = phase;
    output.samples.clear();
    if (live != nullptr) {
        live->begin(live_kind(phase), step, "recording", seconds);
    }
    struct LiveEnd {
        LiveReporter* live;
        ~LiveEnd() {
            if (live != nullptr) {
                live->end();
            }
        }
    } live_end{live};
    const auto end = SteadyClock::now() + std::chrono::duration<double>(seconds);
    while (SteadyClock::now() < end) {
        if (g_cancel.load()) {
            error = "cancelled";
            return false;
        }
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
        if (live != nullptr) {
            live->add(report.imu);
        }
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

constexpr const char* kStillPrompt = "Look straight ahead and hold completely still for 4 seconds.";

bool capture_still(gt::GtHidDevice& device, const Options& options, const Progress& progress, int step,
                   gt::CalibrationPhaseData& still, gt::Vec3& bias_body, std::ofstream& csv,
                   std::string& error) {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (!wait_ready(options, progress, step, kStillPrompt, &device, gt::CalibrationPhase::Still)) {
            error = "cancelled";
            return false;
        }
        progress.emit(step, "recording", kStillPrompt);
        std::printf("Recording stillness now...\n");
        if (!capture_phase(device, gt::CalibrationPhase::Still, 4.0, still, csv, error, progress.live(), step)) {
            return false;
        }
        float rms = 0.0f;
        bias_body = gt::estimate_still_bias(still, rms);
        std::printf("[cal] phase=still n=%zu gyro_rms=%.3fdeg/s\n", still.samples.size(), rms);
        if (rms <= 1.5f) {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "head movement %.2f deg/s (limit 1.5)", rms);
            if (progress.live() != nullptr) {
                progress.live()->set_still(bias_body, still);
            }
            progress.emit(step, "ok", kStillPrompt, detail, gt::still_quality(rms));
            return true;
        }
        std::printf("stillness check failed (%.2f deg/s of movement, limit 1.5).\n"
                    "Rest the back of your head against something and try again.\n",
                    rms);
        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "HOLD_STILL: %.2f deg/s of movement (limit 1.5) - rest the back of your head "
                      "against something",
                      rms < 1e6f ? rms : 99.0f);
        if (attempt < 3) {
            progress.emit(step, "retry", kStillPrompt, detail, 0.0f);
        } else {
            error = std::string("stillness could not be measured after three attempts (") + detail + ")";
        }
    }
    return false;
}

bool capture_motion(gt::GtHidDevice& device, const Options& options, const Progress& progress, int step,
                    gt::CalibrationPhase phase, float command_sign, const std::string& prompt, double seconds,
                    const gt::Vec3& bias_body, gt::CalibrationPhaseData& data, std::ofstream& csv,
                    std::string& error, gt::Vec3* measured_axis = nullptr) {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (!wait_ready(options, progress, step, prompt, &device, phase)) {
            error = "cancelled";
            return false;
        }
        progress.emit(step, "recording", prompt);
        if (!capture_phase(device, phase, seconds, data, csv, error, progress.live(), step)) {
            return false;
        }
        gt::CalibrationAxisDiagnostics diagnostics;
        std::string code;
        std::string message;
        if (options.repeats > 1) {
            gt::AxisRepeatStats stats;
            if (gt::analyze_motion_repeats(data, bias_body, command_sign, diagnostics, stats, code, message)) {
                const float consistency = gt::repeat_consistency_quality(stats);
                const float quality =
                    (std::min)(gt::axis_quality(diagnostics), consistency >= 0.0f ? consistency : 0.6f);
                std::printf("[cal] phase=%s ok: gestures detected=%d usable=%d used=%d rejected=%d "
                            "spread=%.2fdeg excursion=%.1fdeg dominance=%.1f\n",
                            phase_name(phase), stats.detected, stats.usable, stats.used, stats.rejected,
                            stats.spread_deg, diagnostics.excursion_deg, diagnostics.dominance);
                char detail[192];
                // Of the gestures found, not of those asked for: a wearer who
                // does four when asked for three must not read "4 of 3".
                std::snprintf(detail, sizeof(detail),
                              "%d of %d detected gestures used%s, spread %.1f deg, excursion %.1f deg", stats.used,
                              stats.detected, stats.rejected > 0 ? " (outlier dropped)" : "",
                              stats.spread_deg, diagnostics.excursion_deg);
                if (stats.used < 2) {
                    std::printf("Only one usable gesture was found, so its consistency cannot be checked.\n");
                }
                if (measured_axis != nullptr) {
                    *measured_axis = diagnostics.axis_body;
                }
                progress.emit(step, "ok", prompt, detail, quality);
                return true;
            }
        } else if (gt::analyze_motion_phase(data, bias_body, command_sign, diagnostics, code, message)) {
            std::printf("[cal] phase=%s ok: active=%.2fs excursion=%.1fdeg dominance=%.1f\n",
                        phase_name(phase), diagnostics.active_seconds, diagnostics.excursion_deg,
                        diagnostics.dominance);
            char detail[128];
            std::snprintf(detail, sizeof(detail), "excursion %.1f deg, dominance %.1f",
                          diagnostics.excursion_deg, diagnostics.dominance);
            if (measured_axis != nullptr) {
                *measured_axis = diagnostics.axis_body;
            }
            progress.emit(step, "ok", prompt, detail, gt::axis_quality(diagnostics));
            return true;
        }
        std::printf("[cal] phase=%s FAIL %s: %s\n", phase_name(phase), code.c_str(), message.c_str());
        std::printf("  measured: active=%.2fs excursion=%.1fdeg dominance=%.1f perp_rms=%.2fdeg/s\n",
                    diagnostics.active_seconds, diagnostics.excursion_deg, diagnostics.dominance,
                    diagnostics.perpendicular_rms_degs);
        const std::string detail = code + ": " + message;
        if (attempt < 3) {
            std::printf("Let's repeat just this step.\n");
            progress.emit(step, "retry", prompt, detail, 0.0f);
        } else {
            error = std::string("the ") + phase_name(phase) + " step did not pass after three attempts (" +
                    detail + ")";
        }
    }
    return false;
}

int fail_run(const Progress& progress, int step, const std::string& prompt, const std::string& error) {
    std::printf("calibration failed: %s\n", error.c_str());
    progress.emit(step, "failed", prompt, error, -1.0f);
    return 1;
}

// The recording loops poll g_cancel, but a "q" can land after the last step's
// final poll, while its samples are analysed. Checked once more right before
// anything is written: the controller already shows "Nothing was changed".
bool cancel_requested_before_save() {
    if (!g_cancel.load()) {
        return false;
    }
    std::printf("cancelled before saving; nothing was changed\n");
    return true;
}

// A filesystem error in the system's own words, as UTF-8 for the @@progress
// line. error_code::message() is the localized text in the ANSI code page,
// which is not valid UTF-8 on a non-English Windows.
std::string system_error_utf8(const std::error_code& error) {
    if (error.category() != std::system_category()) {
        return error.message();  // CRT errno text: plain ASCII
    }
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(error.value()), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = length > 0 && buffer != nullptr ? std::wstring(buffer, length) : std::wstring();
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    const std::string utf8 = gt::utf8_from_wide_text(text);
    const std::string code = "Windows error " + std::to_string(error.value());
    return utf8.empty() ? code : utf8 + " (" + code + ")";
}

// Which motion would pin down the weakest hard-iron direction, in head terms.
std::string weak_axis_advice(const gt::Vec3& weak_package, const std::array<float, 9>& sensor_to_head) {
    const gt::Vec3 head = gt::apply_sensor_to_head(sensor_to_head, weak_package);
    const float ax = std::fabs(head.x);
    const float ay = std::fabs(head.y);
    const float az = std::fabs(head.z);
    if (az >= ax && az >= ay) {
        return "nod up and down and tilt ear to shoulder more";
    }
    if (ax >= ay) {
        return "turn further left and right and tilt ear to shoulder more";
    }
    return "turn further left and right and nod up and down more";
}

// Guided magnetometer calibration: one contiguous recording of a full slow turn
// plus nods and tilts. The gyro tracks the attitude, so the fit needs no
// perfect sphere of orientations - but it does need one uninterrupted
// recording (a gap breaks the attitude integration).
int calibrate_magnetometer(gt::GtHidDevice& device, const Options& options, std::ofstream& csv) {
    const Progress progress(options, "mag", 2);
    std::array<float, 9> existing{};
    std::string error;
    if (!gt::load_orientation_calibration(options.output_path, existing, error)) {
        std::printf("magnetometer calibration needs the orientation calibration first (%s: %s)\n",
                    options.output_path.c_str(), error.c_str());
        std::printf("run orientation_calibrate.exe without --mag, then repeat with --mag\n");
        return fail_run(progress, 1, kStillPrompt,
                        "NO_ORIENTATION: run the orientation calibration first (" + error + ")");
    }
    std::printf("\nMagnetometer calibration. Keep phones, speakers and loose magnets away from your head.\n"
                "Wear the glasses exactly as you work: if you wear in-ear earbuds while working, put\n"
                "them in now (their magnets move with your head and are calibrated out).\n");
    gt::CalibrationPhaseData still;
    gt::Vec3 bias_body;
    if (!capture_still(device, options, progress, 1, still, bias_body, csv, error)) {
        return fail_run(progress, 1, kStillPrompt, error);
    }
    const std::string motion_prompt =
        "For 45 seconds, move slowly and smoothly: first turn ALL the way around once (swivel chair, "
        "or stand and turn), about 15 s.";
    const std::string second_prompt =
        "Now nod up and down, tilt ear to shoulder both ways, and look far left, far right, up and "
        "down until the timer ends.";
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (!wait_ready(options, progress, 2,
                        "For 45 seconds, move slowly and smoothly:\n"
                        "  1. Turn ALL the way around once (swivel chair, or stand and turn) - about 15 s.\n"
                        "  2. Then nod up and down, tilt ear to shoulder both ways, and look far left,\n"
                        "     far right, up and down, until the timer ends.")) {
            return fail_run(progress, 2, motion_prompt, "cancelled");
        }
        progress.emit(2, "recording", motion_prompt, "0 of 45 s");
        gt::CalibrationPhaseData motion;
        const auto start = SteadyClock::now();
        std::atomic<bool> recording_done{false};
        std::thread ticker([&]() {
            for (int s = 5; s <= 45 && !recording_done.load(); s += 5) {
                const auto target = start + std::chrono::seconds(s);
                while (SteadyClock::now() < target && !recording_done.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (recording_done.load()) {
                    break;
                }
                std::printf("  %d s%s\n", s, s == 15 ? " - now nod, tilt and look around" : "");
                std::fflush(stdout);
                if (s == 15) {
                    progress.emit(2, "recording", second_prompt, "15 of 45 s");
                }
            }
        });
        const bool captured = capture_phase(device, gt::CalibrationPhase::Yaw, 45.0, motion, csv, error);
        recording_done.store(true);
        ticker.join();
        if (!captured) {
            return fail_run(progress, 2, motion_prompt, error);
        }
        const gt::MagFitResult fit = gt::fit_mag_hard_iron(motion.samples, bias_body);
        const float quality = gt::mag_fit_quality(fit);
        std::printf("[mag] fit %s: hard_iron=(%.2f, %.2f, %.2f) uT |B|=%.2f uT resid=%.3f uT "
                    "rotation=%.0f deg tilt=%.0f deg observability=%.3f quality=%.2f\n",
                    fit.code, fit.hard_iron_ut.x, fit.hard_iron_ut.y, fit.hard_iron_ut.z, fit.field_ut,
                    fit.residual_rms_ut, fit.rotation_coverage_deg, fit.tilt_coverage_deg, fit.observability,
                    quality);
        char measured[192];
        std::snprintf(measured, sizeof(measured),
                      "field %.1f uT, residual %.2f uT, turn %.0f deg, tilt %.0f deg, observability %.3f",
                      fit.field_ut, fit.residual_rms_ut, fit.rotation_coverage_deg, fit.tilt_coverage_deg,
                      fit.observability);
        if (fit.ok) {
            if (cancel_requested_before_save()) {
                return fail_run(progress, 2, motion_prompt, "cancelled");
            }
            gt::MagCalibration mag;
            mag.valid = true;
            mag.hard_iron_ut = fit.hard_iron_ut;
            mag.field_ut = fit.field_ut;
            mag.earbuds = options.earbuds;
            mag.quality = quality;
            if (!gt::save_mag_calibration(options.output_path, mag, error)) {
                std::printf("fit succeeded but could not be saved: %s\n", error.c_str());
                return fail_run(progress, 2, motion_prompt, "SAVE_FAILED: " + error);
            }
            std::string advice;
            if (quality < 0.6f) {
                advice = "; for a better result repeat and " +
                         weak_axis_advice(fit.weak_axis_package, existing);
                std::printf("Accepted, but coverage was thin: next time %s.\n",
                            weak_axis_advice(fit.weak_axis_package, existing).c_str());
            }
            progress.emit(2, "ok", motion_prompt, measured, quality);
            std::printf("[mag] result ok\nSaved %s - the heading lock is on at the next start.\n",
                        options.output_path.c_str());
            progress.emit(2, "done", "Magnetometer calibration saved.",
                          std::string(measured) + advice, quality);
            return 0;
        }
        std::string reason = std::string(fit.code) + ": ";
        if (std::strcmp(fit.code, "SMALL_COVERAGE") == 0) {
            reason += "not enough movement - turn fully around AND nod/tilt";
            std::printf("Not enough movement: turn fully around AND nod/tilt. ");
        } else if (std::strcmp(fit.code, "POOR_FIT") == 0 || std::strcmp(fit.code, "IMPLAUSIBLE_FIELD") == 0) {
            reason += "the field was inconsistent (a magnet, speaker or metal nearby?) - move away from it";
            std::printf("The field was inconsistent (a magnet, speaker or metal nearby?). "
                        "Move away from it. ");
        } else if (std::strcmp(fit.code, "GAP") == 0) {
            reason += "the IMU stream had a gap";
            std::printf("The IMU stream had a gap. ");
        } else {
            reason += "the recording could not be fitted";
        }
        reason += std::string(" (") + measured + ")";
        if (attempt < 3) {
            std::printf("Let's repeat this step.\n");
            progress.emit(2, "retry", motion_prompt, reason, quality);
        } else {
            std::printf("magnetometer calibration failed after three attempts; nothing was changed\n");
            return fail_run(progress, 2, motion_prompt,
                            "failed after three attempts; nothing was changed (" + reason + ")");
        }
    }
    return 1;
}

int calibrate_orientation_flow(gt::GtHidDevice& device, const Options& options, std::ofstream& csv) {
    LiveReporter live(options.progress_json, options.repeats);
    {
        // The guide's forward direction comes from the existing calibration
        // when there is a valid one (display only; this run measures its own).
        std::array<float, 9> previous{};
        std::string previous_error;
        if (gt::load_orientation_calibration(options.output_path, previous, previous_error)) {
            live.set_forward_hint(gt::live_forward_from_sensor_to_head(previous), "file");
        }
    }
    const Progress progress(options, "orientation", 4, &live);
    std::printf("\nWear the glasses normally and sit upright. Keep your torso facing forward.\n"
                "Each motion goes away from centre first, then returns to centre.\n");

    gt::CalibrationPhaseData still;
    gt::CalibrationPhaseData yaw;
    gt::CalibrationPhaseData nod;
    gt::CalibrationPhaseData tilt;
    std::string error;

    static constexpr const char* kYawPrompt =
        "Slowly turn your head LEFT about 30 degrees, then return to centre. "
        "Do not tilt or nod. Slow and smooth is fine.";
    static constexpr const char* kNodPrompt =
        "Slowly nod DOWN toward your chest, then return to centre. Do not tilt. "
        "A small nod is enough.";
    static constexpr const char* kTiltPrompt =
        "Slowly tilt your head toward your RIGHT shoulder as far as is "
        "comfortable, then return to centre. About 15-20 degrees is plenty.";

    // --repeats N: the same gesture N times in one longer recording (about
    // 3.5 s per extra out-and-back); a pause at centre is optional.
    const std::string repeat_suffix =
        options.repeats > 1 ? " Do this " + std::to_string(options.repeats) +
                                  " times, coming back to centre each time."
                            : std::string();
    const std::string yaw_prompt = kYawPrompt + repeat_suffix;
    const std::string nod_prompt = kNodPrompt + repeat_suffix;
    const std::string tilt_prompt = kTiltPrompt + repeat_suffix;
    const double motion_seconds = 6.0 + 3.5 * static_cast<double>(options.repeats - 1);

    gt::Vec3 bias_body;
    if (!capture_still(device, options, progress, 1, still, bias_body, csv, error)) {
        return fail_run(progress, 1, kStillPrompt, error);
    }
    if (!capture_motion(device, options, progress, 2, gt::CalibrationPhase::Yaw, +1.0f, yaw_prompt,
                        motion_seconds, bias_body, yaw, csv, error)) {
        return fail_run(progress, 2, yaw_prompt, error);
    }
    gt::Vec3 nod_axis;
    if (!capture_motion(device, options, progress, 3, gt::CalibrationPhase::Nod, -1.0f, nod_prompt,
                        motion_seconds, bias_body, nod, csv, error, &nod_axis)) {
        return fail_run(progress, 3, nod_prompt, error);
    }
    live.set_measured_right(nod_axis);
    if (!capture_motion(device, options, progress, 4, gt::CalibrationPhase::Tilt, +1.0f, tilt_prompt,
                        motion_seconds, bias_body, tilt, csv, error)) {
        return fail_run(progress, 4, tilt_prompt, error);
    }

    gt::OrientationCalibrationOptions calibration_options;
    calibration_options.repeats = options.repeats;
    const gt::OrientationCalibrationResult result =
        gt::calibrate_orientation(still, yaw, nod, tilt, calibration_options);
    std::printf("[cal] begin v1 mag=off\n");
    std::printf("[cal] phase=still n=%zu gyro_rms=%.3fdeg/s\n", still.samples.size(),
                result.still_gyro_rms_degs);
    print_axis("yaw", result.yaw);
    print_axis("nod", result.nod);
    print_axis("tilt", result.tilt);
    std::printf("[cal] quality: up_vs_yaw=%.2fdeg nod_level_err=%.2fdeg nod_tilt_err=%.2fdeg "
                "handedness=%.2fdeg (smaller is straighter; redo wobbly steps)\n",
                result.up_vs_yaw_deg, result.nod_level_error_deg, result.nod_tilt_error_deg,
                result.handedness_error_deg);
    char consistency[192];
    std::snprintf(consistency, sizeof(consistency),
                  "up_vs_yaw %.2f deg, nod_level %.2f deg, nod_tilt %.2f deg, handedness %.2f deg",
                  result.up_vs_yaw_deg, result.nod_level_error_deg, result.nod_tilt_error_deg,
                  result.handedness_error_deg);
    std::string summary = consistency;
    if (result.repeats_requested > 1) {
        for (const auto& [name, stats] : {std::pair<const char*, const gt::AxisRepeatStats*>{"yaw", &result.yaw_repeats},
                                          {"nod", &result.nod_repeats},
                                          {"tilt", &result.tilt_repeats}}) {
            std::printf("[cal] repeats phase=%s detected=%d usable=%d used=%d rejected=%d spread=%.2fdeg%s\n",
                        name, stats->detected, stats->usable, stats->used, stats->rejected, stats->spread_deg,
                        stats->rejected > 0 ? " (outlier dropped)" : "");
        }
        char repeats_text[192];
        if (result.repeat_spread_deg >= 0.0f) {
            std::snprintf(repeats_text, sizeof(repeats_text),
                          "; %d repeats: spread %.2f deg, estimated axis error %.2f deg", result.repeats_requested,
                          result.repeat_spread_deg, result.axis_error_estimate_deg);
        } else {
            std::snprintf(repeats_text, sizeof(repeats_text), "; %d repeats requested, consistency not measurable",
                          result.repeats_requested);
        }
        std::printf("[cal] consistency%s, score %.2f\n", repeats_text + 1, result.consistency_quality);
        summary += repeats_text;
    }
    if (!result.ok) {
        std::printf("[cal] FAIL %s: %s\n", result.code.c_str(), result.message.c_str());
        std::printf("No calibration file was changed. Repeat the tool and follow the named step carefully.\n");
        return fail_run(progress, 4, "Calibration was not saved; start again.",
                        result.code + ": " + result.message + " (" + consistency + ")");
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

    if (cancel_requested_before_save()) {
        return fail_run(progress, 4, tilt_prompt, "cancelled");
    }
    const std::filesystem::path output_path = gt::path_from_utf8(options.output_path);
    if (!output_path.parent_path().empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(output_path.parent_path(), directory_error);
        if (directory_error) {
            std::printf("could not create calibration directory: %s\n",
                        directory_error.message().c_str());
            return fail_run(progress, 4, "Calibration was not saved.",
                            "SAVE_FAILED: " + system_error_utf8(directory_error));
        }
    }
    if (!gt::save_orientation_calibration(options.output_path, result, error)) {
        std::printf("calibration succeeded but could not be saved: %s\n", error.c_str());
        return fail_run(progress, 4, "Calibration was not saved.", "SAVE_FAILED: " + error);
    }
    const float quality = gt::calibration_quality(result);
    std::printf("[cal] result ok\nCalibration quality: %s (%.2f)\nSaved %s\n", gt::calibration_quality_label(quality),
                quality, options.output_path.c_str());
    if (quality < 0.4f) {
        std::printf("The steps disagreed with each other. The file was saved, but repeating the calibration%s "
                    "should give a straighter result.\n",
                    options.repeats > 1 ? "" : " (ideally with --repeats 3)");
    }
    gt::MagCalibration mag;
    std::string mag_error;
    std::string next;
    if (!gt::load_mag_calibration(options.output_path, mag, mag_error) || !mag.valid) {
        std::printf("Next: run orientation_calibrate.exe --mag once for drift-free heading.\n");
        next = "; next: calibrate the magnetometer (--mag) for drift-free heading";
    }
    progress.emit(4, "done", "Orientation calibration saved.", summary + next, quality);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** wide_argv) {
    // Wide entry point, as in spatial_desk: the controller passes UTF-8 paths
    // through a wide command line, and a narrow argv would decode them with
    // the ANSI code page ('?' or a best-fit letter for anything outside it,
    // i.e. a different or invalid --output path). Narrow to UTF-8 once; every
    // path below is decoded with path_from_utf8. Nothing touches the glasses
    // before the arguments parse.
    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<size_t>(argc < 0 ? 0 : argc));
    for (int i = 0; i < argc; ++i) {
        const std::wstring wide = wide_argv[i] != nullptr ? wide_argv[i] : L"";
        const std::string narrow = gt::utf8_from_wide_text(wide);
        if (!wide.empty() && narrow.empty()) {
            std::printf("invalid command line argument %d: not representable in UTF-8\n", i);
            return 2;
        }
        utf8_args.push_back(narrow);
    }
    std::vector<char*> argv;
    argv.reserve(utf8_args.size() + 1);
    for (std::string& argument : utf8_args) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    Options options;
    if (!parse_args(argc, argv.data(), options)) {
        return 2;
    }
    if (options.output_path.empty()) {
        options.output_path = default_output_path();
    }
    g_cleanup_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CleanupDoneSignal cleanup_done;  // before every device guard: destroyed after them
    SetConsoleCtrlHandler(console_handler, TRUE);
    if (options.auto_advance) {
        start_stdin_watcher();
    }
    const Progress setup(options, options.mag_only ? "mag" : "orientation", options.mag_only ? 2 : 4);
    const char* const kConnectPrompt = "Connect the glasses and try again.";

    if (hid_init() != 0) {
        std::printf("hidapi initialization failed\n");
        return fail_run(setup, 1, kConnectPrompt, "NO_HID: hidapi initialization failed");
    }
    struct HidExitGuard {
        ~HidExitGuard() { hid_exit(); }
    } hid_exit_guard;

    const auto devices = gt::GtHidDevice::enumerate();
    if (devices.empty()) {
        std::printf("no RayNeo GT HID device found; connect the glasses and retry\n");
        return fail_run(setup, 1, kConnectPrompt, "NO_DEVICE: no RayNeo GT HID device found");
    }
    for (const auto& device : devices) {
        std::printf("found: %s\n", gt::GtHidDevice::describe(device).c_str());
    }

    gt::GtHidDevice device;
    if (!device.open_first()) {
        std::printf("failed to open the RayNeo GT HID interface\n");
        return fail_run(setup, 1, kConnectPrompt,
                        "OPEN_FAILED: could not open the glasses (is another app using them?)");
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
        return fail_run(setup, 1, "Close other apps using the glasses and try again.",
                        "STREAM_FAILED: " + stream_error);
    }
    stream_guard.mark_started();
    drain(device, 100);

    std::ofstream csv;
    if (!options.log_path.empty()) {
        csv.open(gt::path_from_utf8(options.log_path), std::ios::out | std::ios::trunc);
        if (!csv) {
            std::printf("could not open raw log: %s\n", options.log_path.c_str());
            return fail_run(setup, 1, kConnectPrompt, "LOG_FAILED: could not open " + options.log_path);
        }
        csv << "phase,host_us,tick_100us,ax,ay,az,gx,gy,gz,mx,my,mz,temp_c\n";
    }

    if (options.mag_only) {
        return calibrate_magnetometer(device, options, csv);
    }
    return calibrate_orientation_flow(device, options, csv);
}
