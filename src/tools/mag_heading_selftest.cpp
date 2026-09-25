// Magnetometer heading lock and hard-iron fit: synthetic closed-loop checks.
#include "imu/mag_heading.h"
#include "imu/pose_estimator.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace gt;

namespace {

constexpr float kPi = 3.14159265358979323846f;
int g_failures = 0;

void check(bool ok, const char* label, float value, float limit) {
    std::printf("  [%s] %s (value %.3f, limit %.3f)\n", ok ? "PASS" : "FAIL", label, value, limit);
    if (!ok) {
        ++g_failures;
    }
}

// UK-like field: 51.5 uT, 66.7 deg dip, horizontal along +X of the earth frame.
Vec3 earth_field(float heading_rad, float magnitude = 51.5f, float dip_deg = 66.7f) {
    const float dip = dip_deg * kPi / 180.0f;
    const float h = magnitude * std::cos(dip);
    return Vec3{h * std::cos(heading_rad), h * std::sin(heading_rad), -magnitude * std::sin(dip)};
}

struct LoopResult {
    float max_abs_error_deg = 0.0f;
    float final_error_deg = 0.0f;
    float max_step_deg = 0.0f;
};

// Closed loop: the estimate's yaw error grows at `drift_degs` (a stale gyro
// bias) and the lock's correction is applied back to it, exactly as the
// estimator premultiplies it onto the orientation.
LoopResult run_loop(MagHeadingLock& lock, float& yaw_error_deg, float drift_degs, float seconds,
                    float field_scale = 1.0f, float dip_offset = 0.0f, float true_heading_deg = 0.0f) {
    LoopResult r;
    const float dt = 1.0f / 476.0f;
    const int steps = static_cast<int>(seconds / dt);
    for (int i = 0; i < steps; ++i) {
        yaw_error_deg += drift_degs * dt;
        const bool fresh = (i % 9) == 0;  // ~51 Hz magnetometer
        // The estimate sees the true field rotated by its own yaw error.
        const Vec3 b = earth_field((true_heading_deg + yaw_error_deg) * kPi / 180.0f, 51.5f * field_scale,
                                   66.7f + dip_offset);
        const float step_rad = lock.update(b, fresh, 5.0f, dt);
        const float step_deg = step_rad * 180.0f / kPi;
        yaw_error_deg += step_deg;
        r.max_step_deg = std::max(r.max_step_deg, std::fabs(step_deg));
        r.max_abs_error_deg = std::max(r.max_abs_error_deg, std::fabs(yaw_error_deg));
    }
    r.final_error_deg = yaw_error_deg;
    return r;
}

}  // namespace

int main() {
    std::printf("mag_heading_selftest: a stale gyro bias cannot accumulate yaw\n");
    {
        MagHeadingLock lock;
        lock.configure(MagHeadingLock::Config{});
        float yaw_error = 0.0f;
        run_loop(lock, yaw_error, 0.0f, 3.0f);  // acquire
        check(lock.state() == MagHeadingLock::State::locked, "locks after acquisition",
              static_cast<float>(lock.state()), 2.0f);
        // 0.1 deg/s residual yaw bias for 10 minutes = 60 deg open loop.
        const LoopResult r = run_loop(lock, yaw_error, 0.1f, 600.0f);
        std::printf("  0.1 deg/s for 600 s: max |yaw error| %.3f deg, final %.3f deg (open loop 60 deg)\n",
                    r.max_abs_error_deg, r.final_error_deg);
        check(r.max_abs_error_deg < 2.5f, "error stays bounded", r.max_abs_error_deg, 2.5f);
        check(std::fabs(r.final_error_deg) < 0.2f, "integral removes the steady lag",
              std::fabs(r.final_error_deg), 0.2f);
        check(r.max_step_deg < 0.5f / 476.0f + 1e-5f, "correction is rate-limited (never a snap)",
              r.max_step_deg * 476.0f, 0.5f);
    }

    std::printf("mag_heading_selftest: an existing offset is removed slowly, without a jump\n");
    {
        MagHeadingLock lock;
        lock.configure(MagHeadingLock::Config{});
        float yaw_error = 0.0f;
        run_loop(lock, yaw_error, 0.0f, 3.0f);
        yaw_error = 5.0f;  // e.g. a gyro scale error after a big turn
        const LoopResult r = run_loop(lock, yaw_error, 0.0f, 120.0f);
        std::printf("  5 deg offset after 120 s: %.3f deg\n", r.final_error_deg);
        check(std::fabs(r.final_error_deg) < 0.5f, "offset converges (small PI overshoot allowed)",
              std::fabs(r.final_error_deg), 0.5f);
    }

    std::printf("mag_heading_selftest: a magnetic disturbance is rejected, not followed\n");
    {
        MagHeadingLock lock;
        lock.configure(MagHeadingLock::Config{});
        float yaw_error = 0.0f;
        run_loop(lock, yaw_error, 0.0f, 3.0f);
        // A magnet near the glasses: field magnitude +30% and the reading
        // swings 40 deg, for 5 s. The estimate must not follow it.
        const float before = yaw_error;
        float disturbed_view = yaw_error;
        const float dt = 1.0f / 476.0f;
        for (int i = 0; i < static_cast<int>(5.0f / dt); ++i) {
            const Vec3 b = earth_field((disturbed_view + 40.0f) * kPi / 180.0f, 51.5f * 1.3f);
            disturbed_view += lock.update(b, (i % 9) == 0, 5.0f, dt) * 180.0f / kPi;
        }
        const float moved = std::fabs(disturbed_view - before);
        std::printf("  estimate moved %.4f deg during the disturbance (state %d)\n", moved,
                    static_cast<int>(lock.state()));
        check(moved < 0.05f, "no correction from a disturbed field", moved, 0.05f);
        check(lock.state() == MagHeadingLock::State::disturbed, "reports disturbed",
              static_cast<float>(lock.state()), 3.0f);
        yaw_error = disturbed_view;
        run_loop(lock, yaw_error, 0.0f, 5.0f);
        check(lock.state() == MagHeadingLock::State::locked, "relocks when the field returns",
              static_cast<float>(lock.state()), 2.0f);
    }

    std::printf("mag_heading_selftest: a new environment is re-acquired without a jump\n");
    {
        MagHeadingLock lock;
        lock.configure(MagHeadingLock::Config{});
        float yaw_error = 0.0f;
        run_loop(lock, yaw_error, 0.0f, 3.0f);
        // Moved to another desk: the local field is 20% stronger, 6 deg more dip
        // and points 30 deg elsewhere - consistently. Nothing may snap.
        const LoopResult r = run_loop(lock, yaw_error, 0.0f, 30.0f, 1.2f, 6.0f, 30.0f);
        std::printf("  after 30 s in the new field: state %d, reacquisitions %u, max |yaw change| %.3f deg\n",
                    static_cast<int>(lock.state()), lock.reacquisitions(), r.max_abs_error_deg);
        check(lock.reacquisitions() == 1, "re-acquired once", static_cast<float>(lock.reacquisitions()), 1.0f);
        check(lock.state() == MagHeadingLock::State::locked, "locked on the new field",
              static_cast<float>(lock.state()), 2.0f);
        check(r.max_abs_error_deg < 0.05f, "the view did not move", r.max_abs_error_deg, 0.05f);
    }

    std::printf("mag_heading_selftest: hard-iron fit recovers a known offset from a gyro-tracked turn\n");
    {
        const Vec3 hard_iron{-17.0f, -18.6f, 1.8f};
        std::vector<ImuSample> samples;
        const float dt = 21.0f * 1e-4f;
        uint32_t tick = 1000;
        // Attitude as a rotation matrix: package -> earth.
        float R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        const Vec3 b_earth = earth_field(0.3f);
        uint32_t noise = 99u;
        auto rnd = [&]() {
            noise = noise * 1664525u + 1013904223u;
            return (static_cast<float>((noise >> 8) & 0xffff) / 65535.0f - 0.5f);
        };
        Vec3 mag_hold{};
        const int n = static_cast<int>(60.0f / dt);
        for (int i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) * dt;
            // Body rates: a full slow turn about package Z plus nods/tilts.
            const Vec3 w_degs{25.0f * std::sin(t * 0.9f), 18.0f * std::sin(t * 1.3f + 1.0f),
                              t < 30.0f ? 14.0f : -10.0f};
            ImuSample s;
            s.gyro_degs = Vec3{w_degs.x + 0.4f + rnd() * 0.3f, w_degs.y - 0.2f + rnd() * 0.3f,
                               w_degs.z + 0.1f + rnd() * 0.3f};
            // Gravity in package frame: R^T * (0,0,9.81).
            s.accel_mps2 = Vec3{R[2][0] * 9.81f, R[2][1] * 9.81f, R[2][2] * 9.81f};
            if (i % 9 == 0) {
                // Field in package frame R^T b, then into raw mag axes (P^T), plus hard iron.
                const Vec3 pkg{R[0][0] * b_earth.x + R[1][0] * b_earth.y + R[2][0] * b_earth.z,
                               R[0][1] * b_earth.x + R[1][1] * b_earth.y + R[2][1] * b_earth.z,
                               R[0][2] * b_earth.x + R[1][2] * b_earth.y + R[2][2] * b_earth.z};
                // P maps raw->package: pkg = (raw.y, -raw.x, raw.z) -> raw = (-pkg.y, pkg.x, pkg.z)
                mag_hold = Vec3{-pkg.y + hard_iron.x + rnd() * 0.4f, pkg.x + hard_iron.y + rnd() * 0.4f,
                                pkg.z + hard_iron.z + rnd() * 0.4f};
            }
            s.mag_ut = mag_hold;
            s.tick_100us = tick += 21;
            samples.push_back(s);
            // Integrate R <- R * exp(w dt) (true rates, no bias).
            const float wx = w_degs.x * kPi / 180.0f * dt;
            const float wy = w_degs.y * kPi / 180.0f * dt;
            const float wz = w_degs.z * kPi / 180.0f * dt;
            const float angle = std::sqrt(wx * wx + wy * wy + wz * wz);
            if (angle > 0.0f) {
                const float ax = wx / angle, ay = wy / angle, az = wz / angle;
                const float c = std::cos(angle), sn = std::sin(angle), tt = 1.0f - c;
                const float D[3][3] = {{tt * ax * ax + c, tt * ax * ay - sn * az, tt * ax * az + sn * ay},
                                       {tt * ax * ay + sn * az, tt * ay * ay + c, tt * ay * az - sn * ax},
                                       {tt * ax * az - sn * ay, tt * ay * az + sn * ax, tt * az * az + c}};
                float Rn[3][3];
                for (int r = 0; r < 3; ++r) {
                    for (int col = 0; col < 3; ++col) {
                        Rn[r][col] = R[r][0] * D[0][col] + R[r][1] * D[1][col] + R[r][2] * D[2][col];
                    }
                }
                for (int r = 0; r < 3; ++r) {
                    for (int col = 0; col < 3; ++col) {
                        R[r][col] = Rn[r][col];
                    }
                }
            }
        }
        const MagFitResult fit = fit_mag_hard_iron(samples, Vec3{0.4f, -0.2f, 0.1f});
        const float err = std::sqrt((fit.hard_iron_ut.x - hard_iron.x) * (fit.hard_iron_ut.x - hard_iron.x) +
                                    (fit.hard_iron_ut.y - hard_iron.y) * (fit.hard_iron_ut.y - hard_iron.y) +
                                    (fit.hard_iron_ut.z - hard_iron.z) * (fit.hard_iron_ut.z - hard_iron.z));
        std::printf("  fit %s: hard iron (%.2f, %.2f, %.2f) |B| %.2f resid %.3f rotation %.0f tilt %.0f\n",
                    fit.code, fit.hard_iron_ut.x, fit.hard_iron_ut.y, fit.hard_iron_ut.z, fit.field_ut,
                    fit.residual_rms_ut, fit.rotation_coverage_deg, fit.tilt_coverage_deg);
        check(fit.ok, "fit accepted", fit.ok ? 1.0f : 0.0f, 1.0f);
        check(err < 0.5f, "hard iron recovered", err, 0.5f);
        check(std::fabs(fit.field_ut - 51.5f) < 0.5f, "field magnitude recovered",
              std::fabs(fit.field_ut - 51.5f), 0.5f);

        // A turn-only recording must be rejected (the axis component is weak).
        std::vector<ImuSample> short_window(samples.begin(), samples.begin() + 400);
        const MagFitResult too_short = fit_mag_hard_iron(short_window, Vec3{0.4f, -0.2f, 0.1f});
        check(!too_short.ok, "too-short recording rejected", too_short.ok ? 1.0f : 0.0f, 0.0f);
    }

    std::printf("mag_heading_selftest: estimator without a calibration never uses the magnetometer\n");
    {
        PoseEstimator est(PoseEstimator::Config{});
        check(!est.mag_lock_active(), "lock inactive without calibration", est.mag_lock_active() ? 1.0f : 0.0f,
              0.0f);
    }

    std::printf("mag_heading_selftest: %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
