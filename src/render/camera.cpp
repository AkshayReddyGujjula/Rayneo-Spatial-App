#include "render/camera.h"

namespace gt {
Euler camera_applied_euler(const Quat& head_relative, const CameraSigns& signs) {
    const Euler e = quat_to_euler(head_relative);
    Euler applied;
    applied.yaw_deg = e.yaw_deg * signs.yaw;
    applied.pitch_deg = e.roll_deg * signs.pitch;
    applied.roll_deg = e.pitch_deg * signs.roll;
    return applied;
}

DirectX::XMMATRIX camera_view_matrix(const Quat& head_relative, const CameraSigns& signs) {
    // Head axes are X=right, Y=forward, Z=up. Render axes are X=right,
    // Y=up, Z=forward. That basis swap is a reflection, so axial quaternion
    // components transform as det(B)*B*v: (x,y,z) -> (-x,-z,-y).
    const DirectX::XMVECTOR render_quaternion = DirectX::XMQuaternionNormalize(
        DirectX::XMVectorSet(signs.pitch * head_relative.x, signs.yaw * head_relative.z,
                             signs.roll * head_relative.y, head_relative.w));
    const DirectX::XMMATRIX camera = DirectX::XMMatrixRotationQuaternion(render_quaternion);
    return DirectX::XMMatrixInverse(nullptr, camera);
}

}  // namespace gt
