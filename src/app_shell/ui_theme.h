#pragma once

// Design tokens for the controller UI: colour themes (dark and light), the
// spacing, radius and size scales, the type scale, and the icon glyphs.
// No image assets and no remote resources: every pixel is drawn with
// GDI/GDI+, and icons come from the Segoe MDL2 Assets font that ships with
// Windows 10 and 11.

#include <windows.h>

#ifdef small
#undef small
#endif

#include <string>

namespace gt::ui {

inline constexpr COLORREF rgb(unsigned red, unsigned green, unsigned blue) {
    return static_cast<COLORREF>(red | (green << 8) | (blue << 16));
}

// Linear mix: t = 0 gives a, t = 1 gives b.
COLORREF mix(COLORREF a, COLORREF b, float t);

// One complete colour scheme. Widgets never hard-code a colour: they read
// the theme the Canvas carries, so dark and light share every code path.
struct Theme {
    bool dark = true;
    COLORREF window = 0;         // content background
    COLORREF sidebar = 0;        // navigation rail
    COLORREF surface = 0;        // cards
    COLORREF surface_alt = 0;    // inset controls, tracks, chips
    COLORREF surface_hover = 0;  // hovered rows and controls
    COLORREF canvas = 0;         // the arrangement editor
    COLORREF grid = 0;           // editor rings and grid
    COLORREF border = 0;
    COLORREF border_strong = 0;
    COLORREF text = 0;
    COLORREF text_dim = 0;
    COLORREF text_faint = 0;
    COLORREF disabled_text = 0;
    COLORREF accent = 0;       // accent as text, icons, tracks and outlines
    COLORREF accent_fill = 0;  // primary button fill (accent_text on it meets WCAG AA)
    COLORREF accent_hover = 0; // hovered primary button fill
    COLORREF accent_text = 0;  // label colour on an accent fill
    COLORREF accent_soft = 0;  // tinted background (selected chip, nav item)
    COLORREF accent_dim = 0;   // filled slider track
    COLORREF ok = 0;
    COLORREF warning = 0;
    COLORREF error = 0;
    COLORREF focus = 0;
    COLORREF scrim = 0;   // dim layer behind the setup guide
    COLORREF shadow = 0;  // card shadow tint
};

const Theme& dark_theme();
const Theme& light_theme();

// Spacing scale (96-dpi pixels; scale with scale_px).
namespace space {
inline constexpr int xxs = 2;
inline constexpr int xs = 4;
inline constexpr int sm = 8;
inline constexpr int md = 12;
inline constexpr int lg = 16;
inline constexpr int xl = 24;
inline constexpr int xxl = 32;
inline constexpr int xxxl = 48;
}  // namespace space

namespace radius {
inline constexpr int sm = 6;
inline constexpr int md = 8;
inline constexpr int lg = 12;
}  // namespace radius

// Component sizes (96-dpi pixels).
namespace size {
inline constexpr int control = 40;     // standard button / segmented height
inline constexpr int control_sm = 32;  // compact buttons and chips
inline constexpr int control_lg = 46;  // hero actions
inline constexpr int nav_item = 44;
inline constexpr int sidebar = 236;
inline constexpr int sidebar_compact = 72;
inline constexpr int slider = 58;  // label row + track
inline constexpr int list_row = 60;
inline constexpr int switch_w = 40;
inline constexpr int switch_h = 22;
inline constexpr int help = 20;
}  // namespace size

int scale_px(int value, UINT dpi);

// Type scale. Sizes in points (tenths below), Segoe UI throughout.
struct FontSet {
    HFONT display = nullptr;       // page titles, 22 pt semibold
    HFONT title = nullptr;         // card titles and hero lines, 14 pt semibold
    HFONT heading = nullptr;       // section labels, 11.5 pt semibold
    HFONT body = nullptr;          // 10.5 pt
    HFONT body_strong = nullptr;   // 10.5 pt semibold
    HFONT small = nullptr;         // 9.5 pt
    HFONT small_strong = nullptr;  // 9.5 pt semibold
    HFONT caption = nullptr;       // 9 pt
    HFONT mono = nullptr;          // 9.5 pt Consolas
    HFONT metric = nullptr;        // 18 pt semibold numbers
    HFONT icon = nullptr;          // Segoe MDL2 Assets, 12 pt
    HFONT icon_small = nullptr;    // Segoe MDL2 Assets, 9 pt
    HFONT icon_large = nullptr;    // Segoe MDL2 Assets, 20 pt

    void create(UINT dpi);
    void destroy();
};

// Segoe MDL2 Assets code points.
namespace glyph {
inline constexpr const wchar_t* home = L"";
inline constexpr const wchar_t* layout = L"";
inline constexpr const wchar_t* comfort = L"";
inline constexpr const wchar_t* tracking = L"";
inline constexpr const wchar_t* diagnostics = L"";
inline constexpr const wchar_t* settings = L"";
inline constexpr const wchar_t* play = L"";
inline constexpr const wchar_t* stop = L"";
inline constexpr const wchar_t* refresh = L"";
inline constexpr const wchar_t* recenter = L"";
inline constexpr const wchar_t* check = L"";
inline constexpr const wchar_t* warning = L"";
inline constexpr const wchar_t* error = L"";
inline constexpr const wchar_t* info = L"";
inline constexpr const wchar_t* close = L"";
inline constexpr const wchar_t* add = L"";
inline constexpr const wchar_t* remove = L"";
inline constexpr const wchar_t* save = L"";
inline constexpr const wchar_t* undo = L"";
inline constexpr const wchar_t* fullscreen = L"";
inline constexpr const wchar_t* exit_fullscreen = L"";
inline constexpr const wchar_t* folder = L"";
inline constexpr const wchar_t* document = L"";
inline constexpr const wchar_t* chevron_right = L"";
inline constexpr const wchar_t* glasses = L"";
inline constexpr const wchar_t* compass = L"";
inline constexpr const wchar_t* preview = L"";
inline constexpr const wchar_t* cursor = L"";
inline constexpr const wchar_t* sparkle = L"";
}  // namespace glyph

// Procedurally drawn application icon: a rounded plate with a blue arc and
// three screen marks. Drawn into a 32-bit DIB so it carries real alpha and
// needs no .ico binary in the repository.
HICON create_app_icon(int size);

}  // namespace gt::ui
