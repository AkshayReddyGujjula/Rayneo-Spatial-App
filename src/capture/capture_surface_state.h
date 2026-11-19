#pragma once

// Pure capture bookkeeping, free of Windows/DXGI so view_selftest can check it.
//
// Two rules that DesktopDuplicator follows:
//  1. A GDI resize publishes its new geometry only after every replacement
//     resource was allocated. A failed allocation keeps the committed rect, so
//     the next rect check still sees the change pending and retries it.
//  2. The texture, its surface size and its rotation reach the renderer
//     together. Whenever the cached copy is invalidated (re-duplication, copy
//     texture replaced) or the surface geometry changed since the last
//     publish, the next acquired frame copies and publishes even when it is
//     pointer-only (LastPresentTime == 0).

#include "render/desktop_rotation.h"

#include <cstdint>

namespace gt {

struct CaptureRect {
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
};

enum class GdiRectChange {
    None,    // absent, degenerate or unchanged: keep the committed surface
    Move,    // same size elsewhere: only the rect changes
    Resize,  // new size: reallocate, then commit
};

inline GdiRectChange classify_gdi_rect_change(bool found, const CaptureRect& committed,
                                              const CaptureRect& current) {
    if (!found) return GdiRectChange::None;
    if (current.right <= current.left || current.bottom <= current.top) return GdiRectChange::None;
    if (current.left == committed.left && current.top == committed.top &&
        current.right == committed.right && current.bottom == committed.bottom) {
        return GdiRectChange::None;
    }
    if (current.right - current.left == committed.right - committed.left &&
        current.bottom - current.top == committed.bottom - committed.top) {
        return GdiRectChange::Move;
    }
    return GdiRectChange::Resize;
}

// What the renderer last received with a texture.
struct PublishedDesktop {
    bool valid = false;
    DesktopRotation rotation = DesktopRotation::Identity;
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
};

inline bool desktop_publish_required(bool presented, const PublishedDesktop& published,
                                     DesktopRotation rotation, uint32_t surface_width,
                                     uint32_t surface_height) {
    return presented || !published.valid || published.rotation != rotation ||
           published.surface_width != surface_width || published.surface_height != surface_height;
}

}  // namespace gt
