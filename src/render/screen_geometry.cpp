#include "render/screen_geometry.h"

#include <cmath>

namespace gt {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Vector3 {
    float x;
    float y;
    float z;
};

Vector3 add(const Vector3& a, const Vector3& b) {
    return Vector3{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vector3 scale(const Vector3& value, float amount) {
    return Vector3{value.x * amount, value.y * amount, value.z * amount};
}

}  // namespace

std::array<ScreenGeometryVertex, 6> make_screen_quad(const ScreenLayout& screen) {
    const float yaw = screen.yaw_deg * kPi / 180.0f;
    const float pitch = screen.pitch_deg * kPi / 180.0f;
    const float roll = screen.roll_deg * kPi / 180.0f;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cr = std::cos(roll);
    const float sr = std::sin(roll);

    const Vector3 forward{sy * cp, sp, cy * cp};
    const Vector3 base_right{cy, 0.0f, -sy};
    const Vector3 base_up{-sy * sp, cp, -cy * sp};
    const Vector3 right = add(scale(base_right, cr), scale(base_up, sr));
    const Vector3 up = add(scale(base_up, cr), scale(base_right, -sr));
    const Vector3 centre = scale(forward, screen.distance_m);
    const Vector3 horizontal = scale(right, screen.width_m * 0.5f);
    const Vector3 vertical = scale(up, screen.height_m * 0.5f);

    const Vector3 bottom_left = add(add(centre, scale(horizontal, -1.0f)), scale(vertical, -1.0f));
    const Vector3 bottom_right = add(add(centre, horizontal), scale(vertical, -1.0f));
    const Vector3 top_right = add(add(centre, horizontal), vertical);
    const Vector3 top_left = add(add(centre, scale(horizontal, -1.0f)), vertical);

    const auto vertex = [](const Vector3& point, float u, float v) {
        return ScreenGeometryVertex{point.x, point.y, point.z, u, v};
    };
    return {
        vertex(bottom_left, 0.0f, 1.0f), vertex(bottom_right, 1.0f, 1.0f),
        vertex(top_right, 1.0f, 0.0f), vertex(bottom_left, 0.0f, 1.0f),
        vertex(top_right, 1.0f, 0.0f), vertex(top_left, 0.0f, 0.0f),
    };
}

}  // namespace gt
