// pose_scenarios.cpp
//
// Drives the REAL PoseEstimator with a synthetic, realistic IMU stream that
// mimics a worn head and asserts on the published pose. This is the regression
// harness for the reported "the centre screen ends up slightly off to one side
// after a pan out to the screen edge and back" bug.
//
// Root cause: the estimator's adaptation paths (the gyro-bias adaptation and the
// drift-absorption path) fold part of a *deliberate* movement into their
// corrections, because their gates look only at signal *variation* (a constant
// rate looks perfectly still to a variation detector) plus a loose 5 deg/s rate
// cap, with no hold-off after motion. A slow pan therefore looks exactly like a
// residual gyro bias and is subtracted out.
//
// Build this file against the committed (pre-fix) estimator to see the reported
// bug: scenarios 1, 2, 3, 4 and 9 fail. Against the fixed estimator (tightened
// rate cap + motion hold-off) every scenario passes. Scenario 2 is the mirror of
// scenario 1 and therefore fails alongside it.
//
// Synthetic sensor model: we maintain the true attitude (body -> earth), then
// emit exactly what a worn head would report at 476 Hz:
//     accel_body = R^T * (0, 0, 9.81)
//     gyro_body  = R^T * omega_earth
// converted body -> sensor package with {x, z, -y} (inverse of the estimator's
// sensor_to_head mapping). White gyro noise (0.15 deg/s rms) and a small
// random walk on the residual bias are always present; breathing-like sway is
// opt-in.
//
// Only this file and CMakeLists.txt are owned by the task; nothing under
// src/imu or src/render is modified.

#include "imu/pose_estimator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

using namespace gt;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDt = 21.0f * 1e-4f;  // device tick advances 21 x 100 us => 476 Hz
constexpr float kRateHz = 476.0f;
constexpr float kDegToRad = kPi / 180.0f;

// ---- small 3x3 helpers (same convention as pose_selftest) -------------------

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

// r is body -> earth, so applying it here means multiplying by r^T.
Vec3 earth_to_body(const Mat3& r, const Vec3& v) {
    return Vec3{r.m[0][0] * v.x + r.m[1][0] * v.y + r.m[2][0] * v.z,
                r.m[0][1] * v.x + r.m[1][1] * v.y + r.m[2][1] * v.z,
                r.m[0][2] * v.x + r.m[1][2] * v.y + r.m[2][2] * v.z};
}

// body -> sensor package: inverse of the default sensor_to_head mapping, so it
// must be {x, z, -y} (yaw rate lives on the package Y axis).
Vec3 package_from_body(const Vec3& b) {
    return Vec3{b.x, b.z, -b.y};
}

int g_failures = 0;

void check(bool condition, const char* label, float measured, float limit) {
    std::printf("  [%s] %s (measured %.4f, limit %.4f)\n", condition ? "PASS" : "FAIL", label,
                measured, limit);
    if (!condition) {
        ++g_failures;
    }
}

void check_near(bool condition, const char* label, float measured, float target, float tol) {
    std::printf("  [%s] %s (measured %.4f, target %.4f, tol %.3f)\n",
                condition ? "PASS" : "FAIL", label, measured, target, tol);
    if (!condition) {
        ++g_failures;
    }
}

// ---- synthetic worn-head sensor model ---------------------------------------

struct HeadSim {
    PoseEstimator est;
    Mat3 attitude{};             // body -> earth
    Vec3 bias_pkg_degs{};        // residual gyro bias, sensor-package frame
    uint32_t tick = 100000;
    float t = 0.0f;

    // Breathing-like sway: adds a 0.3 deg/s rate sinusoid at 0.25 Hz to yaw.
    bool sway = false;
    float sway_amp_degs = 0.3f;
    float sway_hz = 0.25f;
    float bias_walk_degs = 5e-4f;  // small per-sample random walk

    std::mt19937 rng;
    std::normal_distribution<float> white{0.0f, 0.15f};  // deg/s rms gyro noise
    std::normal_distribution<float> unit{0.0f, 1.0f};

    explicit HeadSim(uint32_t seed) : rng(seed) {
        attitude.m[0][0] = 1.0f;
        attitude.m[1][1] = 1.0f;
        attitude.m[2][2] = 1.0f;
    }

    void step(const Vec3& omega_earth_degs) {
        Vec3 omega = omega_earth_degs;
        if (sway) {
            omega.z += sway_amp_degs * std::sin(2.0f * kPi * sway_hz * t);
        }
        const Vec3 accel_body = earth_to_body(attitude, Vec3{0.0f, 0.0f, 9.81f});
        const Vec3 gyro_body = earth_to_body(attitude, omega);

        bias_pkg_degs.x += bias_walk_degs * unit(rng);
        bias_pkg_degs.y += bias_walk_degs * unit(rng);
        bias_pkg_degs.z += bias_walk_degs * unit(rng);

        Vec3 gyro_pkg = package_from_body(gyro_body);
        gyro_pkg.x += bias_pkg_degs.x + white(rng);
        gyro_pkg.y += bias_pkg_degs.y + white(rng);
        gyro_pkg.z += bias_pkg_degs.z + white(rng);

        ImuSample s;
        s.accel_mps2 = package_from_body(accel_body);
        s.gyro_degs = gyro_pkg;
        s.mag_ut = Vec3{};
        s.tick_100us = tick += 21;
        est.add_sample(s);

        const Mat3 rx = rotation_about(1.0f, 0.0f, 0.0f, omega.x * kDegToRad * kDt);
        const Mat3 ry = rotation_about(0.0f, 1.0f, 0.0f, omega.y * kDegToRad * kDt);
        const Mat3 rz = rotation_about(0.0f, 0.0f, 1.0f, omega.z * kDegToRad * kDt);
        attitude = multiply(rz, multiply(ry, multiply(rx, attitude)));
        t += kDt;
    }

    // Smooth movement: raised-cosine ramps (the rate starts and ends at exactly
    // zero, like a real head movement) around a constant-rate plateau. Returns
    // the earth-frame total angle (deg) that was applied, so callers can compare
    // against the commanded angle.
    Vec3 move(float peak_rate_degs, float ramp_s, float sustain_s, const Vec3& axis) {
        const float n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
        const Vec3 u{axis.x / n, axis.y / n, axis.z / n};
        const float total_s = 2.0f * ramp_s + sustain_s;
        const int samples = static_cast<int>(std::lround(total_s * kRateHz));
        for (int i = 0; i < samples; ++i) {
            const float local = static_cast<float>(i) / kRateHz;
            float mag;
            if (local < ramp_s) {
                mag = peak_rate_degs * 0.5f * (1.0f - std::cos(kPi * local / ramp_s));
            } else if (local < ramp_s + sustain_s) {
                mag = peak_rate_degs;
            } else {
                const float tail = (local - ramp_s - sustain_s) / ramp_s;
                mag = peak_rate_degs * 0.5f * (1.0f + std::cos(kPi * tail));
            }
            step(Vec3{u.x * mag, u.y * mag, u.z * mag});
        }
        const float mag_total = peak_rate_degs * (ramp_s + sustain_s);
        return Vec3{u.x * mag_total, u.y * mag_total, u.z * mag_total};
    }

    void hold(float seconds) {
        const int samples = static_cast<int>(std::lround(seconds * kRateHz));
        for (int i = 0; i < samples; ++i) {
            step(Vec3{});
        }
    }

    float yaw() const { return est.euler().yaw_deg; }
    float pitch() const { return est.euler().pitch_deg; }
    float roll() const { return est.euler().roll_deg; }
};

constexpr float kWarmupS = 4.0f;  // settle + bias calibration + margin

HeadSim make_sim(uint32_t seed, float initial_bias_y_degs = 0.0f) {
    HeadSim sim(seed);
    sim.bias_pkg_degs.y = initial_bias_y_degs;  // residual bias present from power-on
    sim.est.configure(PoseEstimator::Config{});
    sim.hold(kWarmupS);
    return sim;
}

void print_bias(const HeadSim& sim, const char* when) {
    const Vec3 b = sim.est.gyro_bias_degs();
    std::printf("  bias %s: (%.4f, %.4f, %.4f) deg/s, drift_correction %.4f deg\n", when, b.x, b.y,
                b.z, sim.est.drift_correction_degs());
}

}  // namespace

int main() {
    std::printf("pose_scenarios: synthetic worn-head IMU driving the real PoseEstimator\n");

    // --- 1: pan-and-return right -------------------------------------------
    std::printf("\n-- scenario 1: smooth pan +60 deg, hold 2 s, return, hold 3 s --\n");
    {
        HeadSim sim = make_sim(0xC0FFEE01u);
        std::printf("  calibrated=%d\n", sim.est.bias_done() ? 1 : 0);
        print_bias(sim, "after warmup");
        const float y0 = sim.yaw();
        sim.move(60.0f, 1.0f, 0.0f, Vec3{0.0f, 0.0f, 1.0f});  // peak 60 deg/s over 2 s
        const float y1 = sim.yaw();
        sim.hold(2.0f);
        const float y2 = sim.yaw();
        sim.move(-60.0f, 1.0f, 0.0f, Vec3{0.0f, 0.0f, 1.0f});
        sim.hold(3.0f);
        const float yf = sim.yaw();
        std::printf("  yaw: start %.4f -> after +60 deg %.4f -> after 2 s hold %.4f -> final %.4f\n",
                    y0, y1, y2, yf);
        print_bias(sim, "at end");
        check(std::fabs(yf) < 0.3f, "final yaw returns to centre", yf, 0.3f);
    }

    // --- 2: pan-and-return left --------------------------------------------
    std::printf("\n-- scenario 2: smooth pan -60 deg, hold 2 s, return, hold 3 s --\n");
    {
        HeadSim sim = make_sim(0xC0FFEE02u);
        const float y0 = sim.yaw();
        sim.move(-60.0f, 1.0f, 0.0f, Vec3{0.0f, 0.0f, 1.0f});
        const float y1 = sim.yaw();
        sim.hold(2.0f);
        sim.move(60.0f, 1.0f, 0.0f, Vec3{0.0f, 0.0f, 1.0f});
        sim.hold(3.0f);
        const float yf = sim.yaw();
        std::printf("  yaw: start %.4f -> after -60 deg %.4f -> final %.4f\n", y0, y1, yf);
        print_bias(sim, "at end");
        check(std::fabs(yf) < 0.3f, "final yaw returns to centre", yf, 0.3f);
    }

    // --- 3: small adjustments with holds -----------------------------------
    std::printf("\n-- scenario 3: small moves +5 -5 +2 -2 +8 -8, 1 s hold after each --\n");
    {
        HeadSim sim = make_sim(0xC0FFEE03u);
        const float deltas[6] = {5.0f, -5.0f, 2.0f, -2.0f, 8.0f, -8.0f};
        float cumulative = 0.0f;
        for (int i = 0; i < 6; ++i) {
            const float peak = deltas[i] / 0.2f;  // 0.2 s ramp, no plateau
            sim.move(peak, 0.2f, 0.0f, Vec3{0.0f, 0.0f, 1.0f});
            cumulative += deltas[i];
            sim.hold(1.0f);
            const float y = sim.yaw();
            char label[96];
            std::snprintf(label, sizeof(label),
                          "move %+d deg -> pose holds at commanded %+.1f deg", static_cast<int>(deltas[i]),
                          cumulative);
            check_near(std::fabs(y - cumulative) < 0.4f, label, y, cumulative, 0.4f);
        }
    }

    // --- 4: slow steady pan -------------------------------------------------
    std::printf("\n-- scenario 4: slow steady pan 2 deg/s for ~5 s (10 deg total) --\n");
    {
        HeadSim sim = make_sim(0xC0FFEE04u);
        print_bias(sim, "before pan");
        // 2 deg/s plateau with short cosine ramps: peak*(ramp+sustain) = 2*5 = 10 deg.
        sim.move(2.0f, 0.25f, 4.75f, Vec3{0.0f, 0.0f, 1.0f});
        const float y = sim.yaw();
        const float registered = y / 10.0f * 100.0f;
        std::printf("  registered yaw %.4f deg (%.1f%% of the commanded 10 deg)\n", y, registered);
        print_bias(sim, "after pan");
        check(y >= 8.0f, "at least 80% of the slow pan registers", y, 8.0f);
    }

    // --- 5: fast pan (regression) ------------------------------------------
    std::printf("\n-- scenario 5: fast pan 120 deg/s for ~1 s (120 deg total) --\n");
    {
        HeadSim sim = make_sim(0xC0FFEE05u);
        sim.move(120.0f, 0.1f, 0.9f, Vec3{0.0f, 0.0f, 1.0f});  // total 120 deg over 1.1 s
        const float y = sim.yaw();
        const float registered = y / 120.0f * 100.0f;
        std::printf("  registered yaw %.4f deg (%.1f%% of the commanded 120 deg)\n", y, registered);
        check(y >= 114.0f, "at least 95% of the fast pan registers", y, 114.0f);
    }

    // --- 6: creep while still -----------------------------------------------
    std::printf("\n-- scenario 6: 0.5 deg/s residual yaw bias, hold still 30 s --\n");
    {
        HeadSim sim = make_sim(0xC0FFEE06u, 0.5f);  // residual yaw bias present from power-on
        const float y0 = sim.yaw();
        sim.hold(30.0f);
        const float y1 = sim.yaw();
        const float drift = std::fabs(y1 - y0);
        std::printf("  yaw start %.4f end %.4f => drift %.4f deg\n", y0, y1, drift);
        print_bias(sim, "at end");
        check(drift < 0.5f, "residual bias does not creep the published yaw", drift, 0.5f);
    }

    // --- 7: breathing while still -------------------------------------------
    std::printf("\n-- scenario 7: breathing sway 0.3 deg/s @ 0.25 Hz, hold still 30 s --\n");
    {
        HeadSim sim = make_sim(0xC0FFEE07u);
        sim.sway = true;
        float lo = 1e9f;
        float hi = -1e9f;
        const int n = static_cast<int>(std::lround(30.0f * kRateHz));
        for (int i = 0; i < n; ++i) {
            sim.step(Vec3{});
            const float y = sim.yaw();
            lo = std::min(lo, y);
            hi = std::max(hi, y);
        }
        const float envelope = 0.5f * (hi - lo);  // semi-amplitude (half peak-to-peak)
        std::printf("  yaw min %.4f max %.4f => envelope %.4f deg (peak-to-peak %.4f)\n", lo, hi,
                    envelope, hi - lo);
        check(envelope < 0.4f, "breathing envelope stays small", envelope, 0.4f);
    }

    // --- 8: diagonal --------------------------------------------------------
    std::printf("\n-- scenario 8: diagonal yaw +40 / pitch +15 and back --\n");
    {
        HeadSim sim = make_sim(0xC0FFEE08u);
        // Yaw is about earth Z, nod (pitch) about earth Y. Axis {0, 15, 40} gives
        // Euler yaw ~40 deg and pitch ~14 deg; total magnitude 42.72 deg over 1 s.
        const float mag = std::sqrt(15.0f * 15.0f + 40.0f * 40.0f);
        const float peak = mag / 0.5f;  // 0.5 s raised-cosine rise, 0.5 s fall
        sim.move(peak, 0.5f, 0.0f, Vec3{0.0f, 15.0f, 40.0f});
        const float yaw_out = sim.yaw();
        const float pitch_out = sim.pitch();
        sim.hold(1.0f);
        sim.move(-peak, 0.5f, 0.0f, Vec3{0.0f, 15.0f, 40.0f});
        sim.hold(2.0f);
        const float yaw_back = sim.yaw();
        const float pitch_back = sim.pitch();
        std::printf("  out: yaw %.4f pitch %.4f | back: yaw %.4f pitch %.4f | roll %.4f\n", yaw_out,
                    pitch_out, yaw_back, pitch_back, sim.roll());
        check(std::fabs(yaw_back) < 0.5f, "yaw returns after the diagonal", yaw_back, 0.5f);
        check(std::fabs(pitch_back) < 0.5f, "pitch returns after the diagonal", pitch_back, 0.5f);
    }

    // --- 9: recenter --------------------------------------------------------
    std::printf("\n-- scenario 9: recenter after a +30 deg pan, then 5 s still with bias --\n");
    {
        HeadSim sim = make_sim(0xC0FFEE09u, 0.5f);  // residual yaw bias present from power-on
        // A deliberate but unhurried +30 deg pan (4 deg/s plateau): peak*full width
        // = 4 * 7.5 = 30 deg. Its sub-5 deg/s ramps and plateau are exactly what the
        // pre-fix adaptation paths mistake for drift.
        sim.move(4.0f, 0.5f, 7.0f, Vec3{0.0f, 0.0f, 1.0f});
        sim.hold(0.5f);
        std::printf("  yaw before recenter %.4f, drift_correction %.4f deg\n", sim.yaw(),
                    sim.est.drift_correction_degs());
        sim.est.recenter();
        const float at_recenter = sim.yaw();
        const float pitch_at = sim.pitch();
        std::printf("  immediately after recenter: yaw %.4f pitch %.4f roll %.4f\n", at_recenter,
                    pitch_at, sim.roll());
        check_near(std::fabs(at_recenter) < 0.05f, "recenter publishes exactly zero yaw", at_recenter,
                   0.0f, 0.05f);
        check_near(std::fabs(pitch_at) < 0.05f, "recenter publishes exactly zero pitch", pitch_at, 0.0f,
                   0.05f);

        float max_abs_yaw = 0.0f;
        const int n = static_cast<int>(std::lround(5.0f * kRateHz));
        for (int i = 0; i < n; ++i) {
            sim.step(Vec3{});
            max_abs_yaw = std::max(max_abs_yaw, std::fabs(sim.yaw()));
        }
        std::printf("  max |yaw| over 5 s of stillness: %.4f deg\n", max_abs_yaw);
        check(max_abs_yaw < 0.3f, "recentred pose stays put over 5 s with bias", max_abs_yaw, 0.3f);
    }

    std::printf("\npose_scenarios: %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
