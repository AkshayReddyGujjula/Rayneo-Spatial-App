#include "render/camera.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace DirectX;

namespace {

int failures = 0;

gt::Quat axis_angle(float ax, float ay, float az, float degrees) {
    const float length = std::sqrt(ax * ax + ay * ay + az * az);
    const float half = degrees * 3.14159265358979323846f / 360.0f;
    const float s = std::sin(half) / length;
    return gt::Quat{std::cos(half), ax * s, ay * s, az * s};
}

gt::Euler report(const char* name, const gt::Quat& q, const gt::CameraSigns& signs) {
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
    return applied;
}

void check(bool condition, const char* label) {
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) {
        ++failures;
    }
}

gt::Quat multiply(const gt::Quat& a, const gt::Quat& b) {
    return gt::Quat{a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
                    a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

XMFLOAT3 expected_camera_transform(const gt::Quat& q, const XMFLOAT3& render) {
    // render -> head by B^T, rotate by q, then head -> render by B.
    const gt::Quat vector{0.0f, render.x, render.z, render.y};
    const gt::Quat conjugate{q.w, -q.x, -q.y, -q.z};
    const gt::Quat rotated = multiply(multiply(q, vector), conjugate);
    return XMFLOAT3{rotated.x, rotated.z, rotated.y};
}

void check_exact_basis_transform(const gt::CameraSigns& signs) {
    float maximum_error = 0.0f;
    const XMFLOAT3 input{0.37f, -0.22f, 1.41f};
    for (int i = 0; i < 200; ++i) {
        const float ax = std::sin(static_cast<float>(i) * 0.37f) + 0.2f;
        const float ay = std::cos(static_cast<float>(i) * 0.23f) - 0.1f;
        const float az = std::sin(static_cast<float>(i) * 0.11f + 0.4f);
        const float angle = -170.0f + static_cast<float>(i) * (340.0f / 199.0f);
        const gt::Quat q = axis_angle(ax, ay, az, angle);
        const XMMATRIX camera = XMMatrixInverse(nullptr, gt::camera_view_matrix(q, signs));
        XMFLOAT3 actual;
        XMStoreFloat3(&actual, XMVector3TransformNormal(XMLoadFloat3(&input), camera));
        const XMFLOAT3 expected = expected_camera_transform(q, input);
        const float error = std::sqrt((actual.x - expected.x) * (actual.x - expected.x) +
                                      (actual.y - expected.y) * (actual.y - expected.y) +
                                      (actual.z - expected.z) * (actual.z - expected.z));
        maximum_error = std::max(maximum_error, error);
    }
    std::printf("  maximum combined-orientation basis error: %.8f\n", maximum_error);
    check(maximum_error < 2e-5f, "view matrix is the exact head-to-render basis transform");
}

}  // namespace

int main() {
    const gt::CameraSigns signs;

    const gt::Euler right = report("case 1: head turned RIGHT 40 deg (head -Z)",
                                   axis_angle(0.0f, 0.0f, 1.0f, -40.0f), signs);
    check(std::fabs(right.yaw_deg - 40.0f) < 0.01f && std::fabs(right.pitch_deg) < 0.01f &&
              std::fabs(right.roll_deg) < 0.01f,
          "right turn maps only to renderer yaw");

    const gt::Euler left = report("case 2: head turned LEFT 40 deg (head +Z)",
                                  axis_angle(0.0f, 0.0f, 1.0f, 40.0f), signs);
    check(std::fabs(left.yaw_deg + 40.0f) < 0.01f && std::fabs(left.pitch_deg) < 0.01f &&
              std::fabs(left.roll_deg) < 0.01f,
          "left turn maps only to renderer yaw");

    const gt::Euler down = report("case 3: nod DOWN 30 deg (head -X)",
                                  axis_angle(1.0f, 0.0f, 0.0f, -30.0f), signs);
    check(std::fabs(down.pitch_deg - 30.0f) < 0.01f && std::fabs(down.yaw_deg) < 0.01f &&
              std::fabs(down.roll_deg) < 0.01f,
          "downward nod maps only to renderer pitch");

    const gt::Euler tilt = report("case 4: tilt RIGHT 30 deg (head +Y)",
                                  axis_angle(0.0f, 1.0f, 0.0f, 30.0f), signs);
    check(std::fabs(tilt.roll_deg + 30.0f) < 0.01f && std::fabs(tilt.yaw_deg) < 0.01f &&
              std::fabs(tilt.pitch_deg) < 0.01f,
          "right tilt maps only to renderer roll");

    const gt::Euler up = report("case 5: nod UP 30 deg (head +X)",
                                axis_angle(1.0f, 0.0f, 0.0f, 30.0f), signs);
    check(std::fabs(up.pitch_deg + 30.0f) < 0.01f && std::fabs(up.yaw_deg) < 0.01f &&
              std::fabs(up.roll_deg) < 0.01f,
          "upward nod maps only to renderer pitch");
    check_exact_basis_transform(signs);
    std::printf("camera_selftest: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
