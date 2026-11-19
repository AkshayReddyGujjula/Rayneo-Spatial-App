#include "vdd/display_scale.h"

#include "vdd/display_config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace gt {
namespace {

// Undocumented DisplayConfig device-info types for the per-monitor scale.
const DISPLAYCONFIG_DEVICE_INFO_TYPE kGetDpiScale = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(0xFFFFFFFDu);
const DISPLAYCONFIG_DEVICE_INFO_TYPE kSetDpiScale = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(0xFFFFFFFCu);

struct GetDpiScaleInfo {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 min_scale_rel;
    INT32 cur_scale_rel;
    INT32 max_scale_rel;
};

struct SetDpiScaleInfo {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 scale_rel;
};

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr,
                            nullptr);
    }
    return out;
}

// The active path's source (adapter LUID + source id) for a GDI device name.
bool find_source(const std::wstring& gdi_name, LUID& adapter, UINT32& source_id, std::string& error) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 4 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (result != ERROR_SUCCESS) {
            break;
        }
        paths.resize(path_count);
        modes.resize(mode_count);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                    nullptr);
        if (result == ERROR_SUCCESS) {
            paths.resize(path_count);
        }
    }
    if (result != ERROR_SUCCESS) {
        error = "QueryDisplayConfig failed (" + std::to_string(result) + ")";
        return false;
    }
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        if (_wcsicmp(source.viewGdiDeviceName, gdi_name.c_str()) == 0) {
            adapter = path.sourceInfo.adapterId;
            source_id = path.sourceInfo.id;
            return true;
        }
    }
    error = "no active display path for " + narrow(gdi_name);
    return false;
}

}  // namespace

int display_scale_step_index(int percent) {
    for (int i = 0; i < kDisplayScaleStepCount; ++i) {
        if (kDisplayScaleSteps[i] == percent) {
            return i;
        }
    }
    return -1;
}

int display_scale_percent_at(const DisplayScaleInfo& info, int rel) {
    const int index = -info.min_rel + rel;
    if (index < 0 || index >= kDisplayScaleStepCount) {
        return 0;
    }
    return kDisplayScaleSteps[index];
}

DisplayScaleTarget plan_display_scale(const DisplayScaleInfo& info, int percent) {
    DisplayScaleTarget target;
    const int index = display_scale_step_index(percent);
    const int recommended = -info.min_rel;
    if (index < 0 || recommended < 0 || recommended >= kDisplayScaleStepCount || info.max_rel < info.min_rel) {
        return target;
    }
    // Windows accepts and reads back a rel above max but clamps the effective
    // DPI (measured: 200% on a 175% display), so never write past max.
    const int lowest = info.min_rel;
    int highest = info.max_rel;
    if (recommended + highest >= kDisplayScaleStepCount) {
        highest = kDisplayScaleStepCount - 1 - recommended;
    }
    int rel = index - recommended;
    if (rel > highest) {
        rel = highest;
        target.clamped = true;
    } else if (rel < lowest) {
        rel = lowest;
        target.clamped = true;
    }
    target.valid = true;
    target.rel = rel;
    target.percent = kDisplayScaleSteps[recommended + rel];
    return target;
}

bool query_display_scale(const std::wstring& gdi_name, DisplayScaleInfo& info, std::string& error) {
    GetDpiScaleInfo request{};
    if (!find_source(gdi_name, request.header.adapterId, request.header.id, error)) {
        return false;
    }
    request.header.type = kGetDpiScale;
    request.header.size = sizeof(request);
    const LONG result = DisplayConfigGetDeviceInfo(&request.header);
    if (result != ERROR_SUCCESS) {
        error = "DisplayConfigGetDeviceInfo(scale) failed (" + std::to_string(result) + ")";
        return false;
    }
    info.min_rel = request.min_scale_rel;
    info.cur_rel = request.cur_scale_rel;
    info.max_rel = request.max_scale_rel;
    return true;
}

bool set_display_scale_rel(const std::wstring& gdi_name, int rel, std::string& error) {
    SetDpiScaleInfo request{};
    if (!find_source(gdi_name, request.header.adapterId, request.header.id, error)) {
        return false;
    }
    request.header.type = kSetDpiScale;
    request.header.size = sizeof(request);
    request.scale_rel = rel;
    const LONG result = DisplayConfigSetDeviceInfo(&request.header);
    if (result != ERROR_SUCCESS) {
        error = "DisplayConfigSetDeviceInfo(scale) failed (" + std::to_string(result) + ")";
        return false;
    }
    return true;
}

DisplayScaleOps win32_display_scale_ops() {
    DisplayScaleOps ops;
    ops.query = query_display_scale;
    ops.set = set_display_scale_rel;
    ops.identities = live_device_identities;
    ops.virtual_displays = [] { return active_virtual_display_names(enumerate_virtual_displays()); };
    ops.log = [](const std::string& line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    return ops;
}

namespace {

void append_json_string(std::string& out, const std::string& utf8) {
    out += '"';
    for (const char c : utf8) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (u < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(u));
            out += escaped;
        } else {
            out += c;
        }
    }
    out += '"';
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    }
    return out;
}

// Just enough JSON for the backup file: one object of string keys to objects
// of integer fields. Anything else is an error (the file is then ignored).
class BackupParser {
public:
    explicit BackupParser(const std::string& text) : text_(text) {}

    bool parse(DisplayScaleBackup& backup, std::string& error) {
        DisplayScaleBackup parsed;
        if (!expect('{')) {
            return fail(error, "expected '{'");
        }
        if (!peek('}')) {
            do {
                std::string key;
                if (!read_string(key) || key.empty() || !expect(':') || !expect('{')) {
                    return fail(error, "expected \"monitor id\": {");
                }
                DisplayScaleSaved saved;
                bool has_rel = false;
                if (!peek('}')) {
                    do {
                        std::string field;
                        long long value = 0;
                        if (!read_string(field) || !expect(':') || !read_integer(value)) {
                            return fail(error, "expected \"field\": integer");
                        }
                        if (value < -1000 || value > 1000) {
                            return fail(error, "value out of range");
                        }
                        if (field == "rel") {
                            saved.rel = static_cast<int>(value);
                            has_rel = true;
                        } else if (field == "percent") {
                            saved.percent = static_cast<int>(value);
                        }
                    } while (expect(','));
                }
                if (!expect('}') || !has_rel) {
                    return fail(error, "an entry needs an integer \"rel\"");
                }
                parsed[widen(key)] = saved;
            } while (expect(','));
        }
        if (!expect('}')) {
            return fail(error, "expected '}'");
        }
        skip();
        if (pos_ != text_.size()) {
            return fail(error, "trailing data");
        }
        backup = std::move(parsed);
        return true;
    }

private:
    bool fail(std::string& error, const char* what) const {
        error = std::string("display scale backup: ") + what + " at offset " + std::to_string(pos_);
        return false;
    }
    void skip() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) {
            ++pos_;
        }
    }
    bool peek(char c) {
        skip();
        return pos_ < text_.size() && text_[pos_] == c;
    }
    bool expect(char c) {
        if (!peek(c)) {
            return false;
        }
        ++pos_;
        return true;
    }
    static int hex_value(char h) {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    }
    bool read_string(std::string& out) {
        if (!expect('"')) {
            return false;
        }
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                return false;
            }
            const char e = text_[pos_++];
            if (e == '"' || e == '\\' || e == '/') {
                out += e;
            } else if (e == 'b') {
                out += '\b';
            } else if (e == 'f') {
                out += '\f';
            } else if (e == 'n') {
                out += '\n';
            } else if (e == 'r') {
                out += '\r';
            } else if (e == 't') {
                out += '\t';
            } else if (e == 'u') {
                if (pos_ + 4 > text_.size()) {
                    return false;
                }
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    const int digit = hex_value(text_[pos_++]);
                    if (digit < 0) {
                        return false;
                    }
                    code = (code << 4) | static_cast<unsigned>(digit);
                }
                // BMP only (monitor ids are ASCII); surrogates are rejected.
                if (code >= 0xD800 && code <= 0xDFFF) {
                    return false;
                }
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
            } else {
                return false;
            }
        }
        return false;
    }
    bool read_integer(long long& value) {
        skip();
        const size_t begin = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
        }
        const size_t digits = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
        if (pos_ == digits || pos_ - digits > 6) {
            return false;
        }
        if (pos_ < text_.size() && (text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
            return false;
        }
        value = std::stoll(text_.substr(begin, pos_ - begin));
        return true;
    }

    const std::string& text_;
    size_t pos_ = 0;
};

std::string describe(const DisplayScaleSaved& saved) {
    return saved.percent > 0 ? std::to_string(saved.percent) + "%" : "rel " + std::to_string(saved.rel);
}

}  // namespace

std::string serialize_display_scale_backup(const DisplayScaleBackup& backup) {
    std::string out = "{";
    bool first = true;
    for (const auto& [id, saved] : backup) {
        out += first ? "\n  " : ",\n  ";
        first = false;
        append_json_string(out, narrow(id));
        out += ": {\"rel\": " + std::to_string(saved.rel) + ", \"percent\": " + std::to_string(saved.percent) + "}";
    }
    out += first ? "}\n" : "\n}\n";
    return out;
}

bool parse_display_scale_backup(const std::string& text, DisplayScaleBackup& backup, std::string& error) {
    return BackupParser(text).parse(backup, error);
}

DisplayScaleBackupStore file_display_scale_backup(const std::filesystem::path& path) {
    DisplayScaleBackupStore store;
    store.load = [path](DisplayScaleBackup& backup, std::string& error) {
        backup.clear();
        std::error_code exists_error;
        if (!std::filesystem::exists(path, exists_error)) {
            return true;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open " + narrow(path.wstring());
            return false;
        }
        std::ostringstream text;
        text << in.rdbuf();
        return parse_display_scale_backup(text.str(), backup, error);
    };
    store.save = [path](const DisplayScaleBackup& backup, std::string& error) {
        if (backup.empty()) {
            std::error_code remove_error;
            std::filesystem::remove(path, remove_error);
            if (remove_error) {
                error = "cannot delete " + narrow(path.wstring()) + ": " + remove_error.message();
                return false;
            }
            return true;
        }
        std::filesystem::path temp = path;
        temp += L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            const std::string text = serialize_display_scale_backup(backup);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out) {
                error = "cannot write " + narrow(temp.wstring());
                return false;
            }
        }
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "cannot replace " + narrow(path.wstring()) + " (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        return true;
    };
    return store;
}

DisplayScaleSession::DisplayScaleSession(int target_percent, DisplayScaleOps ops, DisplayScaleBackupStore store)
    : target_(target_percent), ops_(std::move(ops)), store_(std::move(store)) {
    if (store_.load) {
        std::string error;
        if (!store_.load(backup_, error)) {
            backup_.clear();
            log("backup unreadable, ignored: " + error);
        }
    }
    for (const auto& [id, saved] : backup_) {
        log("unclean exit: " + narrow(id) + " was left scaled; its original " + describe(saved) +
            " is restored when it next appears");
    }
}

DisplayScaleSession::~DisplayScaleSession() {
    try {
        restore_all();
    } catch (...) {
    }
}

void DisplayScaleSession::log(const std::string& line) const {
    if (ops_.log) {
        ops_.log("display scale: " + line);
    }
}

std::wstring DisplayScaleSession::live_name(const std::wstring& monitor_id, std::string& reason) const {
    if (monitor_id.empty()) {
        reason = "no monitor id";
        return {};
    }
    // Only ever write to an active virtual display (never the glasses or the
    // laptop panel), and only when exactly one carries the id. A detached
    // ghost twin is not an active virtual display, so it never makes a live
    // display ambiguous.
    const auto identities = ops_.identities();
    std::vector<std::wstring> matches;
    for (const auto& [driver_index, name] : ops_.virtual_displays()) {
        for (const auto& [gdi, id] : identities) {
            if (_wcsicmp(gdi.c_str(), name.c_str()) == 0) {
                if (_wcsicmp(id.c_str(), monitor_id.c_str()) == 0) {
                    matches.push_back(name);
                }
                break;
            }
        }
    }
    if (matches.empty()) {
        reason = "its monitor is no longer an active virtual display";
        return {};
    }
    if (matches.size() > 1) {
        reason = "ambiguous, " + std::to_string(matches.size()) + " active virtual displays share monitor id " +
                 narrow(monitor_id);
        return {};
    }
    return matches.front();
}

bool DisplayScaleSession::persist(const std::wstring& monitor_id, const DisplayScaleSaved& saved) {
    const auto it = backup_.find(monitor_id);
    if (it != backup_.end() && it->second == saved) {
        return true;
    }
    DisplayScaleBackup next = backup_;
    next[monitor_id] = saved;
    if (store_.save) {
        std::string error;
        if (!store_.save(next, error)) {
            log("could not save the original of " + narrow(monitor_id) + ": " + error);
            return false;
        }
    }
    backup_ = std::move(next);
    return true;
}

void DisplayScaleSession::forget(const std::wstring& monitor_id) {
    for (size_t i = 0; i < originals_.size(); ++i) {
        if (originals_[i].monitor_id == monitor_id) {
            originals_.erase(originals_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (backup_.erase(monitor_id) > 0 && store_.save) {
        std::string error;
        if (!store_.save(backup_, error)) {
            log("could not update the backup after a restore: " + error);
        }
    }
}

void DisplayScaleSession::apply(const std::vector<int>& driver_indices) {
    if (!active()) {
        return;
    }
    // Fresh names: GDI names renumber across display churn, so a cached name
    // could belong to another driver index by now.
    const auto live = ops_.virtual_displays();
    const auto identities = ops_.identities();
    for (const int driver_index : driver_indices) {
        const auto found = live.find(driver_index);
        if (found == live.end()) {
            log("VDD " + std::to_string(driver_index) + ": skipped, not an active virtual display");
            continue;
        }
        const std::wstring gdi_name = found->second;
        const std::string label = narrow(gdi_name) + " (VDD " + std::to_string(driver_index) + ")";
        std::wstring monitor_id;
        for (const auto& [gdi, id] : identities) {
            if (_wcsicmp(gdi.c_str(), gdi_name.c_str()) == 0) {
                monitor_id = id;
                break;
            }
        }
        // Without a unique stable id the original could not be restored safely.
        std::string reason;
        if (monitor_id.empty() || _wcsicmp(live_name(monitor_id, reason).c_str(), gdi_name.c_str()) != 0) {
            log(label + ": skipped, no unique stable monitor id for an active virtual display" +
                (reason.empty() ? std::string() : " (" + reason + ")"));
            continue;
        }
        size_t record = originals_.size();
        for (size_t i = 0; i < originals_.size(); ++i) {
            if (originals_[i].monitor_id == monitor_id) {
                originals_[i].driver_index = driver_index;
                record = i;
                break;
            }
        }
        const auto backed = backup_.find(monitor_id);
        if (!enabled()) {
            // Scaling is off now, but an unclean exit left this display scaled.
            if (record == originals_.size() && backed != backup_.end()) {
                log(label + ": scaling is off; restoring the original left by an unclean exit");
                const Original original{monitor_id, driver_index, backed->second.rel, backed->second.percent};
                originals_.push_back(original);
                if (restore_one(original)) {
                    forget(monitor_id);
                }
            }
            continue;
        }
        DisplayScaleInfo info;
        std::string error;
        if (!ops_.query(gdi_name, info, error)) {
            log(label + ": query failed: " + error);
            continue;
        }
        const DisplayScaleTarget target = plan_display_scale(info, target_);
        if (!target.valid) {
            log(label + ": skipped, unsupported scale range (min " + std::to_string(info.min_rel) + " cur " +
                std::to_string(info.cur_rel) + " max " + std::to_string(info.max_rel) + ")");
            continue;
        }
        const int current = display_scale_current_percent(info);
        const std::string current_text = current > 0 ? std::to_string(current) + "%" : "unknown (rel " +
                                                                                           std::to_string(info.cur_rel) + ")";
        if (record == originals_.size()) {
            DisplayScaleSaved saved{info.cur_rel, current};
            if (backed != backup_.end()) {
                // The engine was killed while this display was scaled: the
                // current value is ours, the backup holds the user's own.
                saved = backed->second;
                log(label + ": original " + describe(saved) + " taken from the backup of an unclean exit (now " +
                    current_text + ")");
            }
            originals_.push_back({monitor_id, driver_index, saved.rel, saved.percent});
        }
        const DisplayScaleSaved original{originals_[record].rel, originals_[record].percent};
        if (target.clamped) {
            log(label + ": clamped " + std::to_string(target_) + "% to " + std::to_string(target.percent) +
                "% (the most Windows allows on this display)");
        }
        if (info.cur_rel == target.rel) {
            log(label + ": already " + std::to_string(target.percent) + "%");
            continue;
        }
        // Saved before the first write, so a killed engine cannot lose it.
        if (!persist(monitor_id, original)) {
            log(label + ": not scaled, its original could not be saved");
            continue;
        }
        if (!ops_.set(gdi_name, target.rel, error)) {
            log(label + ": set " + std::to_string(target.percent) + "% failed: " + error);
            continue;
        }
        log(label + ": set " + current_text + " -> " + std::to_string(target.percent) + "% (recommended " +
            std::to_string(display_scale_recommended_percent(info)) + "%, max " +
            std::to_string(display_scale_max_percent(info)) + "%)");
    }
}

bool DisplayScaleSession::restore_one(const Original& original) {
    const std::string what = describe({original.rel, original.percent});
    std::string reason;
    const std::wstring name = live_name(original.monitor_id, reason);
    if (name.empty()) {
        log("restore of VDD " + std::to_string(original.driver_index) + " to " + what + " skipped: " + reason +
            " (kept for a later restore)");
        return false;
    }
    const std::string label = narrow(name) + " (VDD " + std::to_string(original.driver_index) + ")";
    std::string error;
    if (!ops_.set(name, original.rel, error)) {
        log(label + ": restore to " + what + " failed: " + error + " (kept for a later restore)");
        return false;
    }
    log(label + ": restored " + what);
    return true;
}

bool DisplayScaleSession::restore_driver(int driver_index) {
    for (const Original& original : originals_) {
        if (original.driver_index == driver_index) {
            const Original copy = original;
            if (!restore_one(copy)) {
                return false;
            }
            forget(copy.monitor_id);
            return true;
        }
    }
    return false;
}

void DisplayScaleSession::restore_all() {
    const std::vector<Original> pending = originals_;
    for (const Original& original : pending) {
        if (restore_one(original)) {
            forget(original.monitor_id);
        }
    }
}

}  // namespace gt
