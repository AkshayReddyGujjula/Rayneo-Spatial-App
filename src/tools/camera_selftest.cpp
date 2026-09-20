#include "render/camera.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdio>

using namespace DirectX;

namespace {

gt::Quat axis_angle(float ax, float ay, float az, float degrees) {
    const float length = std::sqrt(ax * ax + ay * ay + az * az);
    const float half = degrees * 3.14159265358979323846f / 360.0f;
    const float s = std::sin(half) / length;
    return gt::Quat{std::cos(half), ax * s, ay * s, az * s};
}

void report(const char* name, const gt::Quat& q, const gt::CameraSigns& signs) {
    const gt::Euler applied = gt::camera_applied_euler(q, signs);
    const XMMATRIX view = gt::camera_view_matrix(q, signs);
    std::printf("%s\n", name);
    std::printf("  applied yaw/pitch/roll: %+7.2f %+7.2f %+7.2f\n", applied.yaw_deg, applied.pitch_deg,
                applied.roll_deg);
    const XMFLOAT3 points[3] = {{0.0f, 0.0f, 2.0f}, {2.0f, 0.0f, 2.0f}, {0.0f, 2.0f, 2.0f}};
    const char* labels[3] = {"ahead      ", "ahead-right", "ahead-up   "};
    for (int i = 0; i < 3; ++i) {
        XMVECTOR v = XMVectorSet(points[i].x, points[i].y, points[i].z, 0.0f);
        v = XMVector3TransformNormal(v, view);
        XMFLOAT3 out;
        XMStoreFloat3(&out, v);
        std::printf("  %s world(%4.0f,%4.0f,%4.0f) -> view(%+6.2f, %+6.2f, %+6.2f)\n", labels[i],
                    points[i].x, points[i].y, points[i].z, out.x, out.y, out.z);
    }
}

}  // namespace

int main() {
    const gt::CameraSigns signs;

    report("case 1: head turned RIGHT 40 deg (rotation about earth up, -40)",
           axis_angle(0.0f, 0.0f, 1.0f, -40.0f), signs);
    report("case 2: head turned LEFT 40 deg (rotation about earth up, +40)",
           axis_angle(0.0f, 0.0f, 1.0f, 40.0f), signs);
    report("case 3: nod DOWN 30 deg (rotation about earth left axis, +30)",
           axis_angle(0.0f, 1.0f, 0.0f, 30.0f), signs);
    report("case 4: tilt RIGHT 30 deg (rotation about earth forward axis, +30)",
           axis_angle(1.0f, 0.0f, 0.0f, 30.0f), signs);
    report("case 5: nod UP 30 deg (rotation about earth left axis, -30)",
           axis_angle(0.0f, 1.0f, 0.0f, -30.0f), signs);
    return 0;
}
