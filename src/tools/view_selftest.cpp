// view_selftest - offline "virtual glasses" harness for the spatial view.
//
// It renders the real three-screen layout through the project's exact camera
// math (gt::camera_view_matrix / gt::camera_applied_euler / gt::make_screen_quad)
// for a sequence of head poses, checks the geometry numerically, and writes a
// small dependency-free software-rasterised PPM per scenario so a human or an
// agent can LOOK at the result without the physical glasses.
//
// It deliberately links only the pure-logic translation units
// (render/camera.cpp, render/screen_geometry.cpp, layout/layout.cpp,
// imu/pose_estimator.cpp, imu/fusion.cpp) - no D3D, no image libraries, no HID.
//
// Usage: view_selftest [output_directory]   (default: "scratch")
//
// NOTE: the pan-and-return scenario drives the REAL gt::PoseEstimator, which is
// being changed concurrently. Its assertion is clearly labelled "(tunable)" and
// the measured numbers are always printed so the limit can be re-tuned later.

#include "imu/fusion.h"
#include "imu/pose_estimator.h"
#include "layout/layout.h"
#include "render/camera.h"
#include "render/screen_geometry.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace DirectX;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg = kPi / 180.0f;

constexpr int kImageWidth = 640;
constexpr int kImageHeight = 360;
constexpr float kNear = 0.05f;

int g_failures = 0;

void check(bool ok, const std::string& label) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) {
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// Quaternion helpers
// ---------------------------------------------------------------------------

gt::Quat quat_axis(float ax, float ay, float az, float degrees) {
    const float n = std::sqrt(ax * ax + ay * ay + az * az);
    if (n < 1e-9f) {
        return gt::Quat{};
    }
    const float half = degrees * 0.5f * kDeg;
    const float s = std::sin(half) / n;
    return gt::Quat{std::cos(half), ax * s, ay * s, az * s};
}

gt::Quat quat_mul(const gt::Quat& a, const gt::Quat& b) {
    return gt::Quat{
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

// Build the head-relative quaternion that gt::camera_applied_euler reports as
// exactly (yaw, pitch, roll) under the default CameraSigns(-1,-1,-1).
//
// camera.cpp maps euler (from quat_to_euler, the ZYX / Rz*Ry*Rx convention) as
// applied.yaw = euler.yaw*sy, applied.pitch = euler.roll*sp, applied.roll =
// euler.pitch*sr. Inverting that for signs(-1,-1,-1) gives
// euler.yaw = -yaw, euler.roll = -pitch, euler.pitch = -roll, and the matching
// quaternion is q = Rz(-yaw) * Ry(-roll) * Rx(-pitch).
gt::Quat head_from_applied_euler(float yaw_deg, float pitch_deg, float roll_deg) {
    return quat_mul(quat_mul(quat_axis(0.0f, 0.0f, 1.0f, -yaw_deg),
                             quat_axis(0.0f, 1.0f, 0.0f, -roll_deg)),
                    quat_axis(1.0f, 0.0f, 0.0f, -pitch_deg));
}

// ---------------------------------------------------------------------------
// Projection / view-space helpers
// ---------------------------------------------------------------------------

XMMATRIX make_projection(float fov_horizontal_deg, float aspect) {
    const float fov_y =
        2.0f * std::atan(std::tan(fov_horizontal_deg * kDeg * 0.5f) / aspect);
    return XMMatrixPerspectiveFovLH(fov_y, aspect, 0.05f, 200.0f);
}

XMFLOAT3 world_to_view(const XMMATRIX& view, const gt::ScreenGeometryVertex& v) {
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector4Transform(XMVectorSet(v.x, v.y, v.z, 1.0f), view));
    return out;
}

XMFLOAT3 world_to_view(const XMMATRIX& view, float x, float y, float z) {
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector4Transform(XMVectorSet(x, y, z, 1.0f), view));
    return out;
}

// View-space point -> normalised device coordinates (x,z roughly -1..1).
bool view_to_ndc(const XMMATRIX& proj, const XMFLOAT3& vp, XMFLOAT2& ndc) {
    XMFLOAT4 clip;
    XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(vp.x, vp.y, vp.z, 1.0f), proj));
    if (clip.w < kNear) {
        return false;
    }
    ndc.x = clip.x / clip.w;
    ndc.y = clip.y / clip.w;
    return true;
}

XMFLOAT2 ndc_to_pixel(const XMFLOAT2& ndc) {
    return XMFLOAT2{(ndc.x * 0.5f + 0.5f) * static_cast<float>(kImageWidth),
                    (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(kImageHeight)};
}

// Shortest-path clip of a view-space polygon against the z = kNear plane.
std::vector<XMFLOAT3> clip_near(const std::vector<XMFLOAT3>& poly) {
    std::vector<XMFLOAT3> out;
    if (poly.empty()) {
        return out;
    }
    for (size_t i = 0; i < poly.size(); ++i) {
        const XMFLOAT3& s = poly[i];
        const XMFLOAT3& e = poly[(i + 1) % poly.size()];
        const bool in_s = s.z >= kNear;
        const bool in_e = e.z >= kNear;
        if (in_s) {
            out.push_back(s);
        }
        if (in_s != in_e) {
            const float t = (kNear - s.z) / (e.z - s.z);
            out.push_back(XMFLOAT3{s.x + (e.x - s.x) * t, s.y + (e.y - s.y) * t, kNear});
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Dependency-free software PPM rasteriser
// ---------------------------------------------------------------------------

uint8_t to_byte(float value) {
    const float scaled = value * 255.0f + 0.5f;
    const float clamped = scaled < 0.0f ? 0.0f : (scaled > 255.0f ? 255.0f : scaled);
    return static_cast<uint8_t>(clamped);
}

struct Image {
    std::vector<uint8_t> rgb;

    Image() : rgb(static_cast<size_t>(kImageWidth) * kImageHeight * 3, 0) {}

    void set(int x, int y, float r, float g, float b) {
        if (x < 0 || y < 0 || x >= kImageWidth || y >= kImageHeight) {
            return;
        }
        const size_t i = (static_cast<size_t>(y) * kImageWidth + x) * 3;
        rgb[i + 0] = to_byte(r);
        rgb[i + 1] = to_byte(g);
        rgb[i + 2] = to_byte(b);
    }

    void blend(int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= kImageWidth || y >= kImageHeight) {
            return;
        }
        const size_t i = (static_cast<size_t>(y) * kImageWidth + x) * 3;
        const auto mix = [a](uint8_t dst, float src) {
            const float d = static_cast<float>(dst) / 255.0f;
            return to_byte(d * (1.0f - a) + src * a);
        };
        rgb[i + 0] = mix(rgb[i + 0], r);
        rgb[i + 1] = mix(rgb[i + 1], g);
        rgb[i + 2] = mix(rgb[i + 2], b);
    }

    bool save(const std::string& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            return false;
        }
        out << "P6\n" << kImageWidth << ' ' << kImageHeight << "\n255\n";
        out.write(reinterpret_cast<const char*>(rgb.data()),
                  static_cast<std::streamsize>(rgb.size()));
        return static_cast<bool>(out);
    }
};

void draw_line(Image& img, float x0, float y0, float x1, float y1, float r, float g, float b) {
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) {
        return;
    }
    const float xmin = 0.0f;
    const float ymin = 0.0f;
    const float xmax = static_cast<float>(kImageWidth - 1);
    const float ymax = static_cast<float>(kImageHeight - 1);
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    float t0 = 0.0f;
    float t1 = 1.0f;
    const auto clip = [&](float p, float q) -> bool {
        if (std::fabs(p) < 1e-9f) {
            return q >= 0.0f;
        }
        const float t = q / p;
        if (p < 0.0f) {
            if (t > t1) return false;
            if (t > t0) t0 = t;
        } else {
            if (t < t0) return false;
            if (t < t1) t1 = t;
        }
        return true;
    };
    if (!clip(-dx, x0 - xmin)) return;
    if (!clip(dx, xmax - x0)) return;
    if (!clip(-dy, y0 - ymin)) return;
    if (!clip(dy, ymax - y0)) return;

    const float ax = x0 + t0 * dx;
    const float ay = y0 + t0 * dy;
    const float bx = x0 + t1 * dx;
    const float by = y0 + t1 * dy;
    const int steps = static_cast<int>(std::fabs(bx - ax) + std::fabs(by - ay)) * 2 + 1;
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        img.set(static_cast<int>(std::lround(ax + (bx - ax) * t)),
                static_cast<int>(std::lround(ay + (by - ay) * t)), r, g, b);
    }
}

float edge(const XMFLOAT2& a, const XMFLOAT2& b, const XMFLOAT2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

void draw_triangle(Image& img, const XMFLOAT2& a, const XMFLOAT2& b, const XMFLOAT2& c, float r,
                   float g, float bl, float alpha) {
    const float area = edge(a, b, c);
    if (std::fabs(area) < 1e-6f) {
        return;
    }
    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int max_x =
        std::min(kImageWidth - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int max_y =
        std::min(kImageHeight - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const XMFLOAT2 p{x + 0.5f, y + 0.5f};
            const float w0 = edge(b, c, p) / area;
            const float w1 = edge(c, a, p) / area;
            const float w2 = edge(a, b, p) / area;
            if (w0 >= -1e-4f && w1 >= -1e-4f && w2 >= -1e-4f) {
                img.blend(x, y, r, g, bl, alpha);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Scenario statistics
// ---------------------------------------------------------------------------

struct ScreenStats {
    std::string id;
    int valid_corners = 0;
    bool all_in_front = false;
    float mean_x = 0.0f;
    float mean_y = 0.0f;
    float min_x = 0.0f;
    float max_x = 0.0f;
    float left_x = 0.0f;   // mean of the two left corners (BL, TL)
    float right_x = 0.0f;  // mean of the two right corners (BR, TR)
    std::array<float, 4> corner_x{};
    std::array<float, 4> corner_y{};
    std::array<float, 4> view_z{};
};

ScreenStats compute_screen_stats(const gt::ScreenLayout& screen, const XMMATRIX& view,
                                 const XMMATRIX& proj) {
    ScreenStats st;
    st.id = screen.id;
    st.min_x = 1e9f;
    st.max_x = -1e9f;
    const auto quad = gt::make_screen_quad(screen);
    const int idx[4] = {0, 1, 2, 5};  // BL, BR, TR, TL
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    float sum_left = 0.0f;
    float sum_right = 0.0f;
    int n_left = 0;
    int n_right = 0;
    for (int k = 0; k < 4; ++k) {
        const XMFLOAT3 vp = world_to_view(view, quad[idx[k]]);
        st.view_z[k] = vp.z;
        XMFLOAT2 ndc;
        if (view_to_ndc(proj, vp, ndc)) {
            st.corner_x[k] = ndc.x;
            st.corner_y[k] = ndc.y;
            ++st.valid_corners;
            sum_x += ndc.x;
            sum_y += ndc.y;
            st.min_x = std::min(st.min_x, ndc.x);
            st.max_x = std::max(st.max_x, ndc.x);
            if (k == 0 || k == 3) {
                sum_left += ndc.x;
                ++n_left;
            } else {
                sum_right += ndc.x;
                ++n_right;
            }
        }
    }
    st.all_in_front = st.valid_corners == 4;
    if (st.valid_corners > 0) {
        st.mean_x = sum_x / static_cast<float>(st.valid_corners);
        st.mean_y = sum_y / static_cast<float>(st.valid_corners);
    }
    if (n_left > 0) st.left_x = sum_left / static_cast<float>(n_left);
    if (n_right > 0) st.right_x = sum_right / static_cast<float>(n_right);
    return st;
}

int find_screen(const gt::Layout& layout, const std::string& id) {
    for (size_t i = 0; i < layout.screens.size(); ++i) {
        if (layout.screens[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

struct ScenarioResult {
    std::string name;
    gt::Euler applied;
    ScreenStats left;
    ScreenStats centre;
    ScreenStats right;
    std::string image_path;
    float fx = 0.0f;
};

// ---------------------------------------------------------------------------
// Scene rendering
// ---------------------------------------------------------------------------

void render_background(Image& img) {
    for (int y = 0; y < kImageHeight; ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(kImageHeight - 1);
        const float r = 0.03f + 0.05f * (1.0f - t);
        const float g = 0.03f + 0.06f * (1.0f - t);
        const float b = 0.06f + 0.10f * (1.0f - t);
        for (int x = 0; x < kImageWidth; ++x) {
            img.set(x, y, r, g, b);
        }
    }
}

void draw_ground(Image& img, const XMMATRIX& view, const XMMATRIX& proj) {
    // Ground grid on the y = 0 plane, mirroring the app's floor grid.
    const float grid = 0.17f;
    for (int i = -10; i <= 10; ++i) {
        const float t = static_cast<float>(i) * 2.0f;
        const XMFLOAT3 a = world_to_view(view, t, 0.0f, -20.0f);
        const XMFLOAT3 b = world_to_view(view, t, 0.0f, 20.0f);
        // Draw each half separately so near-clipping stays sane.
        const auto seg = [&](const XMFLOAT3& p, const XMFLOAT3& q) {
            if (p.z < kNear && q.z < kNear) return;
            XMFLOAT3 pp = p;
            XMFLOAT3 qq = q;
            if (pp.z < kNear) {
                const float tt = (kNear - pp.z) / (qq.z - pp.z);
                pp = XMFLOAT3{pp.x + (qq.x - pp.x) * tt, pp.y + (qq.y - pp.y) * tt, kNear};
            } else if (qq.z < kNear) {
                const float tt = (kNear - qq.z) / (pp.z - qq.z);
                qq = XMFLOAT3{qq.x + (pp.x - qq.x) * tt, qq.y + (pp.y - qq.y) * tt, kNear};
            }
            XMFLOAT2 na;
            XMFLOAT2 nb;
            if (!view_to_ndc(proj, pp, na) || !view_to_ndc(proj, qq, nb)) return;
            const XMFLOAT2 pa = ndc_to_pixel(na);
            const XMFLOAT2 pb = ndc_to_pixel(nb);
            draw_line(img, pa.x, pa.y, pb.x, pb.y, grid, grid + 0.02f, grid + 0.05f);
        };
        seg(a, b);
        const XMFLOAT3 c = world_to_view(view, -20.0f, 0.0f, t);
        const XMFLOAT3 d = world_to_view(view, 20.0f, 0.0f, t);
        seg(c, d);
    }

    // The horizon: the ground direction at a large radius, drawn as a dense
    // polyline so it stays a straight line under any head rotation.
    XMFLOAT2 previous{0.0f, 0.0f};
    bool have_previous = false;
    for (int a = -100; a <= 100; ++a) {
        const float az = static_cast<float>(a) * kDeg;
        const float radius = 500.0f;
        const XMFLOAT3 vp = world_to_view(view, radius * std::sin(az), 0.0f, radius * std::cos(az));
        if (vp.z < kNear) {
            have_previous = false;
            continue;
        }
        XMFLOAT2 ndc;
        if (!view_to_ndc(proj, vp, ndc)) {
            have_previous = false;
            continue;
        }
        const XMFLOAT2 px = ndc_to_pixel(ndc);
        if (have_previous) {
            draw_line(img, previous.x, previous.y, px.x, px.y, 0.45f, 0.42f, 0.34f);
        }
        previous = px;
        have_previous = true;
    }
}

void draw_crosshair(Image& img) {
    const int cx = kImageWidth / 2;
    const int cy = kImageHeight / 2;
    for (int i = -16; i <= 16; ++i) {
        img.set(cx + i, cy, 0.95f, 0.95f, 0.95f);
        img.set(cx, cy + i, 0.95f, 0.95f, 0.95f);
    }
    for (int i = -1; i <= 1; ++i) {
        for (int j = -1; j <= 1; ++j) {
            img.set(cx + i, cy + j, 1.0f, 0.30f, 0.30f);
        }
    }
}

ScenarioResult render_scenario(const std::string& name, const gt::Quat& head, const gt::Layout& layout,
                               const gt::CameraSigns& signs, const std::string& out_dir) {
    const float aspect = static_cast<float>(kImageWidth) / static_cast<float>(kImageHeight);
    const XMMATRIX proj = make_projection(layout.fov_deg, aspect);
    const XMMATRIX view = gt::camera_view_matrix(head, signs);

    XMFLOAT4X4 proj_store;
    XMStoreFloat4x4(&proj_store, proj);
    const float fx = proj_store.m[0][0];

    ScenarioResult result;
    result.name = name;
    result.applied = gt::camera_applied_euler(head, signs);
    result.fx = fx;

    Image img;
    render_background(img);
    draw_ground(img, view, proj);

    // Screen fills first (back to front), then every outline on top so no
    // outline is buried by a nearer translucent fill.
    struct Poly {
        const gt::ScreenLayout* screen;
        std::vector<XMFLOAT2> pixels;
        float depth;
    };
    std::vector<Poly> polys;
    for (const gt::ScreenLayout& screen : layout.screens) {
        const auto quad = gt::make_screen_quad(screen);
        const int idx[4] = {0, 1, 2, 5};
        std::vector<XMFLOAT3> poly;
        float depth_sum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const XMFLOAT3 vp = world_to_view(view, quad[idx[k]]);
            depth_sum += vp.z;
            poly.push_back(vp);
        }
        const std::vector<XMFLOAT3> clipped = clip_near(poly);
        Poly entry;
        entry.screen = &screen;
        entry.depth = depth_sum * 0.25f;
        for (const XMFLOAT3& vp : clipped) {
            XMFLOAT2 ndc;
            if (view_to_ndc(proj, vp, ndc)) {
                entry.pixels.push_back(ndc_to_pixel(ndc));
            }
        }
        if (entry.pixels.size() >= 3) {
            polys.push_back(std::move(entry));
        }
    }
    std::sort(polys.begin(), polys.end(),
              [](const Poly& a, const Poly& b) { return a.depth > b.depth; });
    for (const Poly& poly : polys) {
        const float r = poly.screen->color[0];
        const float g = poly.screen->color[1];
        const float b = poly.screen->color[2];
        for (size_t i = 1; i + 1 < poly.pixels.size(); ++i) {
            draw_triangle(img, poly.pixels[0], poly.pixels[i], poly.pixels[i + 1], r, g, b, 0.18f);
        }
    }
    for (const Poly& poly : polys) {
        const float r = poly.screen->color[0];
        const float g = poly.screen->color[1];
        const float b = poly.screen->color[2];
        for (size_t i = 0; i < poly.pixels.size(); ++i) {
            const XMFLOAT2& a = poly.pixels[i];
            const XMFLOAT2& c = poly.pixels[(i + 1) % poly.pixels.size()];
            draw_line(img, a.x, a.y, c.x, c.y, r, g, b);
        }
    }
    draw_crosshair(img);

    const std::string path = out_dir + "/view_" + name + ".ppm";
    if (!img.save(path)) {
        std::printf("  !! failed to write %s\n", path.c_str());
        ++g_failures;
    }
    result.image_path = path;

    const auto stats_for = [&](const std::string& id) {
        const int index = find_screen(layout, id);
        if (index < 0) {
            return ScreenStats{};
        }
        return compute_screen_stats(layout.screens[static_cast<size_t>(index)], view, proj);
    };
    result.left = stats_for("left");
    result.centre = stats_for("centre");
    result.right = stats_for("right");
    return result;
}

void print_scenario(const ScenarioResult& s, const gt::Layout& layout) {
    (void)layout;
    std::printf("scenario %-22s applied yaw=%+7.2f pitch=%+7.2f roll=%+7.2f\n", s.name.c_str(),
                s.applied.yaw_deg, s.applied.pitch_deg, s.applied.roll_deg);
    const auto line = [](const char* label, const ScreenStats& st) {
        std::printf("    %-7s mean ndc x=%+8.4f y=%+8.4f  [%.4f..%.4f]  corners x:", label,
                    st.mean_x, st.mean_y, st.min_x, st.max_x);
        for (float x : st.corner_x) {
            std::printf(" %+7.4f", x);
        }
        std::printf("  behind=%d\n", 4 - st.valid_corners);
    };
    line("left", s.left);
    line("centre", s.centre);
    line("right", s.right);
    std::printf("    image: %s\n", s.image_path.c_str());
}

// ---------------------------------------------------------------------------
// Synthetic-IMU helper (copied from pose_selftest.cpp)
// ---------------------------------------------------------------------------

struct Mat3 {
    float m[3][3];
};

Mat3 rotation_about(float ax, float ay, float az, float angle_rad) {
    const float n = std::sqrt(ax * ax + ay * ay + az * az);
    ax /= n;
    ay /= n;
    az /= n;
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    const float t = 1.0f - c;
    Mat3 r{};
    r.m[0][0] = t * ax * ax + c;
    r.m[0][1] = t * ax * ay - s * az;
    r.m[0][2] = t * ax * az + s * ay;
    r.m[1][0] = t * ax * ay + s * az;
    r.m[1][1] = t * ay * ay + c;
    r.m[1][2] = t * ay * az - s * ax;
    r.m[2][0] = t * ax * az - s * ay;
    r.m[2][1] = t * ay * az + s * ax;
    r.m[2][2] = t * az * az + c;
    return r;
}

Mat3 mat_mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return r;
}

gt::Vec3 earth_to_body(const Mat3& r, const gt::Vec3& v) {
    return gt::Vec3{r.m[0][0] * v.x + r.m[1][0] * v.y + r.m[2][0] * v.z,
                    r.m[0][1] * v.x + r.m[1][1] * v.y + r.m[2][1] * v.z,
                    r.m[0][2] * v.x + r.m[1][2] * v.y + r.m[2][2] * v.z};
}

gt::Vec3 package_from_body(const gt::Vec3& body) {
    return gt::Vec3{body.x, body.z, -body.y};
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out_dir = argc > 1 ? argv[1] : "scratch";
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    std::printf("view_selftest: offline spatial-view verification (%dx%d PPM)\n", kImageWidth,
                kImageHeight);

    gt::Layout layout;
    std::string layout_error;
    std::string layout_source;
    bool loaded = false;
    for (const char* candidate :
         {"config/layouts/default.json", "../config/layouts/default.json",
          "../../config/layouts/default.json"}) {
        if (std::filesystem::exists(candidate) && gt::load_layout(candidate, layout, layout_error)) {
            layout_source = candidate;
            loaded = true;
            break;
        }
    }
    if (!loaded) {
        layout = gt::default_layout();
        layout_source = "hard-coded gt::default_layout()";
    }
    std::printf("layout: %s (%zu screens, fov %.1f deg)\n", layout_source.c_str(),
                layout.screens.size(), layout.fov_deg);

    const gt::CameraSigns signs;  // defaults yaw=-1, pitch=-1, roll=-1

    const float aspect = static_cast<float>(kImageWidth) / static_cast<float>(kImageHeight);
    XMFLOAT4X4 proj_store;
    XMStoreFloat4x4(&proj_store, make_projection(layout.fov_deg, aspect));
    const float fx_scale = proj_store.m[0][0];  // 1 / tan(fov_x / 2)

    // --- direct-Euler scenarios -------------------------------------------------
    std::printf("\n== direct head poses (applied Euler) ==\n");

    const ScenarioResult zero =
        render_scenario("zero", head_from_applied_euler(0.0f, 0.0f, 0.0f), layout, signs, out_dir);
    print_scenario(zero, layout);

    const ScenarioResult yaw_right = render_scenario(
        "yaw_right_40", head_from_applied_euler(40.0f, 0.0f, 0.0f), layout, signs, out_dir);
    print_scenario(yaw_right, layout);

    const ScenarioResult yaw_left = render_scenario(
        "yaw_left_40", head_from_applied_euler(-40.0f, 0.0f, 0.0f), layout, signs, out_dir);
    print_scenario(yaw_left, layout);

    const ScenarioResult nod_down = render_scenario(
        "nod_down_20", head_from_applied_euler(0.0f, 20.0f, 0.0f), layout, signs, out_dir);
    print_scenario(nod_down, layout);

    // --- numeric assertions -----------------------------------------------------
    std::printf("\n== numeric assertions ==\n");

    // Round-trip: our Euler -> quaternion -> camera_applied_euler must recover
    // exactly the requested Euler angles.
    {
        const gt::Euler rt = gt::camera_applied_euler(head_from_applied_euler(40.0f, 20.0f, -15.0f), signs);
        const float err = std::fabs(rt.yaw_deg - 40.0f) + std::fabs(rt.pitch_deg - 20.0f) +
                          std::fabs(rt.roll_deg + 15.0f);
        std::printf("  Euler round-trip error: %.6f deg\n", err);
        check(err < 1e-3f, "head_from_applied_euler inverts camera_applied_euler");
    }

    // Zero pose: the centre screen is centred and symmetric.
    {
        std::printf("  centre mean ndc x = %+.6f (|left| %+.5f vs |right| %+.5f)\n",
                    zero.centre.mean_x, zero.centre.left_x, zero.centre.right_x);
        check(zero.centre.all_in_front, "centre screen corners are all in front of the camera");
        check(std::fabs(zero.centre.mean_x) < 1e-4f,
              "zero pose: centre screen mean projected x ~ 0");
        check(std::fabs(zero.centre.left_x + zero.centre.right_x) < 1e-4f,
              "zero pose: centre screen corners are symmetric about x = 0");
        check(std::fabs(std::fabs(zero.centre.left_x) - std::fabs(zero.centre.right_x)) < 1e-4f,
              "zero pose: |x| of the two halves match within epsilon");
        check(std::fabs(zero.centre.mean_y) < 1e-4f,
              "zero pose: centre screen is vertically centred too");
    }

    // Ordering left < centre < right at zero pose.
    {
        const bool ordered = zero.left.mean_x < zero.centre.mean_x &&
                             zero.centre.mean_x < zero.right.mean_x;
        std::printf("  order zero: left %.4f < centre %.4f < right %.4f\n", zero.left.mean_x,
                    zero.centre.mean_x, zero.right.mean_x);
        check(ordered, "zero pose: screens stay ordered left < centre < right");
    }

    // +40 yaw: the previously-right screen swings toward the middle.
    {
        std::printf("  right screen mean ndc x: zero %+.4f -> yaw+40 %+.4f\n", zero.right.mean_x,
                    yaw_right.right.mean_x);
        check(std::fabs(yaw_right.applied.yaw_deg - 40.0f) < 1e-3f,
              "+40 yaw pose reports applied yaw = +40 deg");
        check(std::fabs(yaw_right.right.mean_x) < std::fabs(zero.right.mean_x),
              "+40 yaw: right screen is closer to the centre than at zero pose");
        check(yaw_right.right.mean_x < zero.right.mean_x - 1e-3f,
              "+40 yaw: right screen moves inward (toward smaller x), not outward");
        check(yaw_right.right.mean_x > -0.05f,
              "+40 yaw: right screen is at or right of the view centre");
        const bool ordered = yaw_right.left.mean_x < yaw_right.centre.mean_x &&
                             yaw_right.centre.mean_x < yaw_right.right.mean_x;
        std::printf("  order yaw+40: left %.4f < centre %.4f < right %.4f\n", yaw_right.left.mean_x,
                    yaw_right.centre.mean_x, yaw_right.right.mean_x);
        check(ordered, "+40 yaw: screens stay ordered left < centre < right");
        check(yaw_right.centre.mean_x < zero.centre.mean_x,
              "+40 yaw: centre screen moves left as the view pans right");
    }

    // Mirror check: -40 yaw swings the left screen toward the middle.
    {
        std::printf("  left screen mean ndc x: zero %+.4f -> yaw-40 %+.4f\n", zero.left.mean_x,
                    yaw_left.left.mean_x);
        check(std::fabs(yaw_left.left.mean_x) < std::fabs(zero.left.mean_x),
              "-40 yaw: left screen is closer to the centre than at zero pose");
        check(yaw_left.left.mean_x < 0.05f,
              "-40 yaw: left screen is at or left of the view centre");
    }

    // --- pan-and-return driven through the real estimator -----------------------
    std::printf("\n== pan-and-return via gt::PoseEstimator (synthetic IMU) ==\n");
    ScenarioResult pan_mid;
    ScenarioResult pan_return;
    float return_yaw_residual = 0.0f;
    {
        gt::PoseEstimator estimator;  // default config; no field names referenced
        Mat3 attitude = rotation_about(0.0f, 0.0f, 1.0f, 0.0f);
        uint32_t tick = 300000;
        const float dt = 21.0f * 1e-4f;

        const auto feed = [&](const gt::Vec3& omega_earth_degs, int samples) {
            for (int i = 0; i < samples; ++i) {
                const gt::Vec3 accel_body = earth_to_body(attitude, gt::Vec3{0.0f, 0.0f, 9.81f});
                const gt::Vec3 gyro_body = earth_to_body(attitude, omega_earth_degs);
                gt::ImuSample s;
                s.accel_mps2 = package_from_body(accel_body);
                s.gyro_degs = package_from_body(gyro_body);
                s.tick_100us = tick += 21;
                estimator.add_sample(s);
                if (omega_earth_degs.z != 0.0f) {
                    attitude = mat_mul(
                        rotation_about(0.0f, 0.0f, 1.0f, omega_earth_degs.z * kDeg * dt), attitude);
                }
            }
        };

        feed(gt::Vec3{0.0f, 0.0f, 0.0f}, 3000);  // settle + calibrate
        const gt::Euler baseline = estimator.euler();
        std::printf("  baseline after calibration: yaw=%+.3f pitch=%+.3f roll=%+.3f (bias_done=%d)\n",
                    baseline.yaw_deg, baseline.pitch_deg, baseline.roll_deg,
                    estimator.bias_done() ? 1 : 0);

        feed(gt::Vec3{0.0f, 0.0f, -25.0f}, 476);  // 1 s pan to the right (~ +25 applied yaw)
        pan_mid = render_scenario("pan_estimator_mid", estimator.quat(), layout, signs, out_dir);
        print_scenario(pan_mid, layout);

        feed(gt::Vec3{0.0f, 0.0f, 25.0f}, 476);  // 1 s back
        feed(gt::Vec3{0.0f, 0.0f, 0.0f}, 476);   // 1 s settle

        pan_return = render_scenario("pan_return", estimator.quat(), layout, signs, out_dir);
        print_scenario(pan_return, layout);

        return_yaw_residual = pan_return.applied.yaw_deg;
        const float equivalent_deg = return_yaw_residual;
        std::printf("  measured residual: euler.yaw=%+.4f deg, applied yaw=%+.4f deg, centre mean ndc x=%+.6f\n",
                    estimator.euler().yaw_deg, return_yaw_residual, pan_return.centre.mean_x);
        std::printf("  (equivalent recentre angle %.4f deg; screen-centre tolerance for 0.3 deg ~ %.4f ndc)\n",
                    equivalent_deg, fx_scale * std::tan(0.3f * kDeg) + 1e-4f);

        // 0.3 deg equivalent recentring - TUNABLE: the estimator is under
        // concurrent change, so re-tune this limit against the printed number.
        const float kReturnLimitDeg = 0.3f;
        check(std::fabs(return_yaw_residual) < kReturnLimitDeg,
              "pan-and-return: applied yaw returns to 0 within 0.3 deg (tunable)");
        check(std::fabs(pan_return.centre.mean_x) < fx_scale * std::tan(0.8f * kDeg),
              "pan-and-return: centre screen returns to centred (tunable)");

        // Sanity: the mid-pan pose really did pan right.
        check(pan_mid.applied.yaw_deg > 15.0f,
              "mid-pan pose reports a right pan (applied yaw > 15 deg)");
    }

    std::printf("\nview_selftest: %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
