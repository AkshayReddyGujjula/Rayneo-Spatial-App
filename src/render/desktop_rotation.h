#pragma once

// Pure mapping between a desktop's logical (upright) texture coordinates and
// the surface that DXGI Desktop Duplication hands back for a rotated output.
// Header-only and Win32-free so the capture library, the renderer and the
// offline selftests share one definition.
//
// DXGI semantics relied on (Microsoft Learn, "Desktop Duplication API" and
// DXGI_OUTDUPL_DESC.Rotation; the Windows "DXGI desktop duplication" sample,
// DisplayManager::SetDirtyVert and DuplicationManager::GetMouse,
// microsoft/Windows-classic-samples, checked verbatim 2026-11-19):
//   * IDXGIOutputDuplication::AcquireNextFrame returns the desktop image in the
//     output's native, UNROTATED scan-out orientation. For a 1920x1080 mode shown
//     with DMDO_90 the surface is 1920x1080 while the desktop is 1080x1920.
//   * DXGI_OUTDUPL_DESC.Rotation states how that image must be rotated to be
//     upright; the application does the rotation (the sample does it while
//     copying dirty rects, with the destination rect formulas
//       ROTATE90 : dest = (W - src.bottom, src.left,  W - src.top,  src.right)
//       ROTATE180: dest = (W - src.right,  H - src.bottom, W - src.left, H - src.top)
//       ROTATE270: dest = (src.top,  H - src.right, src.bottom, H - src.left)
//     where W x H is the upright desktop size). Dirty and move rects are in the
//     surface space too; this project copies the whole frame and does not use them.
//   * The pointer position (DXGI_OUTDUPL_FRAME_INFO.PointerPosition) is in the
//     upright desktop space relative to the output's DesktopCoordinates, and the
//     pointer shape is upright: the sample composites it onto its upright
//     surface with no rotation.
//
// surface_uv = row0/row1 . (u, v, 1), with (u, v) the upright desktop UV
// (0,0 = top-left). These transforms are the inverse of the rect formulas above;
// view_selftest checks them against those formulas pixel by pixel.

#include <cstdint>

namespace gt {

enum class DesktopRotation : uint32_t {
    Identity = 0,
    Rotate90 = 1,
    Rotate180 = 2,
    Rotate270 = 3,
};

// DXGI_MODE_ROTATION values: UNSPECIFIED 0, IDENTITY 1, ROTATE90 2,
// ROTATE180 3, ROTATE270 4. Taken as an integer so this header stays DXGI-free.
inline DesktopRotation desktop_rotation_from_dxgi(int dxgi_mode_rotation) {
    switch (dxgi_mode_rotation) {
        case 2: return DesktopRotation::Rotate90;
        case 3: return DesktopRotation::Rotate180;
        case 4: return DesktopRotation::Rotate270;
        default: return DesktopRotation::Identity;  // IDENTITY or UNSPECIFIED
    }
}

inline int desktop_rotation_degrees(DesktopRotation rotation) {
    return static_cast<int>(rotation) * 90;
}

inline bool desktop_rotation_swaps_axes(DesktopRotation rotation) {
    return rotation == DesktopRotation::Rotate90 || rotation == DesktopRotation::Rotate270;
}

struct SurfaceUvTransform {
    float row0[3];  // surface u = row0[0]*u + row0[1]*v + row0[2]
    float row1[3];  // surface v = row1[0]*u + row1[1]*v + row1[2]
};

inline SurfaceUvTransform surface_uv_transform(DesktopRotation rotation) {
    switch (rotation) {
        case DesktopRotation::Rotate90:  // surface = (v, 1 - u)
            return {{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 1.0f}};
        case DesktopRotation::Rotate180:  // surface = (1 - u, 1 - v)
            return {{-1.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 1.0f}};
        case DesktopRotation::Rotate270:  // surface = (1 - v, u)
            return {{0.0f, -1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}};
        case DesktopRotation::Identity:
        default:
            return {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    }
}

inline void apply_surface_uv_transform(const SurfaceUvTransform& t, float u, float v,
                                       float& surface_u, float& surface_v) {
    surface_u = t.row0[0] * u + t.row0[1] * v + t.row0[2];
    surface_v = t.row1[0] * u + t.row1[1] * v + t.row1[2];
}

// Upright desktop size from the duplicated surface size (and back: the swap is
// its own inverse).
inline void desktop_size_from_surface(DesktopRotation rotation, uint32_t surface_width,
                                      uint32_t surface_height, uint32_t& desktop_width,
                                      uint32_t& desktop_height) {
    if (desktop_rotation_swaps_axes(rotation)) {
        desktop_width = surface_height;
        desktop_height = surface_width;
    } else {
        desktop_width = surface_width;
        desktop_height = surface_height;
    }
}

}  // namespace gt
