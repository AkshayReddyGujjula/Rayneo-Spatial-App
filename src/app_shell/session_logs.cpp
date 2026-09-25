#include "app_shell/session_logs.h"

#include "util/utf8_path.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace gt {

namespace {

constexpr const wchar_t* kArchiverMutex = L"Local\\RayNeoSpatialSessionArchiver";
constexpr const char* kPendingPrefix = ".pending-";
constexpr const char* kErrorFile = "archive-error.txt";
constexpr int kLegacyRotatedGenerations = 9;

std::filesystem::path pending_dir(const std::filesystem::path& archive_dir, const std::string& stem) {
    return archive_dir / path_from_utf8(kPendingPrefix + stem);
}

// Pending folders, oldest first. Only well-formed names are touched.
std::vector<std::filesystem::path> list_pending(const std::filesystem::path& archive_dir) {
    std::vector<std::filesystem::path> pending;
    std::error_code error;
    std::filesystem::directory_iterator it(archive_dir, error);
    if (error) {
        return pending;
    }
    const std::string prefix = kPendingPrefix;
    for (const auto& entry : it) {
        std::error_code type_error;
        if (!entry.is_directory(type_error)) {
            continue;
        }
        const std::string name = utf8_from_path(entry.path().filename());
        if (name.rfind(prefix, 0) == 0 &&
            is_session_archive_name(name.substr(prefix.size()) + ".zip")) {
            pending.push_back(entry.path());
        }
    }
    std::sort(pending.begin(), pending.end());
    return pending;
}

void write_error(const std::filesystem::path& archive_dir, const std::string& text) {
    std::ofstream out(archive_dir / kErrorFile, std::ios::out | std::ios::trunc);
    out << text << "\n";
}

std::wstring quote_arg(const std::wstring& text) {
    return L"\"" + text + L"\"";
}

// Runs a process without a window and waits for it. Returns false when it
// could not start; exit_code carries its result otherwise.
bool run_hidden(const std::wstring& application, std::wstring command_line, DWORD& exit_code) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(application.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup,
                        &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    return true;
}

// Compresses one pending folder into <stem>.zip. The archive is written under a
// temporary name and renamed only when tar succeeded, so a published name is
// always a complete archive.
bool archive_one(const std::filesystem::path& archive_dir, const std::filesystem::path& pending,
                 std::string& error) {
    const std::string stem = utf8_from_path(pending.filename()).substr(std::string(kPendingPrefix).size());
    const std::filesystem::path final_zip = archive_dir / path_from_utf8(stem + ".zip");
    const std::filesystem::path partial = archive_dir / path_from_utf8("." + stem + ".partial");
    std::error_code fs_error;

    std::vector<std::wstring> files;
    for (const auto& entry : std::filesystem::directory_iterator(pending, fs_error)) {
        std::error_code type_error;
        if (entry.is_regular_file(type_error)) {
            files.push_back(entry.path().filename().wstring());
        }
    }
    if (fs_error) {
        error = "could not read " + utf8_from_path(pending) + ": " + fs_error.message();
        return false;
    }
    // Already published (a previous helper stopped before its cleanup) or
    // nothing to keep: only the folder is left to remove.
    if (files.empty() || std::filesystem::exists(final_zip, fs_error)) {
        std::filesystem::remove_all(pending, fs_error);
        return true;
    }

    // The inbox bsdtar, by absolute path: a tar.exe found on PATH (Git's GNU
    // tar) silently writes a tar stream instead of a zip.
    wchar_t system_dir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        error = "could not locate the Windows system directory";
        return false;
    }
    const std::wstring tar = std::wstring(system_dir) + L"\\tar.exe";
    std::wstring command = quote_arg(tar) + L" --format zip -c -f " + quote_arg(partial.wstring()) +
                           L" -C " + quote_arg(pending.wstring());
    for (const std::wstring& file : files) {
        command += L" " + quote_arg(file);
    }

    std::filesystem::remove(partial, fs_error);
    DWORD exit_code = 1;
    if (!run_hidden(tar, command, exit_code)) {
        error = "could not start tar.exe (Windows 10 1803 or later is required)";
        return false;
    }
    if (exit_code != 0 || !std::filesystem::exists(partial, fs_error)) {
        std::filesystem::remove(partial, fs_error);
        error = "tar.exe failed with exit code " + std::to_string(exit_code) + " for " + stem;
        return false;
    }
    if (!MoveFileExW(partial.c_str(), final_zip.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::filesystem::remove(partial, fs_error);
        error = "could not publish " + stem + ".zip";
        return false;
    }
    std::filesystem::remove_all(pending, fs_error);
    return true;
}

void prune_archives(const std::filesystem::path& archive_dir) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(archive_dir, error)) {
        std::error_code type_error;
        if (entry.is_regular_file(type_error)) {
            names.push_back(utf8_from_path(entry.path().filename()));
        }
    }
    for (const std::string& name : session_archives_to_prune(names, kSessionArchiveKeep)) {
        std::filesystem::remove(archive_dir / path_from_utf8(name), error);
    }
}

}  // namespace

const std::vector<std::string>& session_log_names() {
    static const std::vector<std::string> names = {"engine.log", "telemetry.csv", "imu_raw.csv",
                                                   "engine-status.txt"};
    return names;
}

bool session_qualifies_for_archive(double duration_s) {
    return duration_s > kSessionArchiveMinSeconds;
}

std::string session_archive_stem(const std::tm& local_start) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "session-%04d-%02d-%02d_%02d-%02d-%02d",
                  local_start.tm_year + 1900, local_start.tm_mon + 1, local_start.tm_mday,
                  local_start.tm_hour, local_start.tm_min, local_start.tm_sec);
    return buffer;
}

bool is_session_archive_name(const std::string& file_name) {
    // session-YYYY-MM-DD_HH-MM-SS.zip
    static const std::string pattern = "session-0000-00-00_00-00-00.zip";
    if (file_name.size() != pattern.size()) {
        return false;
    }
    for (size_t i = 0; i < pattern.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(file_name[i]);
        if (pattern[i] == '0' ? !std::isdigit(c) : file_name[i] != pattern[i]) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> session_archives_to_prune(std::vector<std::string> names, std::size_t keep) {
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::string& name) { return !is_session_archive_name(name); }),
                names.end());
    std::sort(names.begin(), names.end());
    if (names.size() <= keep) {
        return {};
    }
    names.resize(names.size() - keep);
    return names;
}

std::filesystem::path session_archive_dir(const std::filesystem::path& log_dir) {
    return log_dir / "sessions";
}

void clear_session_logs(const std::filesystem::path& log_dir) {
    std::error_code error;
    for (const std::string& name : session_log_names()) {
        const std::filesystem::path live = log_dir / path_from_utf8(name);
        std::filesystem::remove(live, error);
        for (int generation = 1; generation <= kLegacyRotatedGenerations; ++generation) {
            std::filesystem::path rotated = live;
            rotated += "." + std::to_string(generation);
            std::filesystem::remove(rotated, error);
        }
    }
}

bool stage_session_logs(const std::filesystem::path& log_dir, const std::string& stem,
                        std::string& error) {
    error.clear();
    const std::filesystem::path target = pending_dir(session_archive_dir(log_dir), stem);
    std::error_code fs_error;
    std::filesystem::create_directories(target, fs_error);
    if (fs_error) {
        error = "could not create " + utf8_from_path(target) + ": " + fs_error.message();
        return false;
    }
    int moved = 0;
    for (const std::string& name : session_log_names()) {
        const std::filesystem::path live = log_dir / path_from_utf8(name);
        if (!std::filesystem::exists(live, fs_error)) {
            continue;
        }
        std::filesystem::rename(live, target / path_from_utf8(name), fs_error);
        if (fs_error) {
            error = "could not move " + name + ": " + fs_error.message();
        } else {
            ++moved;
        }
    }
    if (moved == 0) {
        std::filesystem::remove_all(target, fs_error);
        return false;
    }
    return true;
}

bool has_pending_session_logs(const std::filesystem::path& archive_dir) {
    return !list_pending(archive_dir).empty();
}

bool spawn_session_archiver(const std::filesystem::path& archive_dir, std::string& error) {
    std::wstring executable(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0) {
            error = "could not resolve the controller executable";
            return false;
        }
        if (length < executable.size()) {
            executable.resize(length);
            break;
        }
        executable.resize(executable.size() * 2);
    }
    std::wstring command = quote_arg(executable) + L" " + kArchiveSessionsSwitch + L" " +
                           quote_arg(archive_dir.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup,
                        &process)) {
        error = "could not start the log archiver (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

int run_session_archiver(const std::filesystem::path& archive_dir) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kArchiverMutex);
    if (mutex == nullptr) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another helper owns the queue and re-checks it before it exits.
        CloseHandle(mutex);
        return 0;
    }
    std::error_code fs_error;
    for (;;) {
        const std::vector<std::filesystem::path> pending = list_pending(archive_dir);
        if (pending.empty()) {
            // Release first, then look once more: work staged after the last
            // scan is either seen here or picked up by the helper that was
            // launched with it (which can now take the mutex).
            ReleaseMutex(mutex);
            CloseHandle(mutex);
            if (list_pending(archive_dir).empty()) {
                return 0;
            }
            mutex = CreateMutexW(nullptr, TRUE, kArchiverMutex);
            if (mutex == nullptr) {
                return 1;
            }
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                CloseHandle(mutex);
                return 0;
            }
            continue;
        }
        for (const std::filesystem::path& folder : pending) {
            std::string error;
            if (!archive_one(archive_dir, folder, error)) {
                // Keep the pending folder (the logs are not lost) and stop:
                // the next controller start retries.
                write_error(archive_dir, error);
                ReleaseMutex(mutex);
                CloseHandle(mutex);
                return 2;
            }
        }
        std::filesystem::remove(archive_dir / kErrorFile, fs_error);
        prune_archives(archive_dir);
    }
}

}  // namespace gt
