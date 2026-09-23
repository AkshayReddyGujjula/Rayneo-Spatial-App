#include "app_shell/ui_theme.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gt::ui {
namespace {

HFONT create_font(UINT dpi, int point_size, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(point_size, static_cast<int>(dpi), 72), 0, 0, 0, weight, FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

struct Rgba {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
};

Rgba blend(const Rgba& base, const Rgba& over) {
    Rgba out;
    out.a = over.a + base.a * (1.0 - over.a);
    if (out.a <= 0.0) {
        return out;
    }
    out.r = (over.r * over.a + base.r * base.a * (1.0 - over.a)) / out.a;
    out.g = (over.g * over.a + base.g * base.a * (1.0 - over.a)) / out.a;
    out.b = (over.b * over.a + base.b * base.a * (1.0 - over.a)) / out.a;
    return out;
}

Rgba from_colorref(COLORREF color, double alpha) {
    Rgba out;
    out.r = static_cast<double>(GetRValue(color)) / 255.0;
    out.g = static_cast<double>(GetGValue(color)) / 255.0;
    out.b = static_cast<double>(GetBValue(color)) / 255.0;
    out.a = alpha;
    return out;
}

double rounded_rect_coverage(double x, double y, double half_width, double half_height,
                             double radius) {
    const double dx = std::fabs(x) - (half_width - radius);
    const double dy = std::fabs(y) - (half_height - radius);
    const double outside = std::sqrt(std::max(dx, 0.0) * std::max(dx, 0.0) +
                                     std::max(dy, 0.0) * std::max(dy, 0.0));
    const double distance = outside + std::min(std::max(dx, dy), 0.0) - radius;
    return std::clamp(0.5 - distance, 0.0, 1.0);
}

}  // namespace

int scale_px(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void FontSet::create(UINT dpi) {
    destroy();
    title = create_font(dpi, 15, FW_SEMIBOLD, L"Segoe UI");
    heading = create_font(dpi, 11, FW_SEMIBOLD, L"Segoe UI");
    body = create_font(dpi, 10, FW_NORMAL, L"Segoe UI");
    small = create_font(dpi, 9, FW_NORMAL, L"Segoe UI");
    mono = create_font(dpi, 9, FW_NORMAL, L"Consolas");
}

void FontSet::destroy() {
    HFONT* fonts[] = {&title, &heading, &body, &small, &mono};
    for (HFONT* font : fonts) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

HICON create_app_icon(int size) {
    if (size < 8 || size > 256) {
        size = 32;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color_bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    const size_t mask_stride = (static_cast<size_t>(size) + 15u) / 16u * 2u;
    std::vector<uint8_t> mask_pixels(mask_stride * static_cast<size_t>(size), 0);
    HBITMAP mask_bitmap =
        CreateBitmap(size, size, 1, 1, mask_pixels.data());
    ReleaseDC(nullptr, screen);
    if (color_bitmap == nullptr || mask_bitmap == nullptr || bits == nullptr) {
        if (color_bitmap != nullptr) {
            DeleteObject(color_bitmap);
        }
        if (mask_bitmap != nullptr) {
            DeleteObject(mask_bitmap);
        }
        return nullptr;
    }

    const Rgba plate = from_colorref(rgb(21, 34, 58), 1.0);
    const Rgba accent = from_colorref(palette::accent, 1.0);
    const Rgba glyph = from_colorref(rgb(236, 242, 252), 1.0);
    const double half = static_cast<double>(size) * 0.5;
    const double radius = static_cast<double>(size) * 0.22;
    const double arc_radius = static_cast<double>(size) * 0.30;
    const double arc_thickness = std::max(1.0, static_cast<double>(size) * 0.055);

    auto* pixels = static_cast<uint32_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double px = static_cast<double>(x) + 0.5 - half;
            const double py = static_cast<double>(y) + 0.5 - half;
            const double coverage =
                rounded_rect_coverage(px, py, half, half, radius);
            Rgba pixel = from_colorref(0, 0.0);
            if (coverage > 0.0) {
                pixel = blend(from_colorref(0, 0.0), plate);
                pixel.a = coverage;
                const double distance = std::sqrt(px * px + py * py);
                if (std::fabs(distance - arc_radius) < arc_thickness && py < 0.0) {
                    pixel = blend(pixel, accent);
                }
                // Three screen marks along the arc.
                const double marks[3] = {-0.55, 0.0, 0.55};
                for (const double angle : marks) {
                    const double mx = std::sin(angle) * arc_radius;
                    const double my = -std::cos(angle) * arc_radius;
                    if (std::fabs(px - mx) < size * 0.075 && std::fabs(py - my) < size * 0.075) {
                        pixel = blend(pixel, glyph);
                    }
                }
            }
            const double alpha = pixel.a;
            const uint8_t alpha_byte = static_cast<uint8_t>(std::clamp(alpha * 255.0, 0.0, 255.0));
            const uint8_t red = static_cast<uint8_t>(std::clamp(pixel.r * 255.0, 0.0, 255.0));
            const uint8_t green = static_cast<uint8_t>(std::clamp(pixel.g * 255.0, 0.0, 255.0));
            const uint8_t blue = static_cast<uint8_t>(std::clamp(pixel.b * 255.0, 0.0, 255.0));
            pixels[y * size + x] = (static_cast<uint32_t>(alpha_byte) << 24) |
                                   (static_cast<uint32_t>(red) << 16) |
                                   (static_cast<uint32_t>(green) << 8) |
                                   static_cast<uint32_t>(blue);
        }
    }

    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color_bitmap;
    icon_info.hbmMask = mask_bitmap;
    const HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(color_bitmap);
    DeleteObject(mask_bitmap);
    return icon;
}

}  // namespace gt::ui
