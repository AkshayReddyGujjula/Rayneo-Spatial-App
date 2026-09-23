#pragma once

// Dark neutral palette with a single restrained blue accent, plus the fonts the
// whole UI shares. No image assets and no remote resources: every pixel is
// drawn with GDI, so the portable package has nothing to fetch or ship.

#include <windows.h>

#ifdef small
#undef small
#endif

#include <string>

namespace gt::ui {

inline constexpr COLORREF rgb(unsigned red, unsigned green, unsigned blue) {
    return static_cast<COLORREF>(red | (green << 8) | (blue << 16));
}

namespace palette {
inline constexpr COLORREF background = rgb(15, 19, 26);
inline constexpr COLORREF panel = rgb(23, 28, 37);
inline constexpr COLORREF panel_alt = rgb(29, 35, 48);
inline constexpr COLORREF canvas = rgb(12, 15, 21);
inline constexpr COLORREF border = rgb(38, 46, 60);
inline constexpr COLORREF border_strong = rgb(52, 62, 80);
inline constexpr COLORREF text = rgb(230, 234, 242);
inline constexpr COLORREF text_dim = rgb(152, 162, 179);
inline constexpr COLORREF text_faint = rgb(107, 118, 136);
inline constexpr COLORREF accent = rgb(76, 141, 255);
inline constexpr COLORREF accent_soft = rgb(28, 41, 66);
inline constexpr COLORREF accent_dim = rgb(43, 78, 143);
inline constexpr COLORREF ok = rgb(63, 178, 127);
inline constexpr COLORREF warning = rgb(224, 169, 59);
inline constexpr COLORREF error = rgb(224, 96, 94);
inline constexpr COLORREF focus = rgb(130, 176, 255);
inline constexpr COLORREF disabled_text = rgb(88, 96, 110);
}  // namespace palette

int scale_px(int value, UINT dpi);

struct FontSet {
    HFONT title = nullptr;
    HFONT heading = nullptr;
    HFONT body = nullptr;
    HFONT small = nullptr;
    HFONT mono = nullptr;

    void create(UINT dpi);
    void destroy();
};

// Procedurally drawn application icon: a dark rounded square with a blue arc
// and three screen marks. Drawn into a 32-bit DIB so it carries real alpha and
// needs no .ico binary in the repository.
HICON create_app_icon(int size);

}  // namespace gt::ui
