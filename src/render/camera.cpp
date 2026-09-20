#include "render/camera.h"

namespace gt {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float to_radians(float degrees) {
    return degrees * kPi / 180.0f;
}

}  // namespace

Euler camera_applied_euler(const Quat& head_relative, const CameraSigns& signs) {
    Euler e = quat_to_euler(head_relative);
    e.yaw_deg *= signs.yaw;
    e.pitch_deg *= signs.pitch;
    e.roll_deg *= signs.roll;
    return e;
}

DirectX::XMMATRIX camera_view_matrix(const Quat& head_relative, const CameraSigns& signs) {
    const Euler e = camera_applied_euler(head_relative, signs);
    const DirectX::XMMATRIX camera = DirectX::XMMatrixRotationRollPitchYaw(
        to_radians(e.pitch_deg), to_radians(e.yaw_deg), to_radians(e.roll_deg));
    return DirectX::XMMatrixInverse(nullptr, camera);
}

}  // namespace gt
