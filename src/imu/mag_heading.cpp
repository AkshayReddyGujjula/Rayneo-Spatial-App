#include "imu/mag_heading.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

// Cyclic Jacobi on a symmetric 3x3; values ascending, vectors[k] is the unit
// eigenvector (column k stored as row k) for values[k].
void symmetric3_eigen(const double a_in[3][3], double values[3], double vectors[3][3]) {
    double a[3][3];
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            a[i][j] = 0.5 * (a_in[i][j] + a_in[j][i]);
        }
    }
    for (int sweep = 0; sweep < 50; ++sweep) {
        const double off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
        if (off < 1e-15) {
            break;
        }
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(a[p][q]) < 1e-18) {
                    continue;
                }
                const double theta = 0.5 * std::atan2(2.0 * a[p][q], a[q][q] - a[p][p]);
                const double c = std::cos(theta);
                const double s = std::sin(theta);
                for (int k = 0; k < 3; ++k) {
                    const double kp = a[k][p];
                    const double kq = a[k][q];
                    a[k][p] = c * kp - s * kq;
                    a[k][q] = s * kp + c * kq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double pk = a[p][k];
                    const double qk = a[q][k];
                    a[p][k] = c * pk - s * qk;
                    a[q][k] = s * pk + c * qk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double kp = v[k][p];
                    const double kq = v[k][q];
                    v[k][p] = c * kp - s * kq;
                    v[k][q] = s * kp + c * kq;
                }
            }
        }
    }
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int x, int y) { return a[x][x] < a[y][y]; });
    for (int k = 0; k < 3; ++k) {
        const int j = order[k];
        values[k] = a[j][j];
        for (int r = 0; r < 3; ++r) {
            vectors[k][r] = v[r][j];
        }
    }
}

float mag_fit_quality(const MagFitResult& fit) {
    if (!std::isfinite(fit.residual_rms_ut) || !std::isfinite(fit.observability)) {
        return 0.0f;
    }
    const float residual = std::max(0.0f, std::min(1.0f, 1.0f - fit.residual_rms_ut / 1.5f));
    const float coverage = std::max(0.0f, std::min(1.0f, fit.observability / 0.15f));
    return residual * coverage;
}

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
    Mat3d rp_sum{};
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
        for (int r = 0; r < 3; ++r) {
            for (int col = 0; col < 3; ++col) {
                rp_sum.m[r][col] += rp.m[r][col];
            }
        }
        ++rows;
    }
    if (rows < 200) {
        result.code = "TOO_FEW_MAG_SAMPLES";
        return result;
    }

    {
        // Observability of the hard-iron vector: I - Mbar^T Mbar.
        double info[3][3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                double mtm = 0.0;
                for (int k = 0; k < 3; ++k) {
                    mtm += (rp_sum.m[k][i] / rows) * (rp_sum.m[k][j] / rows);
                }
                info[i][j] = (i == j ? 1.0 : 0.0) - mtm;
            }
        }
        double values[3];
        double vectors[3][3];
        symmetric3_eigen(info, values, vectors);
        result.observability = static_cast<float>(std::max(0.0, values[0]));
        const Vec3 weak_raw{static_cast<float>(vectors[0][0]), static_cast<float>(vectors[0][1]),
                            static_cast<float>(vectors[0][2])};
        result.weak_axis_package = mat_apply(kMagToPackage, weak_raw);
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

// ---------------------------------------------------------------------------
// MagHardIronTracker

void MagHardIronTracker::configure(const Config& cfg, const Vec3& calibrated_hard_iron_ut) {
    cfg_ = cfg;
    calibration_ = calibrated_hard_iron_ut;
    reset();
}

void MagHardIronTracker::reset() {
    hard_iron_ = calibration_;
    have_tick_ = false;
    have_mag_ = false;
    have_estimates_ = false;
    step_armed_ = false;
    step_time_s_ = 0.0;
    last_step_ = Vec3{};
    last_scale_ = 1.0f;
    have_hypothesis_ = false;
    hypothesis_ = Vec3{};
    hypothesis_age_s_ = 0.0;
    evidence_sep_ = 0.0;
    evidence_gain_ = 0.0;
    reset_windows();
    steps_ = 0;
    transients_ = 0;
    foreign_steps_ = 0;
    adopted_ = 0;
    rejected_ = 0;
    windows_used_ = 0;
}

void MagHardIronTracker::reset_windows() {
    for (Window& w : windows_) {
        w = Window{};
    }
    since_start_s_ = cfg_.hop_s;  // start one on the next sample
}

Vec3 MagHardIronTracker::offset_from_calibration_ut() const {
    return Vec3{hard_iron_.x - calibration_.x, hard_iron_.y - calibration_.y, hard_iron_.z - calibration_.z};
}

float MagHardIronTracker::hypothesis_scale() const {
    // Least-squares scale along the step, shrunk toward the step as measured
    // (scale 1) by a prior worth scale_prior_ut2 of separation.
    const double a = 1.0 + evidence_gain_ / (evidence_sep_ + static_cast<double>(cfg_.scale_prior_ut2));
    return static_cast<float>(std::max(0.0, std::min(static_cast<double>(cfg_.max_scale), a)));
}

MagHardIronTracker::Event MagHardIronTracker::add_sample(const ImuSample& s, const Vec3& gyro_bias_degs,
                                                         float reference_field_ut) {
    if (!cfg_.enabled) {
        return Event::none;
    }
    double dt = 0.0;
    if (have_tick_) {
        dt = static_cast<double>(s.tick_100us - last_tick_) * 1e-4;
        if (dt <= 0.0 || dt >= 0.05) {
            // A gap breaks the rotation compensation and the window attitude:
            // restart both (a pending hypothesis keeps its evidence).
            have_estimates_ = false;
            step_armed_ = false;
            reset_windows();
            dt = 0.0;
        }
    }
    have_tick_ = true;
    last_tick_ = s.tick_100us;

    const bool finite = std::isfinite(s.mag_ut.x) && std::isfinite(s.mag_ut.y) && std::isfinite(s.mag_ut.z);
    const bool fresh = finite && (!have_mag_ || s.mag_ut.x != last_mag_.x || s.mag_ut.y != last_mag_.y ||
                                  s.mag_ut.z != last_mag_.z);
    if (finite) {
        last_mag_ = s.mag_ut;
        have_mag_ = true;
    }

    const double wx = static_cast<double>(s.gyro_degs.x - gyro_bias_degs.x);
    const double wy = static_cast<double>(s.gyro_degs.y - gyro_bias_degs.y);
    const double wz = static_cast<double>(s.gyro_degs.z - gyro_bias_degs.z);
    const double rate = std::sqrt(wx * wx + wy * wy + wz * wz);
    const bool slow_head = rate <= cfg_.max_rate_degs;
    const Mat3d step = rotvec_to_mat(wx * kRadPerDeg * dt, wy * kRadPerDeg * dt, wz * kRadPerDeg * dt);
    const double centred[3] = {s.mag_ut.x - hard_iron_.x, s.mag_ut.y - hard_iron_.y, s.mag_ut.z - hard_iron_.z};
    // Package-frame field, P (m - c).
    double mp[3];
    for (int i = 0; i < 3; ++i) {
        mp[i] = kMagToPackage[static_cast<size_t>(i * 3)] * centred[0] +
                kMagToPackage[static_cast<size_t>(i * 3 + 1)] * centred[1] +
                kMagToPackage[static_cast<size_t>(i * 3 + 2)] * centred[2];
    }

    since_fresh_s_ += dt;
    Event event = Event::none;

    // --- Step detector -----------------------------------------------------
    if (have_estimates_ && dt > 0.0) {
        // An earth-fixed vector seen from a frame that rotated by `step`:
        // f <- step^T f.
        double f[3];
        double g[3];
        for (int i = 0; i < 3; ++i) {
            f[i] = step.m[0][i] * fast_[0] + step.m[1][i] * fast_[1] + step.m[2][i] * fast_[2];
            g[i] = step.m[0][i] * slow_[0] + step.m[1][i] * slow_[1] + step.m[2][i] * slow_[2];
        }
        for (int i = 0; i < 3; ++i) {
            fast_[i] = f[i];
            slow_[i] = g[i];
        }
    }
    if (fresh) {
        const double interval = since_fresh_s_;
        since_fresh_s_ = 0.0;
        if (!have_estimates_) {
            for (int i = 0; i < 3; ++i) {
                fast_[i] = mp[i];
                slow_[i] = mp[i];
            }
            have_estimates_ = true;
        } else if (slow_head) {
            // Gains from the actual interval since the previous reading, so
            // the time constants hold at any magnetometer rate.
            const double kf = 1.0 - std::exp(-interval / static_cast<double>(cfg_.fast_tau_s));
            for (int i = 0; i < 3; ++i) {
                fast_[i] += (mp[i] - fast_[i]) * kf;
            }
            if (!step_armed_) {
                const double ks = 1.0 - std::exp(-interval / static_cast<double>(cfg_.slow_tau_s));
                for (int i = 0; i < 3; ++i) {
                    slow_[i] += (mp[i] - slow_[i]) * ks;
                }
            }
        }
    }
    if (have_estimates_) {
        const double j[3] = {fast_[0] - slow_[0], fast_[1] - slow_[1], fast_[2] - slow_[2]};
        const double jn = std::sqrt(j[0] * j[0] + j[1] * j[1] + j[2] * j[2]);
        if (!step_armed_) {
            if (jn > cfg_.step_watch_ut && slow_head) {
                step_armed_ = true;
                step_time_s_ = 0.0;
                armed_total_s_ = 0.0;
                for (int i = 0; i < 3; ++i) {
                    step_ref_[i] = j[i];
                }
            }
        } else if (jn < cfg_.step_off_ut) {
            // Fell back before confirming: a transient. The slow estimate was
            // frozen, so it never absorbed it.
            step_armed_ = false;
            ++transients_;
        } else if ((armed_total_s_ += dt) > cfg_.max_armed_s) {
            // Never settled into a stable step: a field that keeps changing.
            // Hand it back to the lock's gates and follow it here.
            step_armed_ = false;
            ++foreign_steps_;
            for (int i = 0; i < 3; ++i) {
                slow_[i] = fast_[i];
            }
        } else {
            const double dx = j[0] - step_ref_[0];
            const double dy = j[1] - step_ref_[1];
            const double dz = j[2] - step_ref_[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > cfg_.step_stability_ut) {
                // Still ramping (or moving): restart the confirmation.
                for (int i = 0; i < 3; ++i) {
                    step_ref_[i] = j[i];
                }
                step_time_s_ = 0.0;
            } else if (slow_head) {
                step_time_s_ += dt;
            }
            if (step_time_s_ >= cfg_.step_confirm_s && jn < cfg_.step_on_ut) {
                // Settled, but below the step threshold (the remains of a
                // transient, or a slow drift): absorb it, it is not a step.
                step_armed_ = false;
                ++transients_;
                for (int i = 0; i < 3; ++i) {
                    slow_[i] = fast_[i];
                }
            } else if (step_time_s_ >= cfg_.step_confirm_s) {
                // Confirmed step. In raw magnetometer axes (P is a rotation,
                // so its inverse is its transpose).
                const Vec3 h{static_cast<float>(kMagToPackage[0] * j[0] + kMagToPackage[3] * j[1] +
                                                kMagToPackage[6] * j[2]),
                             static_cast<float>(kMagToPackage[1] * j[0] + kMagToPackage[4] * j[1] +
                                                kMagToPackage[7] * j[2]),
                             static_cast<float>(kMagToPackage[2] * j[0] + kMagToPackage[5] * j[1] +
                                                kMagToPackage[8] * j[2])};
                last_step_ = h;
                step_armed_ = false;
                const float hn = std::sqrt(h.x * h.x + h.y * h.y + h.z * h.z);
                const double before = std::sqrt(slow_[0] * slow_[0] + slow_[1] * slow_[1] + slow_[2] * slow_[2]);
                const double after = std::sqrt(fast_[0] * fast_[0] + fast_[1] * fast_[1] + fast_[2] * fast_[2]);
                const double ref = reference_field_ut;
                const Vec3 next{hard_iron_.x + h.x, hard_iron_.y + h.y, hard_iron_.z + h.z};
                const float ox = next.x - calibration_.x;
                const float oy = next.y - calibration_.y;
                const float oz = next.z - calibration_.z;
                const bool plausible =
                    hn <= cfg_.max_step_ut && std::sqrt(ox * ox + oy * oy + oz * oz) <= cfg_.max_offset_ut &&
                    (ref <= 0.0 || (std::fabs(before - ref) <= cfg_.pre_step_field_tolerance * ref &&
                                    std::fabs(after - ref) <= cfg_.post_step_field_tolerance * ref));
                if (!plausible) {
                    // Too large, or into/out of a foreign field: not an
                    // earbud-sized head-fixed change. The lock's own
                    // disturbance gates own it; follow it here so it is not
                    // re-detected.
                    ++foreign_steps_;
                    for (int i = 0; i < 3; ++i) {
                        slow_[i] = fast_[i];
                    }
                } else {
                    ++steps_;
                    // Provisionally head-fixed: fold it into the offset now, so
                    // the field the lock sees (and its heading) is continuous
                    // across the step. In the new offset's frame the field
                    // estimates are the pre-step value.
                    hard_iron_ = next;
                    for (int i = 0; i < 3; ++i) {
                        fast_[i] = slow_[i];
                    }
                    // Compose with a pending hypothesis (an earbud out and back
                    // in again cancels).
                    Vec3 combined = h;
                    if (have_hypothesis_) {
                        combined = Vec3{hypothesis_.x + h.x, hypothesis_.y + h.y, hypothesis_.z + h.z};
                    }
                    const float cn =
                        std::sqrt(combined.x * combined.x + combined.y * combined.y + combined.z * combined.z);
                    hypothesis_ = combined;
                    have_hypothesis_ = true;
                    hypothesis_age_s_ = 0.0;
                    evidence_sep_ = 0.0;
                    evidence_gain_ = 0.0;
                    reset_windows();
                    if (cn < cfg_.step_off_ut) {
                        // Out and back in again (or a field that came and
                        // went): restore the offset exactly as it was.
                        revert_hypothesis();
                    }
                    event = Event::step;
                }
            }
        }
    }

    // --- Hypothesis test windows ----------------------------------------------
    if (!have_hypothesis_) {
        return event;
    }
    hypothesis_age_s_ += dt;
    const bool use_row = fresh && slow_head;
    const double h[3] = {hypothesis_.x, hypothesis_.y, hypothesis_.z};
    for (Window& w : windows_) {
        if (!w.active) {
            continue;
        }
        if (dt > 0.0) {
            Mat3d att{};
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    att.m[i][k] = w.att[i][k];
                }
            }
            att = mat_mul(att, step);
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    w.att[i][k] = att.m[i][k];
                }
            }
            w.elapsed_s += dt;
        }
        if (use_row) {
            // M = att * P, y = M (m - c).
            double m[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    m[i][k] = w.att[i][0] * kMagToPackage[static_cast<size_t>(k)] +
                              w.att[i][1] * kMagToPackage[static_cast<size_t>(3 + k)] +
                              w.att[i][2] * kMagToPackage[static_cast<size_t>(6 + k)];
                }
            }
            double y[3];
            for (int i = 0; i < 3; ++i) {
                y[i] = m[i][0] * centred[0] + m[i][1] * centred[1] + m[i][2] * centred[2];
            }
            for (int i = 0; i < 3; ++i) {
                for (int k = 0; k < 3; ++k) {
                    w.m_sum[i][k] += m[i][k];
                }
                w.mty[i] += m[0][i] * y[0] + m[1][i] * y[1] + m[2][i] * y[2];
                w.y_sum[i] += y[i];
                w.yy += y[i] * y[i];
            }
            ++w.rows;
        }
        if (w.elapsed_s >= cfg_.window_s) {
            const Window finished = w;
            w.active = false;
            close_window(finished, reference_field_ut);
        }
    }
    (void)h;
    if (event == Event::none) {
        event = decide();
    }
    if (!have_hypothesis_) {
        return event;
    }
    since_start_s_ += dt;
    if (since_start_s_ >= cfg_.hop_s) {
        for (Window& w : windows_) {
            if (!w.active) {
                w = Window{};
                w.active = true;
                since_start_s_ = 0.0;
                break;
            }
        }
    }
    return event;
}

void MagHardIronTracker::close_window(const Window& w, float reference_field_ut) {
    const double min_rows = static_cast<double>(cfg_.min_fill) * static_cast<double>(cfg_.window_s) * 50.0;
    if (w.rows < 3 || static_cast<double>(w.rows) < min_rows) {
        return;
    }
    const double n = static_cast<double>(w.rows);
    double mbar[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) {
            mbar[i][k] = w.m_sum[i][k] / n;
        }
    }
    const double h[3] = {hypothesis_.x, hypothesis_.y, hypothesis_.z};
    const double ybar[3] = {w.y_sum[0] / n, w.y_sum[1] / n, w.y_sum[2] / n};
    // g = sum (M_t - Mbar)^T (y_t - ybar) = mty - n Mbar^T ybar.
    double g[3];
    for (int i = 0; i < 3; ++i) {
        g[i] = w.mty[i] - n * (mbar[0][i] * ybar[0] + mbar[1][i] * ybar[1] + mbar[2][i] * ybar[2]);
    }
    // h^T S h with S = I - Mbar^T Mbar: |h|^2 - |Mbar h|^2.
    double mh[3];
    for (int i = 0; i < 3; ++i) {
        mh[i] = mbar[i][0] * h[0] + mbar[i][1] * h[1] + mbar[i][2] * h[2];
    }
    const double sep = std::max(0.0, h[0] * h[0] + h[1] * h[1] + h[2] * h[2] -
                                         (mh[0] * mh[0] + mh[1] * mh[1] + mh[2] * mh[2]));
    const double hg = (h[0] * g[0] + h[1] * g[1] + h[2] * g[2]) / n;
    const double var0 = std::max(0.0, w.yy / n - (ybar[0] * ybar[0] + ybar[1] * ybar[1] + ybar[2] * ybar[2]));
    // The offset already includes h (provisional). The alternative is
    // reverting it, i.e. a change of -h: residual var0 + 2 hg + sep.
    const double var_revert = std::max(0.0, var0 + 2.0 * hg + sep);
    // A field that was not constant under either hypothesis (a moving
    // magnet) carries no information about c.
    if (std::sqrt(std::min(var0, var_revert) / 3.0) > cfg_.max_residual_ut) {
        return;
    }
    if (reference_field_ut > 0.0f) {
        const double b0 = std::sqrt(ybar[0] * ybar[0] + ybar[1] * ybar[1] + ybar[2] * ybar[2]);
        const double br[3] = {ybar[0] + mh[0], ybar[1] + mh[1], ybar[2] + mh[2]};
        const double brn = std::sqrt(br[0] * br[0] + br[1] * br[1] + br[2] * br[2]);
        const double tol = static_cast<double>(cfg_.field_tolerance) * reference_field_ut;
        if (std::fabs(b0 - reference_field_ut) > tol && std::fabs(brn - reference_field_ut) > tol) {
            return;
        }
    }
    ++windows_used_;
    // One degree of freedom: the offset c_before + a h. Relative to the
    // current (a = 1) the residual is var0 - 2 (a - 1) hg + (a - 1)^2 sep, so
    // the least-squares a is 1 + sum(hg) / sum(sep): 1 when the step was
    // head-fixed and measured exactly, 0 when it was earth-fixed.
    evidence_sep_ += sep;
    evidence_gain_ += hg;
}

void MagHardIronTracker::revert_hypothesis() {
    hard_iron_ = Vec3{hard_iron_.x - hypothesis_.x, hard_iron_.y - hypothesis_.y, hard_iron_.z - hypothesis_.z};
    // The package-frame field estimates move by +P h in the restored frame.
    const double h[3] = {hypothesis_.x, hypothesis_.y, hypothesis_.z};
    for (int i = 0; i < 3; ++i) {
        const double ph = kMagToPackage[static_cast<size_t>(i * 3)] * h[0] +
                          kMagToPackage[static_cast<size_t>(i * 3 + 1)] * h[1] +
                          kMagToPackage[static_cast<size_t>(i * 3 + 2)] * h[2];
        fast_[i] += ph;
        slow_[i] += ph;
    }
    have_hypothesis_ = false;
    reset_windows();
}

MagHardIronTracker::Event MagHardIronTracker::decide() {
    if (!have_hypothesis_) {
        return Event::none;
    }
    if (evidence_sep_ >= cfg_.decide_separation_ut2) {
        const float a = hypothesis_scale();
        last_scale_ = a;
        if (a < cfg_.revert_below_scale) {
            // Earth-fixed: back to the previous offset; the lock's own gates
            // treat the change as they always did.
            revert_hypothesis();
            ++rejected_;
            return Event::rejected;
        }
        // Head-fixed: refine the provisional offset to c_before + a h.
        const double k = static_cast<double>(a) - 1.0;
        const double adj[3] = {k * hypothesis_.x, k * hypothesis_.y, k * hypothesis_.z};
        hard_iron_ = Vec3{hard_iron_.x + static_cast<float>(adj[0]), hard_iron_.y + static_cast<float>(adj[1]),
                          hard_iron_.z + static_cast<float>(adj[2])};
        for (int i = 0; i < 3; ++i) {
            const double pa = kMagToPackage[static_cast<size_t>(i * 3)] * adj[0] +
                              kMagToPackage[static_cast<size_t>(i * 3 + 1)] * adj[1] +
                              kMagToPackage[static_cast<size_t>(i * 3 + 2)] * adj[2];
            fast_[i] -= pa;
            slow_[i] -= pa;
        }
        have_hypothesis_ = false;
        reset_windows();
        ++adopted_;
        return Event::adopted;
    }
    if (hypothesis_age_s_ >= cfg_.hypothesis_timeout_s) {
        // Never enough rotation to tell: keep the step as measured (with
        // so little rotation the choice barely affects the heading).
        have_hypothesis_ = false;
        reset_windows();
        return Event::expired;
    }
    return Event::none;
}

// ---------------------------------------------------------------------------
// MagHeadingLock

const char* mag_state_name(MagHeadingLock::State state) {
    switch (state) {
        case MagHeadingLock::State::off:
            return "off";
        case MagHeadingLock::State::acquiring:
            return "acquiring";
        case MagHeadingLock::State::locked:
            return "locked";
        case MagHeadingLock::State::disturbed:
            return "disturbed";
        case MagHeadingLock::State::relearning:
            return "relearning";
    }
    return "off";
}

Vec3 MagHeadingLock::track_hard_iron(const ImuSample& sample, const Vec3& gyro_bias_degs,
                                     const MagCalibration& calibration) {
    if (!cfg_.hard_iron.enabled) {
        return calibration.hard_iron_ut;
    }
    if (!tracker_ready_) {
        tracker_.configure(cfg_.hard_iron, calibration.hard_iron_ut);
        tracker_ready_ = true;
    }
    // |B| reference: the calibration's (measured over a full turn), else the
    // lock's once it has one.
    const float ref_field = calibration.field_ut > 1.0f ? calibration.field_ut
                                                        : (ref_field_ut_ > 1.0f ? ref_field_ut_ : 0.0f);
    // A confirmed step is folded into the offset at once, so the heading the
    // lock sees stays continuous and its reference stays valid: the lock is
    // never re-referenced from here. (Re-referencing when a step is reverted
    // as earth-fixed was tried: it removes the pull of a magnet put down on
    // the desk in simulation, 5.9 -> 1.1 deg, but on the recorded session the
    // ambiguous real events made the worst injected case 7.6 -> 11.0 deg.)
    if (tracker_.add_sample(sample, gyro_bias_degs, ref_field) == MagHardIronTracker::Event::step) {
        ++rebases_;
    }
    return tracker_.hard_iron_ut();
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
    tracker_ready_ = false;
    rebases_ = 0;
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

    // While the hard-iron tracker confirms a field step the field is suspect:
    // measure, but apply nothing (a few seconds of gyro-only yaw).
    const bool suspect = cfg_.hard_iron.enabled && tracker_.step_pending();
    if (state_ != State::locked || !have_error_ || cfg_.observe_only || suspect) {
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
