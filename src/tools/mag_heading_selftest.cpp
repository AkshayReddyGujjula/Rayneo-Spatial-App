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

// ---------------------------------------------------------------------------
// Worn-head simulation through the full PoseEstimator, for the online
// hard-iron tracker. Package frame = head frame (sensor_to_head identity):
// x right, y forward, z up. True attitude R = Rz(yaw) Rx(pitch).

// Magnetic dipole field (uT) at offset r (m) from a moment m (A m^2):
// B = mu0/4pi (3 (m.r^) r^ - m) / |r|^3, mu0/4pi = 1e-7 T m/A.
Vec3 dipole_ut(const Vec3& m, const Vec3& r) {
    const float d = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    const Vec3 u{r.x / d, r.y / d, r.z / d};
    const float mu = m.x * u.x + m.y * u.y + m.z * u.z;
    const float k = 1e-7f / (d * d * d) * 1e6f;
    return Vec3{k * (3.0f * mu * u.x - m.x), k * (3.0f * mu * u.y - m.y), k * (3.0f * mu * u.z - m.z)};
}

// Package-frame vector -> raw magnetometer axes (inverse of kMagToPackage).
Vec3 package_to_raw(const Vec3& p) {
    return Vec3{-p.y, p.x, p.z};
}

struct WornSim {
    // Head-fixed extra field (package frame) switched on at `head_step_at`.
    Vec3 head_step_pkg{};
    float head_step_at = 1e9f;
    float head_step_until = 1e9f;
    // Earth-fixed extra field switched on at `earth_step_at`.
    Vec3 earth_step{};
    float earth_step_at = 1e9f;
    // An earth-fixed dipole carried past the head (a phone), centred on
    // `pass_at`, moving along +x at 0.15 m/s at closest distance pass_r.
    float pass_at = 1e9f;
    float pass_r = 0.25f;
    float seconds = 520.0f;
    bool track = true;

    // Results: estimator yaw during the centre holds (sign-agnostic: the
    // view should show the same yaw every time the head returns to centre).
    float centre_ref = 0.0f;
    bool have_ref = false;
    float max_centre_dev_after = 0.0f;  // from 200 s on
    float final_centre_dev = 0.0f;
    MagHardIronTracker::Config used;
    Vec3 final_offset{};
    uint32_t steps = 0, adopted = 0, rejected = 0, transients = 0;
};

// Desk look-around pattern: 30 s cycle - centre 14 s, glance to +35 deg
// (1.5 s ease, 4 s hold, 1.5 s back), centre 3 s, glance to -30 with a 12 deg
// nod down, back. Returns yaw/pitch (deg) and their rates; centre = true
// while both are at rest at zero.
void look_pattern(float t, float& yaw, float& pitch, float& yaw_rate, float& pitch_rate, bool& centre) {
    auto ease = [](float x) { return 0.5f - 0.5f * std::cos(kPi * x); };
    auto ease_rate = [](float x, float duration) { return 0.5f * kPi * std::sin(kPi * x) / duration; };
    yaw = pitch = yaw_rate = pitch_rate = 0.0f;
    centre = false;
    const float c = std::fmod(t, 30.0f);
    if (c < 14.0f) {
        centre = c > 2.0f;  // let the view settle after the return
        return;
    }
    auto glance = [&](float start, float target_yaw, float target_pitch) {
        const float local = c - start;
        float f = 0.0f;
        float fr = 0.0f;
        if (local < 1.5f) {
            f = ease(local / 1.5f);
            fr = ease_rate(local / 1.5f, 1.5f);
        } else if (local < 5.5f) {
            f = 1.0f;
        } else if (local < 7.0f) {
            f = 1.0f - ease((local - 5.5f) / 1.5f);
            fr = -ease_rate((local - 5.5f) / 1.5f, 1.5f);
        }
        yaw = target_yaw * f;
        pitch = target_pitch * f;
        yaw_rate = target_yaw * fr;
        pitch_rate = target_pitch * fr;
    };
    if (c < 21.0f) {
        glance(14.0f, 35.0f, 0.0f);
    } else if (c < 23.0f) {
        return;
    } else {
        glance(23.0f, -30.0f, -12.0f);
    }
}

void run_worn(WornSim& sim) {
    sim.have_ref = false;
    sim.centre_ref = 0.0f;
    sim.max_centre_dev_after = 0.0f;
    sim.final_centre_dev = 0.0f;
    PoseEstimator::Config cfg;
    cfg.sensor_to_head = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const Vec3 calibrated{-16.8f, -17.4f, 3.0f};
    cfg.mag_calibration.valid = true;
    cfg.mag_calibration.hard_iron_ut = calibrated;
    cfg.mag_calibration.field_ut = 51.5f;
    cfg.mag_lock.hard_iron.enabled = sim.track;
    sim.used = cfg.mag_lock.hard_iron;
    PoseEstimator est(cfg);

    const float dt = 21.0f * 1e-4f;
    const Vec3 b_earth = earth_field(0.3f);
    uint32_t noise = 12345u;
    auto rnd = [&]() {
        noise = noise * 1664525u + 1013904223u;
        return static_cast<float>((noise >> 8) & 0xffff) / 65535.0f - 0.5f;
    };
    uint32_t tick = 1000;
    Vec3 mag_hold{};
    const int n = static_cast<int>(sim.seconds / dt);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) * dt;
        float yaw = 0, pitch = 0, yr = 0, pr = 0;
        bool centre = false;
        if (t > 8.0f) {
            look_pattern(t - 8.0f, yaw, pitch, yr, pr, centre);
        }
        const float ps = std::sin(pitch * kPi / 180.0f);
        const float pc = std::cos(pitch * kPi / 180.0f);
        // R = Rz(yaw) Rx(pitch); body rates (pitch_rate, yaw_rate sin p, yaw_rate cos p).
        ImuSample s;
        s.gyro_degs = Vec3{pr + rnd() * 0.6f, yr * ps + rnd() * 0.6f, yr * pc + rnd() * 0.6f};
        // Gravity in the package frame: R^T (0,0,g) = (0, g sin p, g cos p).
        s.accel_mps2 = Vec3{rnd() * 0.02f, 9.81f * ps + rnd() * 0.02f, 9.81f * pc + rnd() * 0.02f};
        if (i % 5 == 0) {
            // Earth-frame field plus earth-fixed extras, into the package frame.
            Vec3 e = b_earth;
            if (t >= sim.earth_step_at) {
                e = Vec3{e.x + sim.earth_step.x, e.y + sim.earth_step.y, e.z + sim.earth_step.z};
            }
            if (std::fabs(t - sim.pass_at) < 6.0f) {
                // Phone (0.5 A m^2, moment along +z) carried past along x.
                const Vec3 r{0.15f * (t - sim.pass_at), sim.pass_r, 0.0f};
                const Vec3 bp = dipole_ut(Vec3{0.0f, 0.0f, 0.5f}, r);
                e = Vec3{e.x + bp.x, e.y + bp.y, e.z + bp.z};
            }
            const float yc = std::cos(yaw * kPi / 180.0f);
            const float ys = std::sin(yaw * kPi / 180.0f);
            // R^T e = Rx^T Rz^T e.
            const Vec3 z{yc * e.x + ys * e.y, -ys * e.x + yc * e.y, e.z};
            Vec3 pkg{z.x, pc * z.y + ps * z.z, -ps * z.y + pc * z.z};
            if (t >= sim.head_step_at && t < sim.head_step_until) {
                pkg = Vec3{pkg.x + sim.head_step_pkg.x, pkg.y + sim.head_step_pkg.y, pkg.z + sim.head_step_pkg.z};
            }
            const Vec3 raw = package_to_raw(pkg);
            mag_hold = Vec3{raw.x + calibrated.x + rnd() * 0.4f, raw.y + calibrated.y + rnd() * 0.4f,
                            raw.z + calibrated.z + rnd() * 0.4f};
        }
        s.mag_ut = mag_hold;
        s.temp_c = 30.0f;
        s.tick_100us = tick += 21;
        if (!est.add_sample(s)) {
            continue;
        }
        if (centre) {
            const float y = est.euler().yaw_deg;
            if (!sim.have_ref && t > 90.0f) {
                sim.centre_ref = y;
                sim.have_ref = true;
            }
            if (sim.have_ref) {
                float d = y - sim.centre_ref;
                while (d > 180.0f) d -= 360.0f;
                while (d < -180.0f) d += 360.0f;
                if (t > 200.0f) {
                    sim.max_centre_dev_after = std::max(sim.max_centre_dev_after, std::fabs(d));
                }
                sim.final_centre_dev = d;
            }
        }
    }
    const auto& tr = est.mag_lock().hard_iron_tracker();
    sim.final_offset = tr.offset_from_calibration_ut();
    sim.steps = tr.steps_detected();
    sim.adopted = tr.hypotheses_adopted();
    sim.rejected = tr.hypotheses_rejected();
    sim.transients = tr.transients_rejected();
}

float norm3(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

void print_worn(const char* label, const WornSim& s) {
    std::printf("  %-34s centre dev max %.2f deg final %+.2f deg | steps %u adopted %u rejected %u transients %u "
                "offset (%+.2f, %+.2f, %+.2f) uT\n",
                label, s.max_centre_dev_after, s.final_centre_dev, s.steps, s.adopted, s.rejected, s.transients,
                s.final_offset.x, s.final_offset.y, s.final_offset.z);
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
        std::printf("  observability %.3f (weak axis %.2f %.2f %.2f), quality %.2f\n", fit.observability,
                    fit.weak_axis_package.x, fit.weak_axis_package.y, fit.weak_axis_package.z,
                    mag_fit_quality(fit));
        check(fit.observability > 0.15f, "full turn plus nods observes every direction", fit.observability,
              0.15f);
        check(mag_fit_quality(fit) > 0.6f, "good recording scores well", mag_fit_quality(fit), 0.6f);

        // Yaw-only recording (rotation about package Z alone): the Z component
        // of the hard iron is unobservable and must be reported as the weak axis.
        {
            std::vector<ImuSample> yaw_only;
            float Rz = 0.0f;
            Vec3 hold{};
            uint32_t t2 = 5000;
            for (int i = 0; i < static_cast<int>(40.0f / dt); ++i) {
                ImuSample s;
                s.gyro_degs = Vec3{0.0f, 0.0f, 12.0f};
                s.accel_mps2 = Vec3{0.0f, 0.0f, 9.81f};
                if (i % 9 == 0) {
                    const float c = std::cos(Rz);
                    const float sn = std::sin(Rz);
                    const Vec3 pkg{c * b_earth.x + sn * b_earth.y, -sn * b_earth.x + c * b_earth.y, b_earth.z};
                    hold = Vec3{-pkg.y + hard_iron.x, pkg.x + hard_iron.y, pkg.z + hard_iron.z};
                }
                s.mag_ut = hold;
                s.tick_100us = t2 += 21;
                yaw_only.push_back(s);
                Rz += 12.0f * kPi / 180.0f * dt;
            }
            const MagFitResult yfit = fit_mag_hard_iron(yaw_only, Vec3{});
            std::printf("  yaw-only: %s observability %.4f weak axis (%.2f %.2f %.2f) quality %.2f\n", yfit.code,
                        yfit.observability, yfit.weak_axis_package.x, yfit.weak_axis_package.y,
                        yfit.weak_axis_package.z, mag_fit_quality(yfit));
            check(yfit.observability < 0.01f, "yaw-only leaves one direction unobserved", yfit.observability,
                  0.01f);
            check(std::fabs(yfit.weak_axis_package.z) > 0.99f, "weak axis is the turn axis",
                  std::fabs(yfit.weak_axis_package.z), 0.99f);
            check(mag_fit_quality(yfit) < 0.1f, "yaw-only scores low", mag_fit_quality(yfit), 0.1f);
        }

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

    std::printf("mag_heading_selftest: online hard-iron tracking (worn head, full estimator)\n");
    {
        // Right earbud: 0.04 A m^2 moment pointing out of the ear (+x), 9 cm
        // behind and 3 cm below a temple-mounted sensor.
        const Vec3 earbud = dipole_ut(Vec3{0.04f, 0.0f, 0.0f}, Vec3{0.0f, -0.09f, -0.03f});
        std::printf("  modelled earbud field at the sensor: (%+.2f, %+.2f, %+.2f) uT, |%.2f| uT\n", earbud.x,
                    earbud.y, earbud.z, norm3(earbud));
        check(norm3(earbud) > 2.0f && norm3(earbud) < 10.0f, "earbud dipole is a few uT", norm3(earbud), 10.0f);

        WornSim quiet_off;
        quiet_off.track = false;
        run_worn(quiet_off);
        print_worn("no event, tracking off:", quiet_off);
        WornSim quiet_on;
        run_worn(quiet_on);
        print_worn("no event, tracking on:", quiet_on);
        check(quiet_on.steps == 0 && norm3(quiet_on.final_offset) < 1e-6f, "no event: no step, offset untouched",
              static_cast<float>(quiet_on.steps), 0.0f);
        check(std::fabs(quiet_on.max_centre_dev_after - quiet_off.max_centre_dev_after) < 0.05f,
              "no event: tracking changes nothing",
              std::fabs(quiet_on.max_centre_dev_after - quiet_off.max_centre_dev_after), 0.05f);

        WornSim bud_off;
        bud_off.track = false;
        bud_off.head_step_pkg = earbud;
        bud_off.head_step_at = 130.0f;
        run_worn(bud_off);
        print_worn("earbud in at 130 s, tracking off:", bud_off);
        WornSim bud_on = bud_off;
        bud_on.track = true;
        run_worn(bud_on);
        print_worn("earbud in at 130 s, tracking on:", bud_on);
        const Vec3 err{bud_on.final_offset.x - package_to_raw(earbud).x,
                       bud_on.final_offset.y - package_to_raw(earbud).y,
                       bud_on.final_offset.z - package_to_raw(earbud).z};
        check(bud_on.max_centre_dev_after < 0.75f, "with tracking the workspace stays put",
              bud_on.max_centre_dev_after, 0.75f);
        check(bud_on.adopted == 1 && norm3(err) < 0.75f, "earbud learned into the offset", norm3(err), 0.75f);

        // In at 130 s, out again at 330 s.
        WornSim inout_off = bud_off;
        inout_off.head_step_until = 330.0f;
        inout_off.seconds = 640.0f;
        run_worn(inout_off);
        print_worn("earbud in 130 s, out 330 s, off:", inout_off);
        WornSim inout_on = inout_off;
        inout_on.track = true;
        run_worn(inout_on);
        print_worn("earbud in 130 s, out 330 s, on:", inout_on);
        check(inout_off.max_centre_dev_after > 2.0f, "without tracking, in-and-out moves the workspace",
              inout_off.max_centre_dev_after, 2.0f);
        check(norm3(inout_on.final_offset) < 0.75f, "removal returns the offset to the calibration",
              norm3(inout_on.final_offset), 0.75f);
        check(inout_on.max_centre_dev_after < 0.75f, "in and out: the workspace stays put",
              inout_on.max_centre_dev_after, 0.75f);

        // Earth-fixed step: a magnet put down near the desk (3 uT, mostly
        // horizontal). Rotation shows it is not head-fixed: the provisional
        // offset is reverted.
        WornSim desk_off;
        desk_off.track = false;
        desk_off.earth_step = Vec3{2.5f, -1.5f, 0.8f};
        desk_off.earth_step_at = 130.0f;
        run_worn(desk_off);
        print_worn("magnet on the desk, tracking off:", desk_off);
        WornSim desk_on = desk_off;
        desk_on.track = true;
        run_worn(desk_on);
        print_worn("magnet on the desk, tracking on:", desk_on);
        check(norm3(desk_on.final_offset) < 0.5f, "earth-fixed change is not learned into the offset",
              norm3(desk_on.final_offset), 0.5f);
        check(desk_on.max_centre_dev_after < desk_off.max_centre_dev_after + 0.5f,
              "earth-fixed change: no worse than without tracking",
              desk_on.max_centre_dev_after - desk_off.max_centre_dev_after, 0.5f);

        // A phone carried past the head at 25 cm: a transient, never learned.
        WornSim phone_off;
        phone_off.track = false;
        phone_off.pass_at = 160.0f;
        run_worn(phone_off);
        print_worn("phone carried past, tracking off:", phone_off);
        WornSim phone_on = phone_off;
        phone_on.track = true;
        run_worn(phone_on);
        print_worn("phone carried past, tracking on:", phone_on);
        check(norm3(phone_on.final_offset) < 0.2f, "a passing magnet is not learned", norm3(phone_on.final_offset),
              0.2f);
        check(phone_on.max_centre_dev_after < phone_off.max_centre_dev_after + 0.3f,
              "a passing magnet moves the view no more than before",
              phone_on.max_centre_dev_after - phone_off.max_centre_dev_after, 0.3f);
    }

    std::printf("mag_heading_selftest: %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
