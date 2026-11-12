#pragma once

// Runs orientation_calibrate.exe with --progress-json behind pipes so the
// controller can render its progress in-app.
//
// A reader thread splits the tool's stdout into lines and posts notify_message
// to the window whenever lines or the exit arrive (no polling). The UI thread
// drains them with take(). stdin stays open so "I'm ready" can answer the
// tool's ENTER prompt; cancel() answers "q" and closes stdin (the tool's own
// cancel path, which turns the IMU stream back off), and terminate() is the
// last resort after a grace period.

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gt {

class CalibrationRunner {
public:
    CalibrationRunner() = default;
    ~CalibrationRunner();
    CalibrationRunner(const CalibrationRunner&) = delete;
    CalibrationRunner& operator=(const CalibrationRunner&) = delete;

    bool start(const std::filesystem::path& tool, const std::vector<std::string>& arguments, HWND notify,
               UINT notify_message, std::string& error);
    // Writes one line (newline appended) to the tool's stdin.
    bool send_line(const std::string& line);
    void cancel();
    void terminate();
    bool active() const { return process_ != nullptr; }
    double cancel_requested_at() const { return cancel_at_s_; }
    void set_cancel_time(double now_s) { cancel_at_s_ = now_s; }

    struct Drain {
        std::vector<std::string> lines;
        bool exited = false;
        int exit_code = 0;
    };
    // Moves out pending lines and, once, the exit. After an exit the process
    // handles are released and active() turns false.
    Drain take();

private:
    void reader_main();
    void release();

    HANDLE process_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HWND notify_ = nullptr;
    UINT message_ = 0;
    std::thread reader_;
    std::mutex mutex_;
    std::vector<std::string> pending_;
    bool exited_ = false;
    int exit_code_ = 0;
    double cancel_at_s_ = -1.0;
};

}  // namespace gt
