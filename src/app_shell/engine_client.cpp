#include "app_shell/engine_client.h"

#include "app/engine_protocol.h"
#include "app_shell/clock.h"
#include "app_shell/paths.h"

#include <cstdio>

namespace gt {
namespace {

// Measured display-restore time is 13-35 s; the grace must cover a full
// restore or quit kills the engine mid-run (stranded virtual desktops and
// a bogus Failed state). Termination stays as a last resort after this.
constexpr double kStopGraceSeconds = 45.0;
constexpr double kStartTimeoutSeconds = 30.0;
constexpr UINT kQueryTimeoutMs = 250;

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string last_error_text(const char* prefix) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s (Windows error %lu)", prefix, GetLastError());
    return std::string(buffer);
}

}  // namespace

const char* engine_process_state_text(EngineProcessState state) {
    switch (state) {
        case EngineProcessState::Starting:
            return "starting";
        case EngineProcessState::Running:
            return "running";
        case EngineProcessState::Stopping:
            return "stopping";
        case EngineProcessState::Exited:
            return "exited";
        case EngineProcessState::Failed:
            return "failed";
        case EngineProcessState::Stopped:
        default:
            return "stopped";
    }
}

const wchar_t* engine_process_state_label(EngineProcessState state) {
    switch (state) {
        case EngineProcessState::Starting:
            return L"Starting";
        case EngineProcessState::Running:
            return L"Running";
        case EngineProcessState::Stopping:
            return L"Stopping";
        case EngineProcessState::Exited:
            return L"Exited";
        case EngineProcessState::Failed:
            return L"Failed";
        case EngineProcessState::Stopped:
        default:
            return L"Stopped";
    }
}

EngineClient::EngineClient() {
    // Started in the body, not the mem-initializer: every member above
    // query_thread_ is fully constructed before the worker can touch it.
    query_thread_ = std::thread(&EngineClient::query_worker_main, this);
}

EngineClient::~EngineClient() {
    {
        std::lock_guard<std::mutex> lock(query_mutex_);
        query_shutdown_ = true;
    }
    query_wake_.notify_one();
    if (query_thread_.joinable()) {
        query_thread_.join();
    }
    release();
}

void EngineClient::query_worker_main() {
    for (;;) {
        HWND target = nullptr;
        {
            std::unique_lock<std::mutex> lock(query_mutex_);
            query_wake_.wait(lock, [&] { return query_shutdown_ || query_busy_; });
            if (query_shutdown_) {
                return;
            }
            target = query_window_;
        }
        DWORD_PTR result = 0;
        const LRESULT sent =
            SendMessageTimeoutW(target, static_cast<UINT>(kEngineMessageQuery), 0, 0,
                                SMTO_ABORTIFHUNG, kQueryTimeoutMs, &result);
        {
            std::lock_guard<std::mutex> lock(query_mutex_);
            query_answered_ = (sent != 0);
            query_flags_ = (sent != 0) ? static_cast<unsigned>(result) : 0u;
            query_done_sequence_ = query_sequence_;
            query_done_ = true;
            query_busy_ = false;
        }
    }
}

void EngineClient::query_request(HWND window) {
    std::lock_guard<std::mutex> lock(query_mutex_);
    if (query_busy_ || query_shutdown_) {
        return;
    }
    query_window_ = window;
    ++query_sequence_;
    query_busy_ = true;
    query_done_ = false;
    query_wake_.notify_one();
}

bool EngineClient::query_consume() {
    std::lock_guard<std::mutex> lock(query_mutex_);
    if (!query_done_ || query_done_sequence_ == query_consumed_sequence_) {
        return false;
    }
    query_consumed_sequence_ = query_done_sequence_;
    snapshot_.query_answered = query_answered_;
    snapshot_.query_flags = query_flags_;
    snapshot_.query_screens =
        static_cast<int>((query_flags_ & kEngineScreenCountMask) >> kEngineScreenCountShift);
    if (!query_answered_) {
        snapshot_.query_flags = 0;
        snapshot_.query_screens = 0;
    }
    return true;
}

bool EngineClient::busy() const {
    return process_ != nullptr || window_ != nullptr;
}

void EngineClient::close_process() {
    if (process_ != nullptr) {
        CloseHandle(process_);
        process_ = nullptr;
    }
    // The PID must not outlive the handle: a quickly started hand-launched
    // engine would otherwise be adopted with a stale PID and wrong ownership.
    process_id_ = 0;
}

void EngineClient::release() {
    close_process();
    process_id_ = 0;
    window_ = nullptr;
    stop_requested_ = false;
    kill_reported_ = false;
    start_timeout_reported_ = false;
    foreign_stop_deadline_s_ = 0.0;
}

bool EngineClient::launch(const EngineLaunchCommand& command, std::string& error) {
    if (command.command_line.empty() || command.executable.empty()) {
        error = "no engine command to launch";
        return false;
    }
    if (busy()) {
        error = "the engine is already running";
        return false;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE log_handle = INVALID_HANDLE_VALUE;
    if (!command.engine_log_path.empty()) {
        log_handle = CreateFileW(widen_utf8(command.engine_log_path).c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log_handle == INVALID_HANDLE_VALUE) {
            error = last_error_text(
                ("could not open the engine log file '" + command.engine_log_path + "'").c_str());
            return false;
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (log_handle != INVALID_HANDLE_VALUE) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = log_handle;
        startup.hStdError = log_handle;
        // The engine never reads stdin; leave it null rather than inheriting
        // a possibly non-inheritable console handle.
        startup.hStdInput = nullptr;
    }
    PROCESS_INFORMATION process_info{};
    std::wstring command_line = widen_utf8(command.command_line);
    std::wstring working_directory = widen_utf8(command.working_directory);
    const BOOL created = CreateProcessW(
        nullptr, command_line.data(), nullptr, nullptr, log_handle != INVALID_HANDLE_VALUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &process_info);
    const DWORD create_error = GetLastError();
    if (log_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(log_handle);
    }
    if (!created) {
        SetLastError(create_error);
        error = last_error_text("could not start spatial_desk.exe");
        return false;
    }
    CloseHandle(process_info.hThread);
    process_ = process_info.hProcess;
    process_id_ = process_info.dwProcessId;

    snapshot_.state = EngineProcessState::Starting;
    snapshot_.process_alive = true;
    snapshot_.window_found = false;
    snapshot_.owns_process = true;
    snapshot_.process_id = process_id_;
    snapshot_.has_exit_code = false;
    snapshot_.exit_code = 0;
    snapshot_.uptime_s = 0.0;
    snapshot_.query_answered = false;
    snapshot_.query_flags = 0;
    snapshot_.query_screens = 0;
    snapshot_.last_error.clear();
    snapshot_.last_command_line = command.command_line;
    snapshot_.mode = command.mode;
    started_at_s_ = steady_now_s();
    stop_requested_ = false;
    stop_deadline_s_ = 0.0;
    foreign_stop_deadline_s_ = 0.0;
    kill_reported_ = false;
    start_timeout_reported_ = false;
    window_ = nullptr;
    return true;
}

bool EngineClient::adopt_if_running() {
    if (busy()) {
        return false;
    }
    HWND candidate = FindWindowW(kEngineWindowClass, nullptr);
    if (candidate == nullptr || !IsWindow(candidate)) {
        return false;
    }
    DWORD owner = 0;
    GetWindowThreadProcessId(candidate, &owner);
    window_ = candidate;
    process_id_ = owner;
    snapshot_.state = EngineProcessState::Running;
    snapshot_.process_alive = false;
    snapshot_.window_found = true;
    snapshot_.owns_process = false;
    snapshot_.process_id = owner;
    snapshot_.has_exit_code = false;
    snapshot_.exit_code = 0;
    snapshot_.uptime_s = 0.0;
    snapshot_.query_answered = false;
    snapshot_.query_flags = 0;
    snapshot_.query_screens = 0;
    snapshot_.last_error.clear();
    snapshot_.last_command_line.clear();
    return true;
}

void EngineClient::poll(double now_s) {
    if (window_ != nullptr && !IsWindow(window_)) {
        window_ = nullptr;
    }
    if (window_ == nullptr) {
        HWND candidate = FindWindowW(kEngineWindowClass, nullptr);
        if (candidate != nullptr) {
            DWORD owner = 0;
            GetWindowThreadProcessId(candidate, &owner);
            if (process_ != nullptr && owner != process_id_) {
                // Another engine instance (hand-launched); never present a
                // foreign window as the owned process.
            } else {
                window_ = candidate;
                if (process_ == nullptr) {
                    process_id_ = owner;
                    snapshot_.process_id = owner;
                    snapshot_.owns_process = false;
                }
            }
        }
    }
    snapshot_.window_found = window_ != nullptr;

    if (window_ != nullptr) {
        // Asynchronous: while a query is outstanding the previous answer
        // stays on screen instead of stalling the UI thread. A completed
        // but unanswered query clears the last answer (the engine is gone).
        query_request(window_);
        query_consume();
    } else {
        snapshot_.query_answered = false;
        snapshot_.query_flags = 0;
        snapshot_.query_screens = 0;
    }

    if (process_ != nullptr) {
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(process_, &exit_code)) {
            snapshot_.last_error = last_error_text("could not query the engine process");
            close_process();
            snapshot_.state = EngineProcessState::Failed;
            snapshot_.process_alive = false;
            return;
        }
        if (exit_code == STILL_ACTIVE) {
            snapshot_.process_alive = true;
            snapshot_.uptime_s = now_s - started_at_s_;
            if (stop_requested_) {
                if (snapshot_.state != EngineProcessState::Failed) {
                    snapshot_.state = EngineProcessState::Stopping;
                }
                if (now_s >= stop_deadline_s_) {
                    TerminateProcess(process_, 1);
                    kill_reported_ = true;
                    if (!start_timeout_reported_) {
                        snapshot_.last_error =
                            "the engine did not stop within 8 s and was terminated; its virtual "
                            "displays are removed by the driver watchdog; if the laptop display "
                            "stays off use Recover displays or press Win+P";
                    }
                    snapshot_.state = EngineProcessState::Failed;
                }
            } else if (snapshot_.window_found) {
                snapshot_.state = EngineProcessState::Running;
            } else if (!start_timeout_reported_ && now_s - started_at_s_ >= kStartTimeoutSeconds) {
                start_timeout_reported_ = true;
                request_stop();
                snapshot_.last_error = "the engine did not create its window within 30 s";
                snapshot_.state = EngineProcessState::Failed;
            } else if (!start_timeout_reported_) {
                snapshot_.state = EngineProcessState::Starting;
            }
            return;
        }
        snapshot_.has_exit_code = true;
        snapshot_.exit_code = static_cast<int>(exit_code);
        snapshot_.process_alive = false;
        snapshot_.uptime_s = 0.0;
        close_process();
        if (kill_reported_) {
            // The stop grace already expired and the termination was reported
            // as Failed; the reaped exit must not downgrade it to Stopped.
            snapshot_.state = EngineProcessState::Failed;
            stop_requested_ = false;
            kill_reported_ = false;
        } else if (stop_requested_) {
            snapshot_.state = EngineProcessState::Stopped;
            stop_requested_ = false;
        } else if (exit_code == 0) {
            snapshot_.state = EngineProcessState::Exited;
        } else {
            snapshot_.state = EngineProcessState::Failed;
            if (snapshot_.last_error.empty()) {
                char buffer[128];
                std::snprintf(buffer, sizeof(buffer), "engine exited with code %lu",
                              static_cast<unsigned long>(exit_code));
                snapshot_.last_error = buffer;
            }
        }
        return;
    }

    snapshot_.process_alive = false;
    if (window_ != nullptr) {
        snapshot_.owns_process = false;
        if (stop_requested_ && now_s >= foreign_stop_deadline_s_) {
            stop_requested_ = false;
            snapshot_.last_error = "the engine did not respond to the quit request";
            snapshot_.state = EngineProcessState::Failed;
        } else if (stop_requested_) {
            snapshot_.state = EngineProcessState::Stopping;
        } else {
            snapshot_.state = EngineProcessState::Running;
        }
        return;
    }
    stop_requested_ = false;
    if (snapshot_.state == EngineProcessState::Starting ||
        snapshot_.state == EngineProcessState::Running ||
        snapshot_.state == EngineProcessState::Stopping) {
        snapshot_.state = EngineProcessState::Stopped;
        snapshot_.uptime_s = 0.0;
    }
}

void EngineClient::request_stop() {
    if (window_ == nullptr) {
        window_ = FindWindowW(kEngineWindowClass, nullptr);
    }
    if (window_ != nullptr) {
        DWORD_PTR result = 0;
        SendMessageTimeoutW(window_, static_cast<UINT>(kEngineMessageQuit), 0, 0, SMTO_ABORTIFHUNG,
                            kQueryTimeoutMs, &result);
    }
    if (process_ != nullptr) {
        stop_requested_ = true;
        stop_deadline_s_ = steady_now_s() + kStopGraceSeconds;
        snapshot_.state = EngineProcessState::Stopping;
    } else if (window_ != nullptr) {
        // Foreign (hand-launched) engine: the quit message is advisory, so
        // watch the same grace period and report if it is ignored.
        stop_requested_ = true;
        foreign_stop_deadline_s_ = steady_now_s() + kStopGraceSeconds;
        snapshot_.state = EngineProcessState::Stopping;
    } else if (snapshot_.state != EngineProcessState::Failed &&
               snapshot_.state != EngineProcessState::Exited) {
        // Idle with nothing to stop: leave terminal states (Failed/Exited)
        // alone so their banner and tooltip survive.
        snapshot_.state = EngineProcessState::Stopped;
    }
}

void EngineClient::note_status_failure(const std::string& detail) {
    if (snapshot_.state != EngineProcessState::Exited) {
        return;
    }
    snapshot_.state = EngineProcessState::Failed;
    if (snapshot_.last_error.empty() && !detail.empty()) {
        snapshot_.last_error = detail;
    }
}

void EngineClient::abort_for_session_end() {
    request_stop();
    if (process_ != nullptr && window_ == nullptr) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(process_, &exit_code) && exit_code == STILL_ACTIVE) {
            // Windowless owned engine (still starting): session teardown
            // will not wait out the grace period, so stop it now.
            TerminateProcess(process_, 1);
        }
    }
}

bool EngineClient::send_command(unsigned message, std::string& error) {
    return send_command(message, 0, 0, error);
}

bool EngineClient::send_command(unsigned message, WPARAM wparam, LPARAM lparam,
                                std::string& error) {
    if (window_ == nullptr) {
        window_ = FindWindowW(kEngineWindowClass, nullptr);
    }
    if (window_ == nullptr) {
        error = "the engine is not running";
        return false;
    }
    if (process_ != nullptr) {
        DWORD owner = 0;
        GetWindowThreadProcessId(window_, &owner);
        if (owner != process_id_) {
            // A foreign window (hand-launched engine) must never receive
            // commands meant for the owned process, e.g. during its startup.
            window_ = nullptr;
            error = "the engine is not ready yet";
            return false;
        }
    }
    DWORD_PTR result = 0;
    const LRESULT sent = SendMessageTimeoutW(window_, static_cast<UINT>(message), wparam, lparam,
                                             SMTO_ABORTIFHUNG, kQueryTimeoutMs, &result);
    if (sent == 0) {
        error = last_error_text("the engine did not answer");
        return false;
    }
    return true;
}

bool EngineClient::launch_tool(const std::filesystem::path& tool,
                               const std::vector<std::string>& arguments, std::string& error) {
    if (tool.empty()) {
        error = "tool path is empty";
        return false;
    }
    // The executable path goes through the shared quoter (backslash runs
    // before a quote are doubled) and the wide API, so staging folders with
    // spaces, quotes or non-ASCII characters still resolve.
    std::wstring wide_command = widen_utf8(quote_command_line_argument(narrow_utf8(tool)));
    for (const std::string& argument : arguments) {
        wide_command += L" ";
        wide_command += widen_utf8(quote_command_line_argument(argument));
    }
    std::wstring working_directory = tool.parent_path().wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        working_directory.empty() ? nullptr : working_directory.c_str(), &startup,
                        &process_info)) {
        error = last_error_text("could not start the tool");
        return false;
    }
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return true;
}

}  // namespace gt
