#include "imu/imu_source.h"

#include "imu/gt_hid.h"
#include "imu/gt_protocol.h"

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
    return Quat{qw_.load(std::memory_order_relaxed), qx_.load(std::memory_order_relaxed),
                qy_.load(std::memory_order_relaxed), qz_.load(std::memory_order_relaxed)};
}

Vec3 ImuSource::gyro_bias_degs() const {
    return Vec3{bx_.load(std::memory_order_relaxed), by_.load(std::memory_order_relaxed),
                bz_.load(std::memory_order_relaxed)};
}

Vec3 ImuSource::last_gyro_degs() const {
    return Vec3{gx_.load(std::memory_order_relaxed), gy_.load(std::memory_order_relaxed),
                gz_.load(std::memory_order_relaxed)};
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
    while (running_.load()) {
        GtHidDevice device;
        if (!device.open_first()) {
            set_status("waiting for RayNeo device");
            has_pose_.store(false);
            sleep_checked(running_, 2000);
            continue;
        }

        uint8_t frame[64];
        build_command(kCmdStreamOff, frame);
        device.write_report(frame, sizeof(frame));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        uint8_t drain[64];
        while (device.read_report(drain, sizeof(drain), 10) > 0) {
        }
        build_command(kCmdStreamOn, frame);
        device.write_report(frame, sizeof(frame));

        set_status("streaming, calibrating");
        PoseEstimator estimator;
        PoseEstimator::Config estimator_config;
        estimator_config.freeze_when_still = freeze_when_still_;
        estimator.configure(estimator_config);
        has_pose_.store(false);

        auto rate_window_start = SteadyClock::now();
        int rate_window_samples = 0;
        std::string last_phase;

        while (running_.load()) {
            if (recenter_request_.exchange(false)) {
                estimator.recenter();
            }
            uint8_t buffer[64] = {};
            const int n = device.read_report(buffer, sizeof(buffer), 100);
            if (n <= 0) {
                continue;
            }
            const Report r = decode_report(buffer, static_cast<size_t>(n));
            if (r.kind != ReportKind::Imu) {
                continue;
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

            const char* phase = !estimator.bias_done() ? "hold still - calibrating"
                                                       : (estimator.still() ? "streaming (still)" : "streaming");
            if (phase != last_phase) {
                last_phase = phase;
                set_status(phase);
            }

            if (ready) {
                const Quat q = estimator.quat();
                qw_.store(q.w, std::memory_order_relaxed);
                qx_.store(q.x, std::memory_order_relaxed);
                qy_.store(q.y, std::memory_order_relaxed);
                qz_.store(q.z, std::memory_order_relaxed);
                has_pose_.store(true, std::memory_order_relaxed);
                ++rate_window_samples;

                const auto now = SteadyClock::now();
                const double elapsed = std::chrono::duration<double>(now - rate_window_start).count();
                if (elapsed >= 1.0) {
                    sample_rate_hz_.store(rate_window_samples / elapsed, std::memory_order_relaxed);
                    rate_window_start = now;
                    rate_window_samples = 0;
                }
            }
        }

        build_command(kCmdStreamOff, frame);
        device.write_report(frame, sizeof(frame));
    }
}

}  // namespace gt
