#pragma once

#include "imu/fusion.h"

#include <DirectXMath.h>

namespace gt {

struct CameraSigns {
    float yaw = -1.0f;
    float pitch = -1.0f;
    float roll = -1.0f;
};

// The fused attitude lives in a Z-up earth frame (Madgwick convention), while
// the renderer works in a Y-up frame. Rebuilding the camera rotation in the
// calibrated head frame is X=right, Y=forward, Z=up. Quaternion roll therefore
// means anatomical nod, and quaternion pitch means anatomical head tilt.
Euler camera_applied_euler(const Quat& head_relative, const CameraSigns& signs);
DirectX::XMMATRIX camera_view_matrix(const Quat& head_relative, const CameraSigns& signs);

}  // namespace gt
