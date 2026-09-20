#include "imu/fusion.h"

#include <cmath>

namespace gt {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegPerRad = 180.0f / kPi;

}  // namespace

Quat quat_conjugate(const Quat& q) {
    return Quat{q.w, -q.x, -q.y, -q.z};
}

Quat quat_multiply(const Quat& a, const Quat& b) {
    return Quat{
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

Quat quat_normalize(const Quat& q) {
    const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n < 1e-6f) {
        return Quat{};
    }
    return Quat{q.w / n, q.x / n, q.y / n, q.z / n};
}

Quat quat_twist_about(const Quat& q, float axis_x, float axis_y, float axis_z) {
    // Swing/twist decomposition: the part of the rotation about the given axis.
    const float projection = q.x * axis_x + q.y * axis_y + q.z * axis_z;
    Quat twist{q.w, axis_x * projection, axis_y * projection, axis_z * projection};
    const float n = std::sqrt(twist.x * twist.x + twist.y * twist.y + twist.z * twist.z +
                              twist.w * twist.w);
    if (n < 1e-6f) {
        return Quat{};
    }
    twist.x /= n;
    twist.y /= n;
    twist.z /= n;
    twist.w /= n;
    if (twist.w < 0.0f) {
        twist.w = -twist.w;
        twist.x = -twist.x;
        twist.y = -twist.y;
        twist.z = -twist.z;
    }
    return twist;
}

Euler quat_to_euler(const Quat& q) {
    const Quat n = quat_normalize(q);
    const float w = n.w;
    const float x = n.x;
    const float y = n.y;
    const float z = n.z;

    float sin_pitch = 2.0f * (w * y - z * x);
    sin_pitch = sin_pitch > 1.0f ? 1.0f : (sin_pitch < -1.0f ? -1.0f : sin_pitch);

    Euler e;
    e.roll_deg = std::atan2(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y)) * kDegPerRad;
    e.pitch_deg = std::asin(sin_pitch) * kDegPerRad;
    e.yaw_deg = std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z)) * kDegPerRad;
    return e;
}

void MadgwickFilter::reset() {
    q_ = Quat{};
}

void MadgwickFilter::update(const Vec3& gyro, const Vec3& accel, const Vec3& mag, float mag_weight,
                            float dt) {
    float q0 = q_.w;
    float q1 = q_.x;
    float q2 = q_.y;
    float q3 = q_.z;

    float ax = accel.x;
    float ay = accel.y;
    float az = accel.z;
    const float a_norm = std::sqrt(ax * ax + ay * ay + az * az);
    if (a_norm > 1e-4f) {
        ax /= a_norm;
        ay /= a_norm;
        az /= a_norm;
    } else {
        ax = 0.0f;
        ay = 0.0f;
        az = 1.0f;
    }

    bool use_mag = mag_weight > 0.0f;
    float mx = 0.0f;
    float my = 0.0f;
    float mz = 0.0f;
    float bx = 0.0f;
    float bz = 0.0f;
    if (use_mag) {
        const float m_norm = std::sqrt(mag.x * mag.x + mag.y * mag.y + mag.z * mag.z);
        if (m_norm > 1e-4f) {
            mx = mag.x / m_norm;
            my = mag.y / m_norm;
            mz = mag.z / m_norm;
        } else {
            use_mag = false;
        }
    }
    if (use_mag) {
        const float hx = 2.0f * (mx * (0.5f - q2 * q2 - q3 * q3) + my * (q1 * q2 - q0 * q3) +
                                 mz * (q1 * q3 + q0 * q2));
        const float hy = 2.0f * (mx * (q1 * q2 + q0 * q3) + my * (0.5f - q1 * q1 - q3 * q3) +
                                 mz * (q2 * q3 - q0 * q1));
        const float hz = 2.0f * (mx * (q1 * q3 - q0 * q2) + my * (q2 * q3 + q0 * q1) +
                                 mz * (0.5f - q1 * q1 - q2 * q2));
        bx = std::sqrt(hx * hx + hy * hy);
        bz = hz;
    }

    const float f0 = 2.0f * (q1 * q3 - q0 * q2) - ax;
    const float f1 = 2.0f * (q0 * q1 + q2 * q3) - ay;
    const float f2 = 2.0f * (0.5f - q1 * q1 - q2 * q2) - az;

    float grad0 = -2.0f * q2 * f0 + 2.0f * q1 * f1;
    float grad1 = 2.0f * q3 * f0 + 2.0f * q0 * f1 - 4.0f * q1 * f2;
    float grad2 = -2.0f * q0 * f0 + 2.0f * q3 * f1 - 4.0f * q2 * f2;
    float grad3 = 2.0f * q1 * f0 + 2.0f * q2 * f1;

    if (use_mag) {
        const float f3 = 2.0f * bx * (0.5f - q2 * q2 - q3 * q3) + 2.0f * bz * (q1 * q3 - q0 * q2) - mx;
        const float f4 = 2.0f * bx * (q1 * q2 - q0 * q3) + 2.0f * bz * (q0 * q1 + q2 * q3) - my;
        const float f5 = 2.0f * bx * (q0 * q2 + q1 * q3) + 2.0f * bz * (0.5f - q1 * q1 - q2 * q2) - mz;

        const float gm0 = -2.0f * bz * q2 * f3 + (-2.0f * bx * q3 + 2.0f * bz * q1) * f4 + 2.0f * bx * q2 * f5;
        const float gm1 = 2.0f * bz * q3 * f3 + (2.0f * bx * q2 + 2.0f * bz * q0) * f4 +
                          (2.0f * bx * q3 - 4.0f * bz * q1) * f5;
        const float gm2 = (-4.0f * bx * q2 - 2.0f * bz * q0) * f3 + (2.0f * bx * q1 + 2.0f * bz * q3) * f4 +
                          (2.0f * bx * q0 - 4.0f * bz * q2) * f5;
        const float gm3 = (-4.0f * bx * q3 + 2.0f * bz * q1) * f3 + (-2.0f * bx * q0 + 2.0f * bz * q2) * f4 +
                          2.0f * bx * q1 * f5;

        grad0 += mag_weight * gm0;
        grad1 += mag_weight * gm1;
        grad2 += mag_weight * gm2;
        grad3 += mag_weight * gm3;
    }

    const float g_norm = std::sqrt(grad0 * grad0 + grad1 * grad1 + grad2 * grad2 + grad3 * grad3);
    if (g_norm > 1e-6f) {
        grad0 /= g_norm;
        grad1 /= g_norm;
        grad2 /= g_norm;
        grad3 /= g_norm;
    }

    const float qd0 = 0.5f * (-q1 * gyro.x - q2 * gyro.y - q3 * gyro.z) - beta_ * grad0;
    const float qd1 = 0.5f * (q0 * gyro.x + q2 * gyro.z - q3 * gyro.y) - beta_ * grad1;
    const float qd2 = 0.5f * (q0 * gyro.y - q1 * gyro.z + q3 * gyro.x) - beta_ * grad2;
    const float qd3 = 0.5f * (q0 * gyro.z + q1 * gyro.y - q2 * gyro.x) - beta_ * grad3;

    q_.w = q0 + qd0 * dt;
    q_.x = q1 + qd1 * dt;
    q_.y = q2 + qd2 * dt;
    q_.z = q3 + qd3 * dt;
    q_ = quat_normalize(q_);
}

}  // namespace gt
