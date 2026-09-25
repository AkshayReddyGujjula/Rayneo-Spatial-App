#pragma once

// Engine process owner.
//
// spatial_desk.exe is a separate process: it owns the D3D device, the glasses
// swapchain, the Parsec virtual displays and the IMU. The controller only
// launches it, watches it, sends the private window messages from
// app/engine_protocol.h, and stops it gracefully (a forced kill is a last
// resort after a deadline and is always reported).

#include "app_shell/engine_commands.h"

#include <windows.h>

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gt {

enum class EngineProcessState {
    Stopped,
    Starting,
    Running,
    Stopping,
    Exited,
    Failed,
};

const char* engine_process_state_text(EngineProcessState state);
const wchar_t* engine_process_state_label(EngineProcessState state);

struct EngineSnapshot {
    EngineProcessState state = EngineProcessState::Stopped;
    bool process_alive = false;
    bool window_found = false;
    bool owns_process = false;
    unsigned long process_id = 0;
    bool has_exit_code = false;
    int exit_code = 0;
    double uptime_s = 0.0;
    bool query_answered = false;
    unsigned query_flags = 0;
    int query_screens = 0;
    std::string last_error;
    std::string last_command_line;
    LaunchMode mode = LaunchMode::None;
};

class EngineClient {
public:
    EngineClient();
    ~EngineClient();
    EngineClient(const EngineClient&) = delete;
    EngineClient& operator=(const EngineClient&) = delete;

    bool launch(const EngineLaunchCommand& command, std::string& error);
    // Marks a cleanly reaped engine as failed when its status file says so.
    // The exit code is checked first; this only corrects what it missed.
    void note_status_failure(const std::string& detail);
    // Session teardown: request a graceful quit, but terminate a windowless
    // owned engine outright (the grace period will not be waited out).
    void abort_for_session_end();
    // Adopts a hand-launched engine window instead of starting a second
    // engine. Returns true when a foreign engine was found and adopted.
    bool adopt_if_running();
    void poll(double now_s);
    void request_stop();
    bool send_command(unsigned message, std::string& error);
    // Integer payload variant (view-comfort messages).
    bool send_command(unsigned message, WPARAM wparam, LPARAM lparam, std::string& error);
    bool launch_tool(const std::filesystem::path& tool, const std::vector<std::string>& arguments,
                     std::string& error);

    const EngineSnapshot& snapshot() const { return snapshot_; }
    HWND window() const { return window_; }
    bool busy() const;

private:
    void close_process();
    void release();
    void query_request(HWND window);
    bool query_consume();
    void query_worker_main();

    HANDLE process_ = nullptr;
    DWORD process_id_ = 0;
    HWND window_ = nullptr;
    double started_at_s_ = 0.0;
    double stop_deadline_s_ = 0.0;
    double foreign_stop_deadline_s_ = 0.0;
    bool stop_requested_ = false;
    bool kill_reported_ = false;
    bool start_timeout_reported_ = false;
    EngineSnapshot snapshot_;

    // The per-poll engine query runs on a worker thread: a render loop stuck
    // in Present would otherwise stall the UI thread for the full timeout on
    // every health poll. At most one query is ever outstanding.
    std::mutex query_mutex_;
    std::condition_variable query_wake_;
    bool query_shutdown_ = false;
    bool query_busy_ = false;
    HWND query_window_ = nullptr;
    unsigned long long query_sequence_ = 0;
    bool query_done_ = false;
    bool query_answered_ = false;
    unsigned query_flags_ = 0;
    unsigned long long query_done_sequence_ = 0;
    unsigned long long query_consumed_sequence_ = 0;
    // Declared last: the worker touches the mutex and flags above, so they
    // must already exist when the thread starts in the constructor body.
    std::thread query_thread_;
};

}  // namespace gt
