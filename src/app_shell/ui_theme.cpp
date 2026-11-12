#include "app_shell/ui_theme.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gt::ui {
namespace {

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

COLORREF mix(COLORREF a, COLORREF b, float t) {
    const float k = std::clamp(t, 0.0f, 1.0f);
    const auto channel = [k](int x, int y) {
        return static_cast<int>(std::lround(static_cast<float>(x) +
                                            (static_cast<float>(y) - static_cast<float>(x)) * k));
    };
    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)),
               channel(GetBValue(a), GetBValue(b)));
}

const Theme& dark_theme() {
    static const Theme theme = [] {
        Theme t;
        t.dark = true;
        t.window = rgb(17, 19, 24);
        t.sidebar = rgb(12, 14, 18);
        t.surface = rgb(25, 28, 35);
        t.surface_alt = rgb(33, 37, 46);
        t.surface_hover = rgb(40, 45, 56);
        t.canvas = rgb(14, 16, 21);
        t.grid = rgb(37, 43, 56);
        t.border = rgb(39, 44, 55);
        t.border_strong = rgb(60, 67, 83);
        t.text = rgb(237, 240, 246);
        t.text_dim = rgb(163, 171, 187);
        t.text_faint = rgb(112, 120, 137);
        t.disabled_text = rgb(82, 89, 103);
        t.accent = rgb(66, 129, 247);
        t.accent_hover = rgb(96, 152, 255);
        t.accent_text = rgb(255, 255, 255);
        t.accent_soft = rgb(29, 43, 71);
        t.accent_dim = rgb(52, 98, 186);
        t.ok = rgb(56, 199, 125);
        t.warning = rgb(240, 178, 62);
        t.error = rgb(240, 98, 98);
        t.focus = rgb(140, 185, 255);
        t.scrim = rgb(5, 7, 11);
        t.shadow = rgb(0, 0, 0);
        return t;
    }();
    return theme;
}

const Theme& light_theme() {
    static const Theme theme = [] {
        Theme t;
        t.dark = false;
        t.window = rgb(244, 246, 250);
        t.sidebar = rgb(235, 238, 244);
        t.surface = rgb(255, 255, 255);
        t.surface_alt = rgb(238, 241, 246);
        t.surface_hover = rgb(229, 233, 241);
        t.canvas = rgb(247, 249, 252);
        t.grid = rgb(219, 225, 235);
        t.border = rgb(221, 226, 234);
        t.border_strong = rgb(192, 200, 214);
        t.text = rgb(22, 27, 36);
        t.text_dim = rgb(78, 88, 106);
        t.text_faint = rgb(124, 134, 151);
        t.disabled_text = rgb(168, 175, 188);
        t.accent = rgb(37, 99, 235);
        t.accent_hover = rgb(29, 84, 210);
        t.accent_text = rgb(255, 255, 255);
        t.accent_soft = rgb(225, 234, 253);
        t.accent_dim = rgb(122, 160, 240);
        t.ok = rgb(22, 150, 90);
        t.warning = rgb(190, 122, 6);
        t.error = rgb(212, 58, 58);
        t.focus = rgb(37, 99, 235);
        t.scrim = rgb(18, 24, 36);
        t.shadow = rgb(30, 40, 62);
        return t;
    }();
    return theme;
}

namespace {

// Point sizes in tenths, so the scale can use half points.
HFONT create_font_tenths(UINT dpi, int tenths, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(tenths, static_cast<int>(dpi), 720), 0, 0, 0, weight, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

}  // namespace

void FontSet::create(UINT dpi) {
    destroy();
    display = create_font_tenths(dpi, 220, FW_SEMIBOLD, L"Segoe UI");
    title = create_font_tenths(dpi, 140, FW_SEMIBOLD, L"Segoe UI");
    heading = create_font_tenths(dpi, 115, FW_SEMIBOLD, L"Segoe UI");
    body = create_font_tenths(dpi, 105, FW_NORMAL, L"Segoe UI");
    body_strong = create_font_tenths(dpi, 105, FW_SEMIBOLD, L"Segoe UI");
    small = create_font_tenths(dpi, 95, FW_NORMAL, L"Segoe UI");
    small_strong = create_font_tenths(dpi, 95, FW_SEMIBOLD, L"Segoe UI");
    caption = create_font_tenths(dpi, 90, FW_NORMAL, L"Segoe UI");
    mono = create_font_tenths(dpi, 95, FW_NORMAL, L"Consolas");
    metric = create_font_tenths(dpi, 180, FW_SEMIBOLD, L"Segoe UI");
    icon = create_font_tenths(dpi, 120, FW_NORMAL, L"Segoe MDL2 Assets");
    icon_small = create_font_tenths(dpi, 90, FW_NORMAL, L"Segoe MDL2 Assets");
    icon_large = create_font_tenths(dpi, 200, FW_NORMAL, L"Segoe MDL2 Assets");
}

void FontSet::destroy() {
    HFONT* fonts[] = {&display, &title,  &heading, &body, &body_strong, &small,     &small_strong,
                      &caption, &mono,   &metric,  &icon, &icon_small,  &icon_large};
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
    const Rgba accent = from_colorref(rgb(76, 141, 255), 1.0);
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
