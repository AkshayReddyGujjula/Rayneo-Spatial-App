#include "imu/pose_estimator.h"

#include <cmath>
#include <cstdio>

using namespace gt;

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Mat3 {
    float m[3][3];
};

Mat3 rotation_about(float ax, float ay, float az, float angle_rad) {
    const float n = std::sqrt(ax * ax + ay * ay + az * az);
    ax /= n;
    ay /= n;
    az /= n;
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    const float t = 1.0f - c;
    Mat3 r{};
    r.m[0][0] = t * ax * ax + c;
    r.m[0][1] = t * ax * ay - s * az;
    r.m[0][2] = t * ax * az + s * ay;
    r.m[1][0] = t * ax * ay + s * az;
    r.m[1][1] = t * ay * ay + c;
    r.m[1][2] = t * ay * az - s * ax;
    r.m[2][0] = t * ax * az - s * ay;
    r.m[2][1] = t * ay * az + s * ax;
    r.m[2][2] = t * az * az + c;
    return r;
}

Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return r;
}

Vec3 body_to_earth(const Mat3& r, const Vec3& v) {
    return Vec3{r.m[0][0] * v.x + r.m[0][1] * v.y + r.m[0][2] * v.z,
                r.m[1][0] * v.x + r.m[1][1] * v.y + r.m[1][2] * v.z,
                r.m[2][0] * v.x + r.m[2][1] * v.y + r.m[2][2] * v.z};
}

Vec3 earth_to_body(const Mat3& r, const Vec3& v) {
    return Vec3{r.m[0][0] * v.x + r.m[1][0] * v.y + r.m[2][0] * v.z,
                r.m[0][1] * v.x + r.m[1][1] * v.y + r.m[2][1] * v.z,
                r.m[0][2] * v.x + r.m[1][2] * v.y + r.m[2][2] * v.z};
}

Vec3 package_from_body(const Vec3& body) {
    return Vec3{body.x, body.z, -body.y};
}

int g_failures = 0;

void check(bool condition, const char* label, float value, float limit) {
    const bool ok = condition;
    std::printf("  [%s] %s (value %.3f, limit %.3f)\n", ok ? "PASS" : "FAIL", label, value, limit);
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

int main() {
    std::printf("pose_selftest: tilted recenter + pure yaw must stay pure yaw\n");
    {
        PoseEstimator estimator;
        PoseEstimator::Config cfg;
        cfg.bias_samples = 1000;
        estimator.configure(cfg);

        const Mat3 initial_tilt = rotation_about(1.0f, 0.0f, 0.0f, 20.0f * kPi / 180.0f);
        Mat3 attitude = initial_tilt;
        uint32_t tick = 100000;

        auto feed = [&](const Vec3& omega_earth_degs, int samples) {
            for (int i = 0; i < samples; ++i) {
                const Mat3 spin = rotation_about(0.0f, 0.0f, 1.0f, 0.0f);
                (void)spin;
                const Vec3 accel_body = earth_to_body(attitude, Vec3{0.0f, 0.0f, 9.81f});
                const Vec3 gyro_body = earth_to_body(attitude, omega_earth_degs);
                ImuSample s;
                s.accel_mps2 = package_from_body(accel_body);
                s.gyro_degs = package_from_body(gyro_body);
                s.tick_100us = tick += 21;
                estimator.add_sample(s);

                const Mat3 step = rotation_about(0.0f, 0.0f, 1.0f, 0.0f);
                (void)step;
                if (omega_earth_degs.z != 0.0f || omega_earth_degs.y != 0.0f || omega_earth_degs.x != 0.0f) {
                    // integrate the world-frame rotation: R <- Rz(wz dt) Ry(wy dt) Rx(wx dt) R
                    const float dt = 21.0f * 1e-4f;
                    const Mat3 wx = rotation_about(1.0f, 0.0f, 0.0f, omega_earth_degs.x * kPi / 180.0f * dt);
                    const Mat3 wy = rotation_about(0.0f, 1.0f, 0.0f, omega_earth_degs.y * kPi / 180.0f * dt);
                    const Mat3 wz = rotation_about(0.0f, 0.0f, 1.0f, omega_earth_degs.z * kPi / 180.0f * dt);
                    attitude = multiply(wz, multiply(wy, multiply(wx, attitude)));
                }
            }
        };

        feed(Vec3{0.0f, 0.0f, 0.0f}, 2000);
        std::printf("  after still calibration: yaw=%.2f pitch=%.2f roll=%.2f\n", estimator.euler().yaw_deg,
                    estimator.euler().pitch_deg, estimator.euler().roll_deg);

        feed(Vec3{0.0f, 0.0f, -90.0f}, 476);
        const Euler e = estimator.euler();
        std::printf("  after pure right yaw 90 deg: yaw=%.2f pitch=%.2f roll=%.2f\n", e.yaw_deg, e.pitch_deg,
                    e.roll_deg);
        check(std::fabs(e.yaw_deg + 90.0f) < 3.0f, "yaw magnitude ~90 deg", e.yaw_deg, 90.0f);
        check(std::fabs(e.pitch_deg) < 3.0f, "pitch leakage small", e.pitch_deg, 3.0f);
        check(std::fabs(e.roll_deg) < 3.0f, "roll leakage small", e.roll_deg, 3.0f);
    }

    std::printf("pose_selftest: contaminated calibration must self-correct while still\n");
    {
        PoseEstimator estimator;
        estimator.configure(PoseEstimator::Config{});

        const Vec3 true_bias_degs{0.3f, -0.2f, 0.5f};
        Mat3 attitude = rotation_about(0.0f, 0.0f, 1.0f, 0.0f);
        uint32_t tick = 500000;

        auto feed = [&](const Vec3& omega_earth_degs, int samples, bool contaminate) {
            for (int i = 0; i < samples; ++i) {
                const Vec3 accel_body = earth_to_body(attitude, Vec3{0.0f, 0.0f, 9.81f});
                const Vec3 gyro_body = earth_to_body(attitude, omega_earth_degs);
                ImuSample s;
                s.accel_mps2 = package_from_body(accel_body);
                const Vec3 pkg = package_from_body(gyro_body);
                s.gyro_degs = Vec3{pkg.x + true_bias_degs.x, pkg.y + true_bias_degs.y, pkg.z + true_bias_degs.z};
                s.tick_100us = tick += 21;
                estimator.add_sample(s);

                const float dt = 21.0f * 1e-4f;
                const Mat3 wy = rotation_about(0.0f, 1.0f, 0.0f, omega_earth_degs.y * kPi / 180.0f * dt);
                const Mat3 wz = rotation_about(0.0f, 0.0f, 1.0f, omega_earth_degs.z * kPi / 180.0f * dt);
                attitude = multiply(wz, multiply(wy, attitude));
                (void)contaminate;
            }
        };

        feed(Vec3{0.0f, 0.0f, 60.0f}, 476, true);
        feed(Vec3{0.0f, 0.0f, 0.0f}, 8000, false);

        const Vec3 bias = estimator.gyro_bias_degs();
        std::printf("  bias estimate (%.3f, %.3f, %.3f), true (%.3f, %.3f, %.3f)\n", bias.x, bias.y, bias.z,
                    true_bias_degs.x, true_bias_degs.y, true_bias_degs.z);
        const float err = std::sqrt((bias.x - true_bias_degs.x) * (bias.x - true_bias_degs.x) +
                                    (bias.y - true_bias_degs.y) * (bias.y - true_bias_degs.y) +
                                    (bias.z - true_bias_degs.z) * (bias.z - true_bias_degs.z));
        check(err < 0.08f, "bias converges after contaminated calibration", err, 0.08f);

        const Euler e1 = estimator.euler();
        feed(Vec3{0.0f, 0.0f, 0.0f}, 2380, false);
        const Euler e2 = estimator.euler();
        const float drift_deg_per_s =
            std::fabs(e2.yaw_deg - e1.yaw_deg) / (2380.0f * 21e-4f);
        std::printf("  residual yaw rate while still: %.4f deg/s (%.2f deg/min)\n", drift_deg_per_s,
                    drift_deg_per_s * 60.0f);
        check(drift_deg_per_s < 0.01f, "residual yaw rate small", drift_deg_per_s, 0.01f);
    }

    std::printf("pose_selftest: reconfigure must reset all estimator state\n");
    {
        PoseEstimator estimator;
        PoseEstimator::Config cfg;
        cfg.settle_samples = 0;
        cfg.bias_samples = 1;
        cfg.bias_timeout_samples = 10;
        cfg.still_hold_s = 0.0f;
        estimator.configure(cfg);
        ImuSample sample;
        sample.accel_mps2 = package_from_body(Vec3{0.0f, 0.0f, 9.81f});
        sample.gyro_degs = Vec3{0.2f, -0.1f, 0.3f};
        sample.tick_100us = 1000;
        estimator.add_sample(sample);
        sample.tick_100us += 21;
        estimator.add_sample(sample);
        check(estimator.bias_done(), "test setup reaches calibrated state", estimator.bias_done() ? 1.0f : 0.0f,
              1.0f);
        estimator.configure(PoseEstimator::Config{});
        const Vec3 reset_bias = estimator.gyro_bias_degs();
        check(!estimator.bias_done() && estimator.samples_fused() == 0,
              "configure clears calibration and fused count", static_cast<float>(estimator.samples_fused()),
              0.0f);
        check(std::fabs(reset_bias.x) < 1e-6f && std::fabs(reset_bias.y) < 1e-6f &&
                  std::fabs(reset_bias.z) < 1e-6f,
              "configure clears learned bias", reset_bias.x, 1e-6f);
    }

    std::printf("pose_selftest: swing/twist decomposition used by the tracking toggles\n");
    {
        const float half = 30.0f * kPi / 360.0f;
        const Quat yaw_only{std::cos(half), 0.0f, 0.0f, std::sin(half)};
        const Quat yaw_twist = quat_twist_about(yaw_only, 0.0f, 0.0f, 1.0f);
        const float yaw_error =
            std::fabs(yaw_twist.w - yaw_only.w) + std::fabs(yaw_twist.z - yaw_only.z);
        check(yaw_error < 1e-5f, "twist about the vertical captures a pure yaw", yaw_error, 1e-5f);

        const Quat nod_only{std::cos(half), 0.0f, std::sin(half), 0.0f};
        const Quat nod_twist = quat_twist_about(nod_only, 0.0f, 0.0f, 1.0f);
        const float nod_error = std::fabs(nod_twist.w - 1.0f) + std::fabs(nod_twist.x) +
                                std::fabs(nod_twist.y) + std::fabs(nod_twist.z);
        check(nod_error < 1e-5f, "twist about the vertical ignores a pure nod", nod_error, 1e-5f);

        const Quat ear_twist = quat_twist_about(nod_only, 0.0f, 1.0f, 0.0f);
        const float ear_error =
            std::fabs(ear_twist.w - nod_only.w) + std::fabs(ear_twist.y - nod_only.y);
        check(ear_error < 1e-5f, "twist about the ear axis captures a pure nod", ear_error, 1e-5f);
    }

    std::printf("pose_selftest: drift absorption keeps micro-movements\n");
    {
        PoseEstimator estimator;
        estimator.configure(PoseEstimator::Config{});

        Mat3 attitude = rotation_about(0.0f, 0.0f, 1.0f, 0.0f);
        uint32_t tick = 900000;
        auto feed = [&](float rate_degs, int samples) {
            for (int i = 0; i < samples; ++i) {
                const Vec3 accel_body = earth_to_body(attitude, Vec3{0.0f, 0.0f, 9.81f});
                const Vec3 gyro_body = earth_to_body(attitude, Vec3{0.0f, 0.0f, rate_degs});
                ImuSample s;
                s.accel_mps2 = package_from_body(accel_body);
                s.gyro_degs = package_from_body(gyro_body);
                s.tick_100us = tick += 21;
                estimator.add_sample(s);
                const float dt = 21.0f * 1e-4f;
                attitude = multiply(
                    rotation_about(0.0f, 0.0f, 1.0f, rate_degs * kPi / 180.0f * dt), attitude);
            }
        };

        feed(0.0f, 3000);
        const float baseline = estimator.euler().yaw_deg;
        feed(0.0f, 4760);
        const float creep = std::fabs(estimator.euler().yaw_deg - baseline);
        check(creep < 0.3f, "no creep while still", creep, 0.3f);

        // A small head movement: a 12 deg/s half-second turn with smooth ramps.
        for (int i = 0; i < 286; ++i) {
            const float t = static_cast<float>(i) / 476.0f;
            const float rate = 12.0f * std::sin(kPi * t / 0.6f);
            feed(rate, 1);
        }
        const float moved = std::fabs(estimator.euler().yaw_deg - baseline);
        std::printf("  registered movement: %.2f deg, drift correction: %.2f deg\n", moved,
                    estimator.drift_correction_degs());
        check(moved > 2.0f, "the movement registers", moved, 2.0f);

        feed(0.0f, 4760);
        const float held = std::fabs(estimator.euler().yaw_deg - baseline);
        const float pull_back = std::fabs(held - moved);
        check(pull_back < 0.4f, "movement is held afterwards", pull_back, 0.4f);
    }

    std::printf("pose_selftest: %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
