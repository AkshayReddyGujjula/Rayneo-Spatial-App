#pragma once

// Orbiting perspective math for the controller's 3D arrangement view.
// Header-only and dependency-free (pure float math + win32 RECT/POINT) so the
// dashboard and the selftest share it. World frame: X = right, Y = forward,
// Z = up, metres, with the head at the origin.

#include "layout/layout.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <limits>
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

inline OrbitVec3 orbit_add(OrbitVec3 a, OrbitVec3 b) {
    return OrbitVec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

inline OrbitVec3 orbit_scale(OrbitVec3 v, float scale) {
    return OrbitVec3{v.x * scale, v.y * scale, v.z * scale};
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

inline float orbit_edge_distance_sq(int x, int y, POINT a, POINT b) {
    const float dx = static_cast<float>(b.x - a.x);
    const float dy = static_cast<float>(b.y - a.y);
    const float length_sq = dx * dx + dy * dy;
    if (length_sq < 1e-4f) {
        const float px = static_cast<float>(x - a.x);
        const float py = static_cast<float>(y - a.y);
        return px * px + py * py;
    }
    const float along = std::clamp(
        (static_cast<float>(x - a.x) * dx + static_cast<float>(y - a.y) * dy) / length_sq,
        0.0f, 1.0f);
    const float px = static_cast<float>(x - a.x) - along * dx;
    const float py = static_cast<float>(y - a.y) - along * dy;
    return px * px + py * py;
}

// Select the visible projected face, including its interior and a small edge
// tolerance. Overlapping faces select the one nearest the editor camera.
inline int orbit_hit_test(const Layout& layout, const OrbitView& view, const OrbitCamera& camera,
                          const RECT& rect, int x, int y, int radius_px) {
    int best = -1;
    float best_distance_sq = std::numeric_limits<float>::infinity();
    float best_depth = std::numeric_limits<float>::infinity();
    const float tolerance_sq = static_cast<float>(radius_px * radius_px);
    for (size_t i = 0; i < layout.screens.size(); ++i) {
        const OrbitQuad quad = orbit_screen_quad(layout.screens[i]);
        POINT corners[4];
        bool visible = true;
        for (int corner = 0; corner < 4; ++corner) {
            const OrbitPoint projected = orbit_project(view, camera, quad.corners[corner], rect);
            if (projected.behind) {
                visible = false;
                break;
            }
            corners[corner] = projected.pixel;
        }
        if (!visible) {
            continue;
        }
        bool has_positive = false;
        bool has_negative = false;
        float edge_distance_sq = std::numeric_limits<float>::infinity();
        for (int corner = 0; corner < 4; ++corner) {
            const POINT a = corners[corner];
            const POINT b = corners[(corner + 1) % 4];
            const float side = static_cast<float>(b.x - a.x) * static_cast<float>(y - a.y) -
                               static_cast<float>(b.y - a.y) * static_cast<float>(x - a.x);
            has_positive |= side > 0.0f;
            has_negative |= side < 0.0f;
            edge_distance_sq = std::min(edge_distance_sq, orbit_edge_distance_sq(x, y, a, b));
        }
        const bool inside = !(has_positive && has_negative);
        const float distance_sq = inside ? 0.0f : edge_distance_sq;
        if (distance_sq > tolerance_sq) {
            continue;
        }
        const float depth = orbit_project(view, camera, quad.center, rect).depth_m;
        if (distance_sq < best_distance_sq - 1e-3f ||
            (std::fabs(distance_sq - best_distance_sq) <= 1e-3f && depth < best_depth)) {
            best_distance_sq = distance_sq;
            best_depth = depth;
            best = static_cast<int>(i);
        }
    }
    return best;
}

struct OrbitDragAngles {
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
};

// Drag the selected screen's centre along the pointer in the current camera
// view. The screen stays at its chosen head distance; the separate Distance
// slider changes that radius. Choose the ray/sphere intersection closest to
// the starting screen so an oblique camera cannot flip it to the far side.
inline OrbitDragAngles orbit_drag_angles(const ScreenLayout& screen, const OrbitView& view,
                                          const OrbitCamera& camera, const RECT& rect,
                                          int delta_x, int delta_y) {
    OrbitDragAngles result{screen.yaw_deg, screen.pitch_deg};
    if (delta_x == 0 && delta_y == 0) {
        return result;
    }
    constexpr float kPi = 3.14159265358979323846f;
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    if (width <= 0.0f || height <= 0.0f || screen.distance_m <= 0.0f) {
        return result;
    }
    const OrbitVec3 start = orbit_screen_quad(screen).center;
    const OrbitPoint projected = orbit_project(view, camera, start, rect);
    if (projected.behind) {
        return result;
    }
    const float focal = std::min(width, height) * 0.5f /
                        std::tan(view.fov_deg * 0.5f * kPi / 180.0f);
    const float x = static_cast<float>(projected.pixel.x + delta_x);
    const float y = static_cast<float>(projected.pixel.y + delta_y);
    const float horizontal = (x - static_cast<float>(rect.left) - width * 0.5f) / focal;
    const float vertical = -(y - static_cast<float>(rect.top) - height * 0.5f) / focal;
    const OrbitVec3 ray = orbit_norm(orbit_add(
        camera.forward,
        orbit_add(orbit_scale(camera.right, horizontal), orbit_scale(camera.up, vertical))));
    const float along = -orbit_dot(camera.origin, ray);
    const float c = orbit_dot(camera.origin, camera.origin) -
                    screen.distance_m * screen.distance_m;
    const float discriminant = along * along - c;
    OrbitVec3 point;
    if (discriminant >= 0.0f) {
        const float root = std::sqrt(discriminant);
        const float near_t = along - root;
        const float far_t = along + root;
        const OrbitVec3 near_point = orbit_add(camera.origin, orbit_scale(ray, near_t));
        const OrbitVec3 far_point = orbit_add(camera.origin, orbit_scale(ray, far_t));
        const OrbitVec3 near_delta = orbit_sub(near_point, start);
        const OrbitVec3 far_delta = orbit_sub(far_point, start);
        point = (near_t > 0.0f &&
                 (far_t <= 0.0f || orbit_dot(near_delta, near_delta) <=
                                        orbit_dot(far_delta, far_delta)))
                    ? near_point : far_point;
    } else {
        // The pointer has moved beyond the sphere's projected silhouette.
        // Stop at the nearest reachable point rather than jumping or reversing.
        point = orbit_scale(orbit_norm(orbit_add(camera.origin,
                                                orbit_scale(ray, std::max(0.0f, along)))),
                            screen.distance_m);
    }
    result.yaw_deg = std::atan2(point.x, point.y) * 180.0f / kPi;
    result.pitch_deg = std::asin(std::clamp(point.z / screen.distance_m, -1.0f, 1.0f)) *
                       180.0f / kPi;
    return result;
}

}  // namespace gt
