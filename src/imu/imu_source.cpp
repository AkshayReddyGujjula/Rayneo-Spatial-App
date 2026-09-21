#include "imu/imu_source.h"

#include "imu/gt_hid.h"
#include "imu/gt_protocol.h"

#include <hidapi.h>

#include <chrono>

namespace gt {
namespace {

using SteadyClock = std::chrono::steady_clock;

void sleep_checked(const std::atomic<bool>& running, int milliseconds) {
    const int steps = milliseconds / 50;
    for (int i = 0; i < steps && running.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace

ImuSource::~ImuSource() {
    stop();
}

void ImuSource::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&ImuSource::run, this);
}

void ImuSource::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

Quat ImuSource::orientation() const {
    for (;;) {
        const uint64_t before = pose_sequence_.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) {
            std::this_thread::yield();
            continue;
        }
        const Quat pose{qw_.load(std::memory_order_relaxed), qx_.load(std::memory_order_relaxed),
                        qy_.load(std::memory_order_relaxed), qz_.load(std::memory_order_relaxed)};
        const uint64_t after = pose_sequence_.load(std::memory_order_acquire);
        if (before == after) {
            return pose;
        }
    }
}

Vec3 ImuSource::gyro_bias_degs() const {
    return Vec3{bx_.load(std::memory_order_relaxed), by_.load(std::memory_order_relaxed),
                bz_.load(std::memory_order_relaxed)};
}

Vec3 ImuSource::last_gyro_degs() const {
    return Vec3{gx_.load(std::memory_order_relaxed), gy_.load(std::memory_order_relaxed),
                gz_.load(std::memory_order_relaxed)};
}

float ImuSource::drift_correction_degs() const {
    return drift_degs_.load(std::memory_order_relaxed);
}

std::string ImuSource::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void ImuSource::set_status(const std::string& text) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = text;
}

void ImuSource::run() {
    if (hid_init() != 0) {
        set_status("hidapi initialization failed");
        return;
    }
    while (running_.load()) {
        GtHidDevice device;
        if (!device.open_first()) {
            set_status("waiting for RayNeo device");
            has_pose_.store(false);
            sleep_checked(running_, 2000);
            continue;
        }

        device.send_command_verified(kCmdStreamOff, 300, nullptr);
        uint8_t drain[64];
        while (device.read_report(drain, sizeof(drain), 10) > 0) {
        }
        std::string ack_error;
        if (!device.send_command_verified(kCmdStreamOn, 500, &ack_error)) {
            set_status("failed to start IMU stream (" + ack_error + "), retrying");
            has_pose_.store(false, std::memory_order_release);
            sleep_checked(running_, 500);
            continue;
        }

        set_status("streaming, calibrating");
        PoseEstimator estimator;
        PoseEstimator::Config estimator_config;
        estimator_config.sensor_to_head = sensor_to_head_;
        estimator.configure(estimator_config);
        has_pose_.store(false);
        sample_rate_hz_.store(0.0, std::memory_order_relaxed);

        auto rate_window_start = SteadyClock::now();
        int rate_window_samples = 0;
        std::string last_phase;
        auto last_imu_time = SteadyClock::now();

        while (running_.load()) {
            if (recenter_request_.exchange(false)) {
                estimator.recenter();
            }
            uint8_t buffer[64] = {};
            const int n = device.read_report(buffer, sizeof(buffer), 100);
            if (n < 0) {
                set_status("RayNeo disconnected, reconnecting");
                has_pose_.store(false, std::memory_order_release);
                break;
            }
            if (n == 0) {
                if (SteadyClock::now() - last_imu_time > std::chrono::seconds(2)) {
                    set_status("IMU stream timed out, reconnecting");
                    has_pose_.store(false, std::memory_order_release);
                    break;
                }
                continue;
            }
            const Report r = decode_report(buffer, static_cast<size_t>(n));
            if (r.kind != ReportKind::Imu) {
                continue;
            }
            last_imu_time = SteadyClock::now();

            // Stream-rate telemetry describes the device stream, including the
            // startup calibration phase. Counting only fused samples made a
            // healthy 476 Hz stream misleadingly print as 0 Hz until tracking
            // opened, which hid calibration-gate failures in live tests.
            ++rate_window_samples;
            const auto rate_now = SteadyClock::now();
            const double rate_elapsed =
                std::chrono::duration<double>(rate_now - rate_window_start).count();
            if (rate_elapsed >= 1.0) {
                sample_rate_hz_.store(rate_window_samples / rate_elapsed,
                                      std::memory_order_relaxed);
                rate_window_start = rate_now;
                rate_window_samples = 0;
            }

            const bool ready = estimator.add_sample(r.imu);
            gx_.store(r.imu.gyro_degs.x, std::memory_order_relaxed);
            gy_.store(r.imu.gyro_degs.y, std::memory_order_relaxed);
            gz_.store(r.imu.gyro_degs.z, std::memory_order_relaxed);
            last_tick_.store(r.imu.tick_100us, std::memory_order_relaxed);
            const Vec3 bias = estimator.gyro_bias_degs();
            bx_.store(bias.x, std::memory_order_relaxed);
            by_.store(bias.y, std::memory_order_relaxed);
            bz_.store(bias.z, std::memory_order_relaxed);
            still_.store(estimator.still(), std::memory_order_relaxed);
            rest_.store(estimator.rest(), std::memory_order_relaxed);
            adapt_state_.store(estimator.adapt_state(), std::memory_order_relaxed);
            corrected_rate_degs_.store(estimator.corrected_rate_degs(),
                                       std::memory_order_relaxed);
            stillness_degs_.store(estimator.stillness_degs(), std::memory_order_relaxed);
            accel_dev_mps2_.store(estimator.accel_dev_mps2(), std::memory_order_relaxed);
            escape_rollbacks_.store(estimator.escape_rollbacks(), std::memory_order_relaxed);
            calibrated_.store(estimator.calibrated(), std::memory_order_relaxed);
            drift_degs_.store(estimator.drift_correction_degs(), std::memory_order_relaxed);

            const char* phase = nullptr;
            if (!estimator.bias_done()) {
                phase = "hold still - calibrating";
            } else if (estimator.adapt_state() == static_cast<int>(BiasAdaptState::escape)) {
                phase = "streaming (bias escape)";
            } else if (estimator.still()) {
                phase = "streaming (still)";
            } else {
                phase = "streaming";
            }
            if (phase != last_phase) {
                last_phase = phase;
                set_status(phase);
            }

            if (ready) {
                const Quat q = estimator.quat();
                pose_sequence_.fetch_add(1, std::memory_order_acq_rel);
                qw_.store(q.w, std::memory_order_relaxed);
                qx_.store(q.x, std::memory_order_relaxed);
                qy_.store(q.y, std::memory_order_relaxed);
                qz_.store(q.z, std::memory_order_relaxed);
                pose_sequence_.fetch_add(1, std::memory_order_release);
                has_pose_.store(true, std::memory_order_release);
            }
        }

        device.send_command_verified(kCmdStreamOff, 300, nullptr);
    }
    hid_exit();
}

}  // namespace gt
