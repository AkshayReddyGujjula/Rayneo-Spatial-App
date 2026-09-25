#include "imu/mag_heading.h"

#include <algorithm>
#include <cmath>

namespace gt {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegPerRad = 180.0f / kPi;
constexpr float kRadPerDeg = kPi / 180.0f;

float wrap_pi(float a) {
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
}

Vec3 mat_apply(const std::array<float, 9>& m, const Vec3& v) {
    return Vec3{m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

// Small dense solver for the 6x6 normal equations (Gaussian elimination with
// partial pivoting). Returns false on a singular system.
bool solve6(double a[6][6], double b[6], double x[6]) {
    for (int col = 0; col < 6; ++col) {
        int pivot = col;
        for (int row = col + 1; row < 6; ++row) {
            if (std::fabs(a[row][col]) > std::fabs(a[pivot][col])) {
                pivot = row;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-9) {
            return false;
        }
        if (pivot != col) {
            for (int k = 0; k < 6; ++k) {
                std::swap(a[col][k], a[pivot][k]);
            }
            std::swap(b[col], b[pivot]);
        }
        for (int row = col + 1; row < 6; ++row) {
            const double f = a[row][col] / a[col][col];
            for (int k = col; k < 6; ++k) {
                a[row][k] -= f * a[col][k];
            }
            b[row] -= f * b[col];
        }
    }
    for (int row = 5; row >= 0; --row) {
        double s = b[row];
        for (int k = row + 1; k < 6; ++k) {
            s -= a[row][k] * x[k];
        }
        x[row] = s / a[row][row];
    }
    return true;
}

struct Mat3d {
    double m[3][3];
};

Mat3d mat_identity() {
    return Mat3d{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
}

Mat3d mat_mul(const Mat3d& a, const Mat3d& b) {
    Mat3d r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return r;
}

Mat3d rotvec_to_mat(double x, double y, double z) {
    const double angle = std::sqrt(x * x + y * y + z * z);
    if (angle < 1e-12) {
        return mat_identity();
    }
    const double ax = x / angle;
    const double ay = y / angle;
    const double az = z / angle;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double t = 1.0 - c;
    return Mat3d{{{t * ax * ax + c, t * ax * ay - s * az, t * ax * az + s * ay},
                  {t * ax * ay + s * az, t * ay * ay + c, t * ay * az - s * ax},
                  {t * ax * az - s * ay, t * ay * az + s * ax, t * az * az + c}}};
}

}  // namespace

Vec3 mag_to_package(const Vec3& raw_ut, const MagCalibration& cal) {
    const Vec3 centred{raw_ut.x - cal.hard_iron_ut.x, raw_ut.y - cal.hard_iron_ut.y,
                       raw_ut.z - cal.hard_iron_ut.z};
    return mat_apply(kMagToPackage, centred);
}

MagFitResult fit_mag_hard_iron(const std::vector<ImuSample>& samples, const Vec3& gyro_bias_degs) {
    MagFitResult result;
    if (samples.size() < 2000) {
        result.code = "TOO_SHORT";
        return result;
    }

    // P as a double matrix.
    Mat3d p{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            p.m[i][j] = kMagToPackage[static_cast<size_t>(i * 3 + j)];
        }
    }

    double ata[6][6] = {};
    double aty[6] = {};
    Mat3d attitude = mat_identity();
    Vec3 last_mag{};
    bool have_last = false;
    uint32_t last_tick = samples.front().tick_100us;
    int rows = 0;
    double max_rotation_rad = 0.0;
    double max_tilt_rad = 0.0;
    const Vec3 a0 = samples.front().accel_mps2;
    const double a0n = std::sqrt(a0.x * a0.x + a0.y * a0.y + a0.z * a0.z);
    std::vector<Mat3d> used_rp;
    std::vector<Vec3> used_m;
    used_rp.reserve(samples.size() / 8);
    used_m.reserve(samples.size() / 8);

    for (size_t i = 0; i < samples.size(); ++i) {
        const ImuSample& s = samples[i];
        if (i > 0) {
            const uint32_t delta = s.tick_100us - last_tick;
            const double dt = static_cast<double>(delta) * 1e-4;
            if (dt <= 0.0 || dt >= 0.05) {
                result.code = "GAP";
                return result;
            }
            const double wx = (samples[i - 1].gyro_degs.x - gyro_bias_degs.x) * kRadPerDeg * dt;
            const double wy = (samples[i - 1].gyro_degs.y - gyro_bias_degs.y) * kRadPerDeg * dt;
            const double wz = (samples[i - 1].gyro_degs.z - gyro_bias_degs.z) * kRadPerDeg * dt;
            attitude = mat_mul(attitude, rotvec_to_mat(wx, wy, wz));
        }
        last_tick = s.tick_100us;
        {
            const double trace = attitude.m[0][0] + attitude.m[1][1] + attitude.m[2][2];
            const double angle = std::acos(std::max(-1.0, std::min(1.0, (trace - 1.0) * 0.5)));
            max_rotation_rad = std::max(max_rotation_rad, angle);
            const double an = std::sqrt(s.accel_mps2.x * s.accel_mps2.x + s.accel_mps2.y * s.accel_mps2.y +
                                        s.accel_mps2.z * s.accel_mps2.z);
            if (an > 1e-3 && a0n > 1e-3) {
                const double cosine =
                    (s.accel_mps2.x * a0.x + s.accel_mps2.y * a0.y + s.accel_mps2.z * a0.z) / (an * a0n);
                max_tilt_rad = std::max(max_tilt_rad, std::acos(std::max(-1.0, std::min(1.0, cosine))));
            }
        }

        const bool fresh = !have_last || s.mag_ut.x != last_mag.x || s.mag_ut.y != last_mag.y ||
                           s.mag_ut.z != last_mag.z;
        last_mag = s.mag_ut;
        have_last = true;
        if (!fresh) {
            continue;
        }
        const Mat3d rp = mat_mul(attitude, p);
        const double m[3] = {s.mag_ut.x, s.mag_ut.y, s.mag_ut.z};
        for (int r = 0; r < 3; ++r) {
            // R P (m - c) = b  ->  row [rp[r][0..2], +e_r] . [c; b] = rp[r] . m
            double row[6] = {rp.m[r][0], rp.m[r][1], rp.m[r][2], 0.0, 0.0, 0.0};
            row[3 + r] = 1.0;
            const double y = rp.m[r][0] * m[0] + rp.m[r][1] * m[1] + rp.m[r][2] * m[2];
            for (int a = 0; a < 6; ++a) {
                aty[a] += row[a] * y;
                for (int b = 0; b < 6; ++b) {
                    ata[a][b] += row[a] * row[b];
                }
            }
        }
        used_rp.push_back(rp);
        used_m.push_back(s.mag_ut);
        ++rows;
    }
    if (rows < 200) {
        result.code = "TOO_FEW_MAG_SAMPLES";
        return result;
    }

    double x[6] = {};
    if (!solve6(ata, aty, x)) {
        result.code = "SINGULAR";
        return result;
    }
    result.hard_iron_ut = Vec3{static_cast<float>(x[0]), static_cast<float>(x[1]), static_cast<float>(x[2])};
    result.field_ut = static_cast<float>(std::sqrt(x[3] * x[3] + x[4] * x[4] + x[5] * x[5]));

    // Residuals.
    double sq = 0.0;
    for (size_t k = 0; k < used_m.size(); ++k) {
        const double c[3] = {used_m[k].x - x[0], used_m[k].y - x[1], used_m[k].z - x[2]};
        for (int r = 0; r < 3; ++r) {
            const double pred = used_rp[k].m[r][0] * c[0] + used_rp[k].m[r][1] * c[1] +
                                used_rp[k].m[r][2] * c[2];
            const double e = pred - x[3 + r];
            sq += e * e;
        }
    }
    result.residual_rms_ut = static_cast<float>(std::sqrt(sq / (3.0 * static_cast<double>(used_m.size()))));
    result.rotation_coverage_deg = static_cast<float>(max_rotation_rad * kDegPerRad);
    result.tilt_coverage_deg = static_cast<float>(max_tilt_rad * kDegPerRad);

    // Plausibility: an earth field is 25..70 uT; the fit must explain the data
    // to ~1.5 uT; and the head must have covered a real range of directions.
    if (result.field_ut < 20.0f || result.field_ut > 80.0f) {
        result.code = "IMPLAUSIBLE_FIELD";
        return result;
    }
    if (result.residual_rms_ut > 1.5f) {
        result.code = "POOR_FIT";
        return result;
    }
    // A pure yaw turn leaves the hard-iron component along the turn axis
    // weakly determined; the fit needs a real turn AND some nod/tilt.
    if (result.rotation_coverage_deg < 90.0f || result.tilt_coverage_deg < 20.0f) {
        result.code = "SMALL_COVERAGE";
        return result;
    }
    result.ok = true;
    result.code = "OK";
    return result;
}

void MagHeadingLock::configure(const Config& cfg) {
    cfg_ = cfg;
    reset();
}

void MagHeadingLock::reset() {
    state_ = State::acquiring;
    begin_acquire();
    ref_heading_rad_ = 0.0f;
    ref_field_ut_ = 0.0f;
    ref_dip_deg_ = 0.0f;
    rejected_s_ = 0.0f;
    rej_stable_ = false;
    error_deg_ = 0.0f;
    have_error_ = false;
    integral_degs_ = 0.0f;
    field_ut_ = 0.0f;
    dip_deg_ = 0.0f;
    since_fresh_s_ = 0.0f;
    reacquisitions_ = 0;
    total_correction_deg_ = 0.0f;
}

void MagHeadingLock::begin_acquire() {
    acq_time_s_ = 0.0f;
    acq_hx_ = 0.0f;
    acq_hy_ = 0.0f;
    acq_field_ = 0.0f;
    acq_dip_ = 0.0f;
    acq_n_ = 0;
    have_error_ = false;
}

float MagHeadingLock::update(const Vec3& b, bool fresh, float head_rate_degs, float dt) {
    since_fresh_s_ += dt;
    if (fresh) {
        const float horizontal = std::sqrt(b.x * b.x + b.y * b.y);
        field_ut_ = std::sqrt(horizontal * horizontal + b.z * b.z);
        dip_deg_ = std::atan2(-b.z, horizontal) * kDegPerRad;
        const float heading = std::atan2(b.y, b.x);
        const float sample_dt = since_fresh_s_;
        since_fresh_s_ = 0.0f;
        const bool usable = horizontal >= cfg_.min_horizontal_ut && field_ut_ > 1.0f &&
                            head_rate_degs <= cfg_.max_turn_rate_degs;

        if (state_ == State::acquiring) {
            if (usable) {
                acq_hx_ += b.x / horizontal;
                acq_hy_ += b.y / horizontal;
                acq_field_ += field_ut_;
                acq_dip_ += dip_deg_;
                acq_time_s_ += sample_dt;
                ++acq_n_;
                if (acq_time_s_ >= cfg_.acquire_s && acq_n_ > 0) {
                    ref_heading_rad_ = std::atan2(acq_hy_, acq_hx_);
                    ref_field_ut_ = acq_field_ / static_cast<float>(acq_n_);
                    ref_dip_deg_ = acq_dip_ / static_cast<float>(acq_n_);
                    state_ = State::locked;
                    rejected_s_ = 0.0f;
                    integral_degs_ = 0.0f;
                }
            }
        } else {
            const bool field_ok =
                std::fabs(field_ut_ - ref_field_ut_) <= cfg_.field_tolerance * ref_field_ut_ &&
                std::fabs(dip_deg_ - ref_dip_deg_) <= cfg_.dip_tolerance_deg;
            const float err = wrap_pi(heading - ref_heading_rad_) * kDegPerRad;
            if (usable && field_ok && std::fabs(err) <= cfg_.max_plausible_error_deg) {
                state_ = State::locked;
                rejected_s_ = 0.0f;
                error_deg_ = err;
                have_error_ = true;
                const float k = std::min(1.0f, sample_dt / cfg_.reference_tau_s);
                ref_field_ut_ += (field_ut_ - ref_field_ut_) * k;
                ref_dip_deg_ += (dip_deg_ - ref_dip_deg_) * k;
            } else if (usable) {
                // Disturbed. Hold the loop (no new error, integral frozen) and
                // watch whether the new field is itself stable: a consistent
                // new environment is re-acquired, a moving magnet is not.
                state_ = State::disturbed;
                have_error_ = false;
                const float head_now = heading;
                if (rejected_s_ <= 0.0f) {
                    rej_field_ = field_ut_;
                    rej_dip_ = dip_deg_;
                    rej_heading_ = head_now;
                    rej_stable_ = true;
                } else if (std::fabs(field_ut_ - rej_field_) > cfg_.field_tolerance * 0.5f * rej_field_ ||
                           std::fabs(dip_deg_ - rej_dip_) > cfg_.reacquire_stability_deg ||
                           std::fabs(wrap_pi(head_now - rej_heading_)) * kDegPerRad >
                               cfg_.reacquire_stability_deg * 4.0f) {
                    // Unstable field: restart the stability window.
                    rej_field_ = field_ut_;
                    rej_dip_ = dip_deg_;
                    rej_heading_ = head_now;
                    rejected_s_ = 0.0f;
                }
                rejected_s_ += sample_dt;
                if (rejected_s_ >= cfg_.reacquire_s) {
                    ++reacquisitions_;
                    state_ = State::acquiring;
                    begin_acquire();
                    rejected_s_ = 0.0f;
                }
            } else {
                // Fast rotation or weak horizontal field: skip, keep state.
                have_error_ = false;
            }
        }
    }

    if (state_ != State::locked || !have_error_ || cfg_.observe_only) {
        return 0.0f;
    }
    const float kp = 1.0f / cfg_.tau_s;
    const float ki = cfg_.integral_gain_scale * kp * kp * 0.25f;
    integral_degs_ += ki * error_deg_ * dt;
    integral_degs_ = std::max(-cfg_.integral_limit_degs, std::min(cfg_.integral_limit_degs, integral_degs_));
    float rate = kp * error_deg_ + integral_degs_;
    rate = std::max(-cfg_.max_correction_degs, std::min(cfg_.max_correction_degs, rate));
    const float step_deg = -rate * dt;
    // Apply the correction to our copy of the error too: the next fresh mag
    // sample re-measures it, but between samples the loop must not keep
    // pushing against an error it has already partly removed.
    error_deg_ += step_deg;
    total_correction_deg_ += step_deg;
    return step_deg * kRadPerDeg;
}

}  // namespace gt
