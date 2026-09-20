#pragma once

#include <array>
#include <atomic>
#include <cstdint>
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

class VddClient {
public:
    VddClient() = default;
    ~VddClient();

    VddClient(const VddClient&) = delete;
    VddClient& operator=(const VddClient&) = delete;

    static VddDriverStatus driver_status();

    bool connect(size_t display_count, std::string& error);
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
    bool remove_display(int index);
    bool ping();
    bool query_version(int& version);
    bool ioctl(uint32_t code, const std::array<uint8_t, 32>& input, uint32_t timeout_ms,
               uint32_t* output, std::string* error);
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
