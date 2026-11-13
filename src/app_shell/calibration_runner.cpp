#include "app_shell/calibration_runner.h"

#include "app_shell/engine_commands.h"
#include "app_shell/paths.h"

#include <utility>

namespace gt {
namespace {

std::string last_error_message(const char* what) {
    return std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")";
}

void close_handle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

}  // namespace

CalibrationRunner::~CalibrationRunner() {
    if (process_ != nullptr) {
        terminate();
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    release();
}

bool CalibrationRunner::start(const std::filesystem::path& tool, const std::vector<std::string>& arguments,
                              HWND notify, UINT notify_message, std::string& error) {
    if (process_ != nullptr) {
        error = "a calibration is already running";
        return false;
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    release();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
        exited_ = false;
        exit_code_ = 0;
    }
    cancel_at_s_ = -1.0;
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE stdout_write = nullptr;
    HANDLE stdin_read = nullptr;
    if (!CreatePipe(&stdout_read_, &stdout_write, &inherit, 0) ||
        !SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stdin_read, &stdin_write_, &inherit, 0) ||
        !SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0)) {
        error = last_error_message("could not create the calibration pipes");
        close_handle(stdout_write);
        close_handle(stdin_read);
        release();
        return false;
    }
    std::wstring command = wide_from_utf8(quote_command_line_argument(narrow_utf8(tool)));
    for (const std::string& argument : arguments) {
        command += L" ";
        command += wide_from_utf8(quote_command_line_argument(argument));
    }
    const std::wstring working_directory = tool.parent_path().wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stdout_write;
    startup.hStdInput = stdin_read;
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                       working_directory.empty() ? nullptr : working_directory.c_str(),
                                       &startup, &info);
    // The child owns its ends now (or never will): close ours so EOF reaches
    // the reader when the tool exits.
    close_handle(stdout_write);
    close_handle(stdin_read);
    if (!created) {
        error = last_error_message("could not start orientation_calibrate.exe");
        release();
        return false;
    }
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    notify_ = notify;
    message_ = notify_message;
    reader_ = std::thread(&CalibrationRunner::reader_main, this);
    return true;
}

void CalibrationRunner::reader_main() {
    std::string partial;
    char buffer[1024];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(stdout_read_, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
            break;
        }
        partial.append(buffer, read);
        std::vector<std::string> lines;
        size_t start = 0;
        for (size_t i = 0; i < partial.size(); ++i) {
            if (partial[i] == '\n') {
                lines.emplace_back(partial.substr(start, i - start));
                start = i + 1;
            }
        }
        partial.erase(0, start);
        if (partial.size() > 64 * 1024) {
            lines.emplace_back(std::move(partial));
            partial.clear();
        }
        if (!lines.empty()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (std::string& line : lines) {
                    pending_.push_back(std::move(line));
                }
            }
            PostMessageW(notify_, message_, 0, 0);
        }
    }
    if (!partial.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(partial);
    }
    DWORD code = 1;
    WaitForSingleObject(process_, 10000);
    GetExitCodeProcess(process_, &code);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exited_ = true;
        exit_code_ = static_cast<int>(code);
    }
    PostMessageW(notify_, message_, 0, 0);
}

bool CalibrationRunner::send_line(const std::string& line) {
    if (stdin_write_ == nullptr) {
        return false;
    }
    const std::string payload = line + "\n";
    DWORD written = 0;
    return WriteFile(stdin_write_, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) != 0 &&
           written == payload.size();
}

void CalibrationRunner::cancel() {
    send_line("q");
    close_handle(stdin_write_);
}

bool CalibrationRunner::wait_for_exit(DWORD timeout_ms) const {
    return process_ == nullptr || WaitForSingleObject(process_, timeout_ms) == WAIT_OBJECT_0;
}

void CalibrationRunner::terminate() {
    if (process_ != nullptr) {
        TerminateProcess(process_, 1);
    }
    close_handle(stdin_write_);
}

CalibrationRunner::Drain CalibrationRunner::take() {
    Drain drain;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drain.lines.swap(pending_);
        drain.exited = exited_;
        drain.exit_code = exit_code_;
        exited_ = false;
    }
    if (drain.exited) {
        if (reader_.joinable()) {
            reader_.join();
        }
        release();
    }
    return drain;
}

void CalibrationRunner::release() {
    close_handle(process_);
    close_handle(stdout_read_);
    close_handle(stdin_write_);
    cancel_at_s_ = -1.0;
}

}  // namespace gt
