#pragma once

// Per-session log archive.
//
// Every engine session starts with fresh log files. When a session ends after
// more than kSessionArchiveMinSeconds, the controller moves its files into
// logs/sessions/.pending-<stem>/ (a same-volume rename, so it is instant and the
// next session can start at once) and launches a detached
// "RayNeo Spatial.exe --archive-sessions <dir>" helper. The helper compresses
// each pending folder with the inbox tar.exe into a temporary file, publishes it
// as logs/sessions/<stem>.zip only once it is complete, and keeps the newest
// kSessionArchiveKeep archives. Because the helper outlives the controller,
// quitting never waits on compression, and a pending folder left behind by a
// crash or a power cut is archived the next time the controller starts.
// Shorter sessions are not archived; their files are cleared when the next
// session starts.

#include <cstddef>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace gt {

inline constexpr double kSessionArchiveMinSeconds = 600.0;
inline constexpr std::size_t kSessionArchiveKeep = 3;
inline constexpr const wchar_t* kArchiveSessionsSwitch = L"--archive-sessions";

// The files one engine session writes into the log directory.
const std::vector<std::string>& session_log_names();

// A session is archived only when it ran for more than the minimum.
bool session_qualifies_for_archive(double duration_s);

// "session-YYYY-MM-DD_HH-MM-SS" for the session's local start time; the name
// sorts chronologically.
std::string session_archive_stem(const std::tm& local_start);

// True only for a published archive name ("session-...zip"), never for a
// pending folder, a temporary file or anything the user put there.
bool is_session_archive_name(const std::string& file_name);

// The archive names outside the newest `keep`, oldest first.
std::vector<std::string> session_archives_to_prune(std::vector<std::string> names, std::size_t keep);

std::filesystem::path session_archive_dir(const std::filesystem::path& log_dir);

// Removes the previous session's live files (and the rotated generations older
// builds left behind) so the engine starts on empty files.
void clear_session_logs(const std::filesystem::path& log_dir);

// Moves the session's files into the pending folder. Returns false with an
// empty error when there was nothing to move.
bool stage_session_logs(const std::filesystem::path& log_dir, const std::string& stem,
                        std::string& error);

bool has_pending_session_logs(const std::filesystem::path& archive_dir);

// Starts the detached helper (this executable with kArchiveSessionsSwitch).
bool spawn_session_archiver(const std::filesystem::path& archive_dir, std::string& error);

// The helper's body: archives every pending folder, prunes, and returns the
// process exit code. A second concurrent helper exits at once; the running one
// re-checks for new work before it lets go, so no pending folder is missed.
int run_session_archiver(const std::filesystem::path& archive_dir);

}  // namespace gt
