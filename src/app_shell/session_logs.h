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
// Shorter successful sessions are not archived; their files are cleared when
// the next session starts.
//
// A session whose engine failed is kept whatever its length. Its files stay in
// the log directory (so the Diagnostics page and "Open engine log" still show
// them) with a kFailedSessionMarker beside them; just before the next session
// clears the directory they are staged as failed-session-<start>.zip. The
// marker, or an engine-status.txt that says state=failed, also covers a
// controller that was restarted in between. Failed archives keep their own
// newest kSessionArchiveKeep, pruned separately, so failures never evict a
// good long session.

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace gt {

inline constexpr double kSessionArchiveMinSeconds = 600.0;
inline constexpr std::size_t kSessionArchiveKeep = 3;
inline constexpr const wchar_t* kArchiveSessionsSwitch = L"--archive-sessions";
// Written beside a failed session's live files: its archive stem, then the reason.
inline constexpr const char* kFailedSessionMarker = "session-failed.txt";

// The files one engine session writes into the log directory.
const std::vector<std::string>& session_log_names();

// A successful session is archived only when it ran for more than the minimum.
bool session_qualifies_for_archive(double duration_s);

// What the controller knows once the engine process is gone.
struct SessionEndEvidence {
    bool controller_failed = false;  // the controller showed Failed during the session
    bool has_exit_code = false;
    int exit_code = 0;
    bool status_failed = false;      // engine-status.txt says state=failed
};
bool session_ended_in_failure(const SessionEndEvidence& evidence);

// True when <log_dir>/engine-status.txt parses with state=failed. No freshness
// gate: the file belongs to the session being judged (the controller removes it
// before every launch). updated_unix receives the file's own stamp (0 if none).
bool session_status_reports_failure(const std::filesystem::path& log_dir,
                                    std::uint64_t* updated_unix = nullptr);

// "session-YYYY-MM-DD_HH-MM-SS" for the session's local start time; the name
// sorts chronologically.
std::string session_archive_stem(const std::tm& local_start);

// "failed-session-YYYY-MM-DD_HH-MM-SS".
std::string failed_session_archive_stem(const std::tm& local_start);

// True only for a published archive name ("session-...zip"), never for a
// pending folder, a temporary file or anything the user put there.
bool is_session_archive_name(const std::string& file_name);

// The same strict test for "failed-session-...zip".
bool is_failed_session_archive_name(const std::string& file_name);

// The archive names outside the newest `keep`, oldest first. Each function
// considers only its own class, so the two rolling sets never evict each other.
std::vector<std::string> session_archives_to_prune(std::vector<std::string> names, std::size_t keep);
std::vector<std::string> failed_session_archives_to_prune(std::vector<std::string> names,
                                                          std::size_t keep);

std::filesystem::path session_archive_dir(const std::filesystem::path& log_dir);

// Removes the previous session's live files (and the rotated generations older
// builds left behind, and a failed-session marker) so the engine starts on
// empty files.
void clear_session_logs(const std::filesystem::path& log_dir);

// Records that the session whose files are in log_dir failed: writes the marker
// with its failed-session stem and reason. The files themselves stay in place.
bool mark_session_failed(const std::filesystem::path& log_dir, const std::string& stem,
                         const std::string& reason, std::string& error);

// Called just before clear_session_logs. When the leftover session failed (a
// marker, or engine-status.txt with state=failed), moves its files and the
// marker into a failed-session pending folder and returns true with its stem.
// A marker's stem is used only when it is a well-formed failed-session name;
// otherwise the stem comes from the status file's stamp. Returns false with an
// empty error when there was no failed session to keep.
bool stage_failed_session_logs(const std::filesystem::path& log_dir, std::string& stem,
                               std::string& error);

// Moves the session's files into the pending folder (stem is a session or a
// failed-session stem). Returns false with an empty error when there was
// nothing to move.
bool stage_session_logs(const std::filesystem::path& log_dir, const std::string& stem,
                        std::string& error);

bool has_pending_session_logs(const std::filesystem::path& archive_dir);

// Starts the detached helper (this executable with kArchiveSessionsSwitch).
bool spawn_session_archiver(const std::filesystem::path& archive_dir, std::string& error);

// The named mutex that makes one helper own one archive directory's queue.
// Separate directories get separate names, so a helper for another log folder
// (a second copy of the app, a concurrent test run) never turns this one away.
std::wstring session_archiver_mutex_name(const std::filesystem::path& archive_dir);

// The helper's body: archives every pending folder, prunes, and returns the
// process exit code. A second concurrent helper for the same directory exits
// at once; the running one
// re-checks for new work before it lets go, so no pending folder is missed.
int run_session_archiver(const std::filesystem::path& archive_dir);

}  // namespace gt
