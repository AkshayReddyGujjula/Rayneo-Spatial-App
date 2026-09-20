#pragma once

#include "imu/gt_protocol.h"

namespace gt {

struct Quat {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Euler {
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
};

Quat quat_conjugate(const Quat& q);
Quat quat_multiply(const Quat& a, const Quat& b);
Quat quat_normalize(const Quat& q);
Quat quat_twist_about(const Quat& q, float axis_x, float axis_y, float axis_z);
// Exact rotation by the given fraction of q's angle (stays a unit rotation for
// any fraction, unlike a linear interpolation).
Quat quat_scaled(const Quat& q, float fraction);
Euler quat_to_euler(const Quat& q);

class MadgwickFilter {
public:
    void reset();
    void set_orientation(const Quat& q) { q_ = quat_normalize(q); }
    void set_beta(float beta) { beta_ = beta; }

    // gyro in rad/s, accel in m/s^2, mag in uT; mag_weight 0 disables the
    // magnetometer correction (6-axis mode).
    void update(const Vec3& gyro_rad_s, const Vec3& accel, const Vec3& mag, float mag_weight, float dt);

    const Quat& orientation() const { return q_; }

private:
    Quat q_;
    float beta_ = 0.05f;
};

}  // namespace gt
