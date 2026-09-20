#pragma once

#include "imu/fusion.h"
#include "imu/pose_estimator.h"

#include <atomic>
#include <array>
#include <mutex>
#include <string>
#include <thread>

namespace gt {

class ImuSource {
public:
    ImuSource() = default;
    ~ImuSource();

    ImuSource(const ImuSource&) = delete;
    ImuSource& operator=(const ImuSource&) = delete;

    void start();
    void stop();

    bool has_pose() const { return has_pose_.load(std::memory_order_relaxed); }
    Quat orientation() const;
    double sample_rate_hz() const { return sample_rate_hz_.load(std::memory_order_relaxed); }
    Vec3 gyro_bias_degs() const;
    Vec3 last_gyro_degs() const;
    uint32_t last_tick() const { return last_tick_.load(std::memory_order_relaxed); }
    bool still() const { return still_.load(std::memory_order_relaxed); }
    float drift_correction_degs() const;
    std::string status() const;

    void recenter() { recenter_request_.store(true, std::memory_order_relaxed); }
    void set_freeze_when_still(bool enabled) { freeze_when_still_ = enabled; }
    void set_sensor_to_head(const std::array<float, 9>& matrix) { sensor_to_head_ = matrix; }

private:
    void run();
    void set_status(const std::string& text);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> recenter_request_{false};
    std::atomic<bool> has_pose_{false};
    std::atomic<float> qw_{1.0f};
    std::atomic<float> qx_{0.0f};
    std::atomic<float> qy_{0.0f};
    std::atomic<float> qz_{0.0f};
    std::atomic<uint64_t> pose_sequence_{0};
    std::atomic<double> sample_rate_hz_{0.0};
    std::atomic<float> bx_{0.0f};
    std::atomic<float> by_{0.0f};
    std::atomic<float> bz_{0.0f};
    std::atomic<float> gx_{0.0f};
    std::atomic<float> gy_{0.0f};
    std::atomic<float> gz_{0.0f};
    std::atomic<uint32_t> last_tick_{0};
    std::atomic<bool> still_{false};
    std::atomic<float> drift_degs_{0.0f};
    mutable std::mutex status_mutex_;
    std::string status_;
    bool freeze_when_still_ = false;
    std::array<float, 9> sensor_to_head_{
        1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f,
        0.0f, 1.0f, 0.0f,
    };
};

}  // namespace gt
