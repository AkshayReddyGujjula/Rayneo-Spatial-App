#pragma once

#include "imu/fusion.h"
#include "imu/pose_estimator.h"

#include <atomic>
#include <array>
#include <cstdint>
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
    // Dwell-qualified rest (the estimator's stillness); `rest()` is the
    // instantaneous rest condition.
    bool still() const { return still_.load(std::memory_order_relaxed); }
    bool rest() const { return rest_.load(std::memory_order_relaxed); }
    // 0 = idle, 1 = routine, 2 = escape, 3 = calibrating (see BiasAdaptState).
    int adapt_state() const { return adapt_state_.load(std::memory_order_relaxed); }
    float corrected_rate_degs() const {
        return corrected_rate_degs_.load(std::memory_order_relaxed);
    }
    float stillness_degs() const { return stillness_degs_.load(std::memory_order_relaxed); }
    float accel_dev_mps2() const { return accel_dev_mps2_.load(std::memory_order_relaxed); }
    uint32_t escape_rollbacks() const {
        return escape_rollbacks_.load(std::memory_order_relaxed);
    }
    // True once the startup bias came from a contiguous rest window. Diagnostic
    // timeouts never open the pose with an unqualified estimate.
    bool calibrated() const { return calibrated_.load(std::memory_order_relaxed); }
    float drift_correction_degs() const;
    std::string status() const;

    void recenter() { recenter_request_.store(true, std::memory_order_relaxed); }
    void set_sensor_to_head(const std::array<float, 9>& matrix) { sensor_to_head_ = matrix; }
    // Call before start(). An invalid calibration leaves the heading lock off.
    void set_mag_calibration(const MagCalibration& calibration) { mag_calibration_ = calibration; }
    // Optional full-rate raw sample log in the gt_imu_probe CSV format, so a
    // real session can be replayed offline with imu_replay. Truncated at start.
    void set_raw_log_path(const std::string& path) { raw_log_path_ = path; }

    // Magnetometer heading lock diagnostics (see MagHeadingLock).
    bool mag_lock_active() const { return mag_active_.load(std::memory_order_relaxed); }
    int mag_state() const { return mag_state_.load(std::memory_order_relaxed); }
    float mag_error_deg() const { return mag_error_deg_.load(std::memory_order_relaxed); }
    float mag_field_ut() const { return mag_field_ut_.load(std::memory_order_relaxed); }
    float mag_dip_deg() const { return mag_dip_deg_.load(std::memory_order_relaxed); }
    float mag_reference_field_ut() const { return mag_ref_field_ut_.load(std::memory_order_relaxed); }
    float mag_reference_dip_deg() const { return mag_ref_dip_deg_.load(std::memory_order_relaxed); }
    float mag_integral_degs() const { return mag_integral_degs_.load(std::memory_order_relaxed); }
    float mag_total_correction_deg() const { return mag_total_deg_.load(std::memory_order_relaxed); }
    uint32_t mag_reacquisitions() const { return mag_reacq_.load(std::memory_order_relaxed); }
    float temperature_c() const { return temp_c_.load(std::memory_order_relaxed); }

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
    std::atomic<bool> rest_{false};
    std::atomic<int> adapt_state_{static_cast<int>(BiasAdaptState::calibrating)};
    std::atomic<float> corrected_rate_degs_{0.0f};
    std::atomic<float> stillness_degs_{0.0f};
    std::atomic<float> accel_dev_mps2_{0.0f};
    std::atomic<uint32_t> escape_rollbacks_{0};
    std::atomic<bool> calibrated_{false};
    std::atomic<float> drift_degs_{0.0f};
    std::atomic<bool> mag_active_{false};
    std::atomic<int> mag_state_{0};
    std::atomic<float> mag_error_deg_{0.0f};
    std::atomic<float> mag_field_ut_{0.0f};
    std::atomic<float> mag_dip_deg_{0.0f};
    std::atomic<float> mag_ref_field_ut_{0.0f};
    std::atomic<float> mag_ref_dip_deg_{0.0f};
    std::atomic<float> mag_integral_degs_{0.0f};
    std::atomic<float> mag_total_deg_{0.0f};
    std::atomic<uint32_t> mag_reacq_{0};
    std::atomic<float> temp_c_{0.0f};
    MagCalibration mag_calibration_;
    std::string raw_log_path_;
    mutable std::mutex status_mutex_;
    std::string status_;
    std::array<float, 9> sensor_to_head_{
        1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f,
        0.0f, 1.0f, 0.0f,
    };
};

}  // namespace gt
