#pragma once

#include "imu/fusion.h"

#include <DirectXMath.h>

namespace gt {

struct CameraSigns {
    float yaw = -1.0f;
    float pitch = 1.0f;
    float roll = -1.0f;
};

// The fused attitude lives in a Z-up earth frame (Madgwick convention), while
// the renderer works in a Y-up frame. Rebuilding the camera rotation in the
// renderer's own convention (yaw about Y, pitch about X, roll about Z) keeps
// head yaw panning the world instead of rolling it.
Euler camera_applied_euler(const Quat& head_relative, const CameraSigns& signs);
DirectX::XMMATRIX camera_view_matrix(const Quat& head_relative, const CameraSigns& signs);

}  // namespace gt
