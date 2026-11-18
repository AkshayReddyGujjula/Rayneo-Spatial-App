#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gt {

enum class VddDriverStatus {
    Ready,
    NotInstalled,
    Disabled,
    RestartRequired,
    DriverError,
    Inaccessible,
    Unknown,
};

const char* vdd_driver_status_text(VddDriverStatus status);
std::array<uint8_t, 32> vdd_remove_payload(int driver_index);
std::vector<int> vdd_cleanup_order(const std::vector<int>& addition_order);
bool retry_vdd_removal(int index, const std::function<bool(int, std::string&)>& remove,
                       const std::function<void(int)>& sleep_ms, std::string& error);

class VddClient {
public:
    VddClient() = default;
    ~VddClient();

    VddClient(const VddClient&) = delete;
    VddClient& operator=(const VddClient&) = delete;

    static VddDriverStatus driver_status();

    bool connect(size_t display_count, std::string& error);
    // On failure the owned indices reflect completed operations. The caller
    // can restore its previous count or disconnect during startup cleanup.
    bool resize(size_t display_count, std::string& error);
    void disconnect();

    bool connected() const { return handle_ != nullptr; }
    int driver_version() const { return driver_version_; }
    const std::vector<int>& display_indices() const { return display_indices_; }
    uint64_t keepalive_failures() const { return keepalive_failures_.load(); }
    uint64_t consecutive_keepalive_failures() const {
        return consecutive_keepalive_failures_.load();
    }

private:
    bool open(std::string& error);
    void close();
    bool add_display(int& index, std::string& error);
    // Mid-session removals keep the short timeout (they hold the IOCTL lock,
    // so the keepalive ping waits): the retry loop covers a slow one. The
    // shutdown disconnect, where nothing must stay alive, waits longer.
    static constexpr uint32_t kRemoveTimeoutMs = 1000;
    static constexpr uint32_t kShutdownRemoveTimeoutMs = 5000;
    bool remove_display(int index, std::string& error, uint32_t timeout_ms = kRemoveTimeoutMs);
    bool ping();
    bool query_version(int& version);
    bool ioctl(uint32_t code, const std::array<uint8_t, 32>& input, uint32_t timeout_ms,
               uint32_t* output, std::string* error, bool require_output_size = true);
    void keepalive_loop();

    void* handle_ = nullptr;
    std::mutex ioctl_mutex_;
    std::thread keepalive_thread_;
    std::atomic<bool> keepalive_running_{false};
    std::atomic<uint64_t> keepalive_failures_{0};
    std::atomic<uint64_t> consecutive_keepalive_failures_{0};
    int driver_version_ = -1;
    std::vector<int> display_indices_;
};

}  // namespace gt
