#pragma once

// Orbiting perspective math for the controller's 3D arrangement view.
// Header-only and dependency-free (pure float math + win32 RECT/POINT) so the
// dashboard and the selftest share it. World frame: X = right, Y = forward,
// Z = up, metres, with the head at the origin.

#include "layout/layout.h"

#include <windows.h>

#include <cmath>
#include <vector>

namespace gt {

struct OrbitVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline OrbitVec3 orbit_sub(OrbitVec3 a, OrbitVec3 b) {
    return OrbitVec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline float orbit_dot(OrbitVec3 a, OrbitVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline OrbitVec3 orbit_cross(OrbitVec3 a, OrbitVec3 b) {
    return OrbitVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline OrbitVec3 orbit_norm(OrbitVec3 v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length < 1e-6f) {
        return OrbitVec3{0.0f, 0.0f, 1.0f};
    }
    return OrbitVec3{v.x / length, v.y / length, v.z / length};
}

struct OrbitView {
    float yaw_deg = -30.0f;    // orbit around the target; 0 looks from behind the head
    float pitch_deg = 18.0f;   // camera height angle; positive looks down
    float distance_m = 6.5f;   // camera distance to the target
    float target_x = 0.0f;
    float target_y = 1.0f;
    float target_z = 0.0f;
    float fov_deg = 50.0f;
};

struct OrbitCamera {
    OrbitVec3 origin;
    OrbitVec3 right;
    OrbitVec3 up;
    OrbitVec3 forward;
};

inline OrbitCamera orbit_camera(const OrbitView& view) {
    constexpr float kPi = 3.14159265358979323846f;
    const float yaw = view.yaw_deg * kPi / 180.0f;
    const float pitch = view.pitch_deg * kPi / 180.0f;
    const OrbitVec3 target{view.target_x, view.target_y, view.target_z};
    const OrbitVec3 offset{std::sin(yaw) * std::cos(pitch) * view.distance_m,
                           -std::cos(yaw) * std::cos(pitch) * view.distance_m,
                           std::sin(pitch) * view.distance_m};
    OrbitCamera camera;
    camera.origin = OrbitVec3{target.x + offset.x, target.y + offset.y, target.z + offset.z};
    camera.forward = orbit_norm(orbit_sub(target, camera.origin));
    camera.right = orbit_norm(orbit_cross(camera.forward, OrbitVec3{0.0f, 0.0f, 1.0f}));
    camera.up = orbit_cross(camera.right, camera.forward);
    return camera;
}

struct OrbitPoint {
    POINT pixel{};
    float depth_m = 0.0f;  // camera-space forward distance; <= 0.1 means behind the camera
    bool behind = true;
};

inline OrbitPoint orbit_project(const OrbitView& view, const OrbitCamera& camera, OrbitVec3 world,
                                const RECT& rect) {
    constexpr float kPi = 3.14159265358979323846f;
    OrbitPoint out;
    const OrbitVec3 delta = orbit_sub(world, camera.origin);
    const float cx = orbit_dot(delta, camera.right);
    const float cy = orbit_dot(delta, camera.up);
    const float cz = orbit_dot(delta, camera.forward);
    out.depth_m = cz;
    if (cz < 0.1f) {
        return out;
    }
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    const float focal =
        (width < height ? width : height) * 0.5f / std::tan(view.fov_deg * 0.5f * kPi / 180.0f);
    out.pixel.x = rect.left + static_cast<int>(width * 0.5f + focal * cx / cz);
    out.pixel.y = rect.top + static_cast<int>(height * 0.5f - focal * cy / cz);
    out.behind = false;
    return out;
}

struct OrbitQuad {
    OrbitVec3 center;
    OrbitVec3 corners[4];
    float depth_m = 0.0f;
};

// Screen quad in world coordinates: the normal faces the head, right/up follow
// yaw/pitch/roll so the preview matches the headset orientation.
inline OrbitQuad orbit_screen_quad(const ScreenLayout& screen) {
    constexpr float kPi = 3.14159265358979323846f;
    const float yaw = screen.yaw_deg * kPi / 180.0f;
    const float pitch = screen.pitch_deg * kPi / 180.0f;
    const float roll = screen.roll_deg * kPi / 180.0f;
    OrbitQuad quad;
    quad.center = OrbitVec3{screen.distance_m * std::sin(yaw) * std::cos(pitch),
                            screen.distance_m * std::cos(yaw) * std::cos(pitch),
                            screen.distance_m * std::sin(pitch)};
    const OrbitVec3 normal = orbit_norm(orbit_sub(OrbitVec3{0.0f, 0.0f, 0.0f}, quad.center));
    const OrbitVec3 flat_right = orbit_norm(orbit_cross(OrbitVec3{0.0f, 0.0f, 1.0f}, normal));
    const OrbitVec3 flat_up = orbit_cross(normal, flat_right);
    const float cr = std::cos(roll);
    const float sr = std::sin(roll);
    const OrbitVec3 right{flat_right.x * cr + flat_up.x * sr, flat_right.y * cr + flat_up.y * sr,
                          flat_right.z * cr + flat_up.z * sr};
    const OrbitVec3 up{-flat_right.x * sr + flat_up.x * cr, -flat_right.y * sr + flat_up.y * cr,
                       -flat_right.z * sr + flat_up.z * cr};
    const float hw = screen.width_m * 0.5f;
    const float hh = screen.height_m * 0.5f;
    quad.corners[0] = OrbitVec3{quad.center.x - right.x * hw - up.x * hh,
                                quad.center.y - right.y * hw - up.y * hh,
                                quad.center.z - right.z * hw - up.z * hh};
    quad.corners[1] = OrbitVec3{quad.center.x + right.x * hw - up.x * hh,
                                quad.center.y + right.y * hw - up.y * hh,
                                quad.center.z + right.z * hw - up.z * hh};
    quad.corners[2] = OrbitVec3{quad.center.x + right.x * hw + up.x * hh,
                                quad.center.y + right.y * hw + up.y * hh,
                                quad.center.z + right.z * hw + up.z * hh};
    quad.corners[3] = OrbitVec3{quad.center.x - right.x * hw + up.x * hh,
                                quad.center.y - right.y * hw + up.y * hh,
                                quad.center.z - right.z * hw + up.z * hh};
    return quad;
}

// Nearest projected screen centre within radius_px, preferring the nearer
// screen on ties. Returns -1 when nothing is close.
inline int orbit_hit_test(const Layout& layout, const OrbitView& view, const OrbitCamera& camera,
                          const RECT& rect, int x, int y, int radius_px) {
    int best = -1;
    float best_distance = static_cast<float>(radius_px);
    float best_depth = 0.0f;
    for (size_t i = 0; i < layout.screens.size(); ++i) {
        const OrbitQuad quad = orbit_screen_quad(layout.screens[i]);
        const OrbitPoint projected = orbit_project(view, camera, quad.center, rect);
        if (projected.behind) {
            continue;
        }
        const float dx = static_cast<float>(projected.pixel.x - x);
        const float dy = static_cast<float>(projected.pixel.y - y);
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < best_distance - 1e-3f ||
            (distance <= best_distance + 1e-3f && projected.depth_m > best_depth)) {
            best_distance = distance;
            best_depth = projected.depth_m;
            best = static_cast<int>(i);
        }
    }
    return best;
}

}  // namespace gt
