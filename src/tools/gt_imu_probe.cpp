#include "imu/gt_hid.h"
#include "imu/gt_protocol.h"
#include "imu/pose_estimator.h"

#include <hidapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using SteadyClock = std::chrono::steady_clock;

uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now().time_since_epoch())
            .count());
}

struct Options {
    std::string log_path;
    double seconds = 10.0;
    bool list_only = false;
    bool keep_stream = false;
    bool pose = false;
    bool mag = false;
    int settle_samples = 150;
    int bias_samples = 1500;
    float beta = 0.05f;
};

void print_usage() {
    std::printf(
        "gt_imu_probe - RayNeo GT HID IMU probe\n"
        "  --list          enumerate matching HID devices and exit\n"
        "  --seconds N     stream duration in seconds (default 10)\n"
        "  --log FILE      write samples as CSV\n"
        "  --pose          stream through the Madgwick pose estimator\n"
        "  --mag           enable the magnetometer correction (default off)\n"
        "  --settle N      samples skipped before bias calibration (default 150)\n"
        "  --bias-samples N  samples averaged for the gyro bias (default 1000)\n"
        "  --beta F        Madgwick gain (default 0.05)\n"
        "  --keep-stream   do not stop an already running stream first\n");
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--list") == 0) {
            opt.list_only = true;
        } else if (std::strcmp(a, "--keep-stream") == 0) {
            opt.keep_stream = true;
        } else if (std::strcmp(a, "--pose") == 0) {
            opt.pose = true;
        } else if (std::strcmp(a, "--mag") == 0) {
            opt.mag = true;
        } else if (std::strcmp(a, "--settle") == 0 && i + 1 < argc) {
            opt.settle_samples = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--bias-samples") == 0 && i + 1 < argc) {
            opt.bias_samples = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--beta") == 0 && i + 1 < argc) {
            opt.beta = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--seconds") == 0 && i + 1 < argc) {
            opt.seconds = std::atof(argv[++i]);
        } else if (std::strcmp(a, "--log") == 0 && i + 1 < argc) {
            opt.log_path = argv[++i];
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            print_usage();
            return false;
        } else {
            std::printf("unknown option: %s\n", a);
            print_usage();
            return false;
        }
    }
    return true;
}

void send_command(gt::GtHidDevice& dev, uint8_t cmd, const char* name) {
    uint8_t frame[64];
    gt::build_command(cmd, frame);
    const bool ok = dev.write_report(frame, sizeof(frame));
    std::printf("  send 66 %02x (%s): %s\n", cmd, name, ok ? "ok" : "FAILED");
}

int drain_replies(gt::GtHidDevice& dev, double seconds, const char* tag, int max_print) {
    const auto end = SteadyClock::now() + std::chrono::duration<double>(seconds);
    int printed = 0;
    while (SteadyClock::now() < end) {
        uint8_t buf[64] = {};
        const int n = dev.read_report(buf, sizeof(buf), 50);
        if (n <= 0) {
            continue;
        }
        const gt::Report r = gt::decode_report(buf, static_cast<size_t>(n));
        if (r.kind == gt::ReportKind::Imu || printed >= max_print) {
            continue;
        }
        ++printed;
        std::printf("  [%s] %dB  %s\n", tag, n, gt::to_hex(r.raw, r.length).c_str());
    }
    return printed;
}

double wrap180(double deg) {
    while (deg > 180.0) {
        deg -= 360.0;
    }
    while (deg < -180.0) {
        deg += 360.0;
    }
    return deg;
}

int run_pose_loop(gt::GtHidDevice& dev, const Options& opt, std::ofstream& csv) {
    gt::PoseEstimator::Config cfg;
    cfg.beta = opt.beta;
    cfg.mag_weight = opt.mag ? 0.5f : 0.0f;
    cfg.settle_samples = opt.settle_samples;
    cfg.bias_samples = opt.bias_samples;
    gt::PoseEstimator estimator;
    estimator.configure(cfg);

    std::printf("  pose mode: settle %d samples, bias over %d still samples, mag %s, beta %.3f\n",
                cfg.settle_samples, cfg.bias_samples, opt.mag ? "on" : "off", cfg.beta);

    double elapsed = 0.0;
    double next_print = 0.0;
    bool announced = false;
    bool have_yaw = false;
    double last_raw_yaw = 0.0;
    double yaw_unwrapped = 0.0;
    double yaw_first_sum = 0.0;
    double yaw_first_n = 0.0;
    double yaw_last_sum = 0.0;
    double yaw_last_n = 0.0;
    double yaw_min = 1e9;
    double yaw_max = -1e9;
    double pitch_min = 1e9;
    double pitch_max = -1e9;
    double roll_min = 1e9;
    double roll_max = -1e9;
    uint64_t samples = 0;
    uint64_t foreign = 0;
    char line[256];

    const auto start = SteadyClock::now();
    const auto end = start + std::chrono::duration<double>(opt.seconds);

    while (SteadyClock::now() < end) {
        uint8_t buf[64] = {};
        const int n = dev.read_report(buf, sizeof(buf), 100);
        if (n <= 0) {
            continue;
        }
        gt::Report r = gt::decode_report(buf, static_cast<size_t>(n));
        if (r.kind != gt::ReportKind::Imu) {
            ++foreign;
            continue;
        }
        r.imu.host_time_us = now_us();
        const bool ready = estimator.add_sample(r.imu);
        if (!ready) {
            if (!announced) {
                std::printf("  settling and calibrating...\n");
                announced = true;
            }
            continue;
        }

        const gt::Euler e = estimator.euler();
        if (!have_yaw) {
            yaw_unwrapped = e.yaw_deg;
            have_yaw = true;
        } else {
            yaw_unwrapped += wrap180(e.yaw_deg - last_raw_yaw);
        }
        last_raw_yaw = e.yaw_deg;

        elapsed = std::chrono::duration<double>(SteadyClock::now() - start).count();
        if (elapsed < 2.0) {
            yaw_first_sum += yaw_unwrapped;
            yaw_first_n += 1.0;
        }
        if (elapsed > opt.seconds - 2.0) {
            yaw_last_sum += yaw_unwrapped;
            yaw_last_n += 1.0;
        }
        if (yaw_unwrapped < yaw_min) yaw_min = yaw_unwrapped;
        if (yaw_unwrapped > yaw_max) yaw_max = yaw_unwrapped;
        if (e.pitch_deg < pitch_min) pitch_min = e.pitch_deg;
        if (e.pitch_deg > pitch_max) pitch_max = e.pitch_deg;
        if (e.roll_deg < roll_min) roll_min = e.roll_deg;
        if (e.roll_deg > roll_max) roll_max = e.roll_deg;

        if (elapsed >= next_print) {
            std::printf("  t=%5.1fs  yaw=%8.2f  pitch=%8.2f  roll=%8.2f  (rel start)\n", elapsed,
                        e.yaw_deg, e.pitch_deg, e.roll_deg);
            next_print = elapsed + 0.5;
        }

        if (csv) {
            std::snprintf(line, sizeof(line), "%llu,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f\n",
                          static_cast<unsigned long long>(r.imu.host_time_us), r.imu.tick_100us,
                          r.imu.accel_mps2.x, r.imu.accel_mps2.y, r.imu.accel_mps2.z, r.imu.gyro_degs.x,
                          r.imu.gyro_degs.y, r.imu.gyro_degs.z, r.imu.mag_ut.x, r.imu.mag_ut.y,
                          r.imu.mag_ut.z, r.imu.temp_c);
            csv << line;
        }
        ++samples;
    }

    const gt::Vec3 bias = estimator.gyro_bias_degs();
    std::printf("pose summary:\n");
    std::printf("  gyro bias (deg/s): %.3f %.3f %.3f\n", bias.x, bias.y, bias.z);
    std::printf("  fused samples: %llu (of %llu, %llu non-IMU)\n",
                static_cast<unsigned long long>(estimator.samples_fused()),
                static_cast<unsigned long long>(samples + estimator.samples_fused()),
                static_cast<unsigned long long>(foreign));
    if (yaw_first_n > 0.0 && yaw_last_n > 0.0) {
        const double first = yaw_first_sum / yaw_first_n;
        const double last = yaw_last_sum / yaw_last_n;
        const double rate = (last - first) / ((opt.seconds - 2.0) / 60.0);
        std::printf("  yaw drift: first2s %.2f deg -> last2s %.2f deg  =>  %.2f deg/min\n", first, last,
                    rate);
    }
    std::printf("  yaw   range: %.2f .. %.2f deg\n", yaw_min, yaw_max);
    std::printf("  pitch range: %.2f .. %.2f deg\n", pitch_min, pitch_max);
    std::printf("  roll  range: %.2f .. %.2f deg\n", roll_min, roll_max);
    return samples > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        return 2;
    }

    hid_init();

    const auto devices = gt::GtHidDevice::enumerate();
    std::printf("found %zu RayNeo HID device(s)\n", devices.size());
    for (const auto& d : devices) {
        std::printf("  %s\n", gt::GtHidDevice::describe(d).c_str());
    }
    if (devices.empty()) {
        hid_exit();
        return 1;
    }
    if (opt.list_only) {
        hid_exit();
        return 0;
    }

    gt::GtHidDevice dev;
    if (!dev.open(devices.front().path)) {
        std::printf("open failed (hidapi error %d)\n", hid_error(nullptr) != nullptr ? 1 : 0);
        hid_exit();
        return 1;
    }
    std::printf("opened device\n");

    if (!opt.keep_stream) {
        send_command(dev, gt::kCmdStreamOff, "stream off");
        drain_replies(dev, 0.5, "after stream-off", 4);
    }

    send_command(dev, gt::kCmdDeviceInfo, "device info");
    drain_replies(dev, 1.0, "device info", 4);

    send_command(dev, gt::kCmdCalibration, "factory calibration");
    drain_replies(dev, 1.0, "calibration", 8);

    send_command(dev, gt::kCmdStreamOn, "stream on");
    drain_replies(dev, 0.3, "stream on", 4);

    std::ofstream csv;
    if (!opt.log_path.empty()) {
        csv.open(opt.log_path, std::ios::binary);
        csv << "host_us,tick_100us,ax,ay,az,gx,gy,gz,mx,my,mz,temp_c\n";
    }

    if (opt.pose) {
        const int rc = run_pose_loop(dev, opt, csv);
        send_command(dev, gt::kCmdStreamOff, "stream off");
        dev.close();
        hid_exit();
        return rc;
    }

    gt::ImuSample first;
    gt::ImuSample last;
    uint64_t samples = 0;
    uint64_t foreign = 0;
    const auto start = SteadyClock::now();
    const auto end = start + std::chrono::duration<double>(opt.seconds);
    char line[256];

    while (SteadyClock::now() < end) {
        uint8_t buf[64] = {};
        const int n = dev.read_report(buf, sizeof(buf), 100);
        if (n <= 0) {
            continue;
        }
        gt::Report r = gt::decode_report(buf, static_cast<size_t>(n));
        if (r.kind != gt::ReportKind::Imu) {
            ++foreign;
            if (foreign <= 5) {
                std::printf("  [stream] %dB  %s\n", n, gt::to_hex(r.raw, r.length).c_str());
            }
            continue;
        }
        r.imu.host_time_us = now_us();
        if (samples == 0) {
            first = r.imu;
        }
        last = r.imu;
        if (samples < 5 || samples % 1000 == 0) {
            std::printf(
                "  sample %6llu: acc=(%8.3f,%8.3f,%8.3f) gyr=(%8.2f,%8.2f,%8.2f) mag=(%8.1f,%8.1f,%8.1f) T=%.1f tick=%u\n",
                static_cast<unsigned long long>(samples), r.imu.accel_mps2.x, r.imu.accel_mps2.y,
                r.imu.accel_mps2.z, r.imu.gyro_degs.x, r.imu.gyro_degs.y, r.imu.gyro_degs.z,
                r.imu.mag_ut.x, r.imu.mag_ut.y, r.imu.mag_ut.z, r.imu.temp_c, r.imu.tick_100us);
        }
        if (csv) {
            std::snprintf(line, sizeof(line), "%llu,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%.2f\n",
                          static_cast<unsigned long long>(r.imu.host_time_us), r.imu.tick_100us,
                          r.imu.accel_mps2.x, r.imu.accel_mps2.y, r.imu.accel_mps2.z, r.imu.gyro_degs.x,
                          r.imu.gyro_degs.y, r.imu.gyro_degs.z, r.imu.mag_ut.x, r.imu.mag_ut.y,
                          r.imu.mag_ut.z, r.imu.temp_c);
            csv << line;
        }
        ++samples;
    }

    const double elapsed = std::chrono::duration<double>(SteadyClock::now() - start).count();
    send_command(dev, gt::kCmdStreamOff, "stream off");
    dev.close();
    hid_exit();

    std::printf("summary: %llu samples in %.2fs = %.1f Hz host-measured, %llu non-IMU reports\n",
                static_cast<unsigned long long>(samples), elapsed,
                elapsed > 0.0 ? static_cast<double>(samples) / elapsed : 0.0,
                static_cast<unsigned long long>(foreign));
    if (samples > 1) {
        const double device_seconds =
            static_cast<double>(static_cast<uint32_t>(last.tick_100us - first.tick_100us)) * 100e-6;
        std::printf("device tick: %u -> %u (%.3fs) => %.1f Hz device-measured\n", first.tick_100us,
                    last.tick_100us, device_seconds, device_seconds > 0.0 ? (samples - 1) / device_seconds : 0.0);
    }
    return samples > 0 ? 0 : 1;
}
