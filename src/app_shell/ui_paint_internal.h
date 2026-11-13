#pragma once

// Shared plumbing for the page painters (ui_paint.cpp, ui_pages.cpp,
// ui_page_layout.cpp, ui_onboarding.cpp). Not part of the public paint API.

#include "app_shell/app_state.h"
#include "app_shell/ui_help.h"
#include "app_shell/ui_paint.h"
#include "app_shell/ui_widgets.h"

#include <string>
#include <vector>

namespace gt::ui::detail {

inline RECT rect_of(int left, int top, int width, int height) {
    return RECT{left, top, left + width, top + height};
}
inline int rw(const RECT& r) {
    return static_cast<int>(r.right - r.left);
}
inline int rh(const RECT& r) {
    return static_cast<int>(r.bottom - r.top);
}

// Help-mark id panels (ui_help_id(panel, row)).
enum HelpPanel : int {
    kHelpStatus = 0,    // health rows (Home readiness, Diagnostics)
    kHelpEngine = 1,    // engine controls, comfort and tracking options
    kHelpTools = 2,     // recovery, calibration and settings actions
    kHelpLayout = 3,    // presets and screen management
    kHelpFields = 4,    // selected-screen sliders
    kHelpEditor = 5,    // arrangement editor chrome
    kHelpHeader = 6,    // page header actions
};

// Fixed rows inside kHelpEngine.
enum EngineHelpRow : int {
    kRowStartWorkspace = 0,
    kRowStartPreview = 1,
    kRowStop = 2,
    kRowRecenter = 3,
    kRowReload = 4,
    kRowYaw = 5,
    kRowPitch = 6,
    kRowNoImu = 7,
    kRowStabilise = 8,
    kRowDimMode = 9,
    kRowBrightness = 10,  // + screen index (8 rows)
    kRowFocusDim = 18,
    kRowNightTint = 19,
    kRowNightWarmth = 20,
    kRowCursor = 21,
    kRowPrediction = 22,
    kRowHeadingLock = 23,
    kRowMagTrack = 24,
};

// Fixed rows inside kHelpTools.
enum ToolsHelpRow : int {
    kRowCalConsole = 0,
    kRowEngineLog = 1,
    kRowLogsFolder = 2,
    kRowCloseToTray = 3,
    kRowRecover = 4,
    kRowCalHead = 5,
    kRowCalMag = 6,
    kRowTheme = 7,
    kRowSetupGuide = 8,
    kRowCalEarbuds = 9,
};

// One paint pass: the canvas, the state, fonts/theme, the hotspot sink and
// the scroll portal hotspots are currently clipped to.
struct Ui {
    Canvas& c;
    AppState& s;
    const FontSet& f;
    const Theme& t;
    UINT dpi;
    std::vector<Hotspot>& hs;
    int panel = -1;  // ScrollPanel of the open scroll area, -1 outside
    RECT view{};

    int px(int value) const { return scale_px(value, dpi); }
    // Registers an interactive element (clipped to the open scroll view).
    void hot(int id, int arg, const RECT& rect, bool enabled = true);
    // Hover/press/focus for an element, eased through the animator.
    WidgetState ws(int id, bool enabled = true);
    float track(uint32_t channel, int id, float target, float tau = anim::kTauHover);
    // A "?" mark; registers ui_help_id(panel, row) -> topic.
    void help(int help_panel, int row, HelpTopic topic, const RECT& rect);
};

struct ScrollArea {
    ScrollPanel panel = kScrollPage;
    RECT frame{};  // panel rect (wheel target)
    RECT view{};   // clipped content rect
    int offset = 0;
    int saved_panel = -1;
    RECT saved_view{};
};

// Opens a clipped scroll area; content is laid out from view.top - offset.
ScrollArea begin_scroll(Ui& ui, ScrollPanel panel, const RECT& frame, const RECT& view);
// Closes it with the measured content height: updates the portal cache,
// draws the scrollbar and requests a repaint if the offset had to clamp.
void end_scroll(Ui& ui, ScrollArea& area, int content_h);
void hide_scroll(Ui& ui, ScrollPanel panel);

// Building blocks used by several pages.
void card(Ui& ui, const RECT& rect);
// Card title (+ optional one-line description). Returns the y below it.
int card_header(Ui& ui, const RECT& card_rect, const wchar_t* title,
                const wchar_t* description = nullptr);
// Wrapped paragraph; returns its height.
int paragraph(Ui& ui, const RECT& area, HFONT font, COLORREF color, const std::wstring& text);
void button(Ui& ui, int id, const RECT& rect, const std::wstring& label, ButtonStyle style,
            bool enabled = true, const wchar_t* icon = nullptr);
// Title + description on the left, a switch on the right, help mark after.
// Returns the row height.
int switch_row(Ui& ui, int id, int left, int top, int width, const wchar_t* title,
               const std::wstring& description, bool value, bool enabled, int help_panel,
               int help_row, HelpTopic topic);
void segmented(Ui& ui, int base_id, const RECT& rect, const wchar_t* const* labels, int count,
               int selected, bool enabled = true);
void slider(Ui& ui, int id, int arg, const RECT& rect, const std::wstring& label,
            const std::wstring& value_text, float fraction, bool enabled, bool dragging);

// Engine state as the shell presents it.
struct EngineView {
    COLORREF color = 0;
    std::wstring label;     // short: "Running", "Stopped"
    std::wstring headline;  // hero line
    std::wstring detail;
    bool busy = false;
    bool transitional = false;  // starting / stopping: pulse
};
EngineView engine_view(const AppState& state, const Theme& theme);
float pulse_phase(Ui& ui, bool active);

// The sidebar's always-visible engine card: label and dot colour.
struct StatusCard {
    std::wstring label;
    COLORREF dot = 0;
    bool pulse = false;
};
StatusCard status_card(const EngineView& view, const AppState& state, const Theme& theme);

// Readiness checks shared by Home, Diagnostics and the setup guide.
struct HealthItem {
    const wchar_t* title = L"";
    std::wstring value;
    std::wstring detail;  // plain-language next step (Home, setup guide)
    std::wstring raw;     // the diagnostic's own text (Diagnostics page)
    Severity severity = Severity::Neutral;
    HelpTopic topic = HelpTopic::StatusEngine;
    int status_row = 0;  // index for ui_help_id(kHelpStatus, row)
};
std::vector<HealthItem> health_items(const AppState& state);
COLORREF severity_colour(const Theme& theme, Severity severity);
const wchar_t* severity_glyph(Severity severity);

// Calibration progress block (setup guide and Tracking page). Returns the
// height used; with draw == false it only measures.
int paint_calibration_run(Ui& ui, int left, int top, int width, bool draw);

// "id  yaw°" label of a screen in the arrangement editor (2D and 3D).
std::wstring editor_screen_label(const ScreenLayout& screen);

// Small top-down arc thumbnail of a layout (Home, setup guide).
void paint_arc_preview(Ui& ui, const RECT& rect, const Layout& layout, int selected);

// Page painters. content is the area right of the sidebar below the header.
void paint_home(Ui& ui, const RECT& content);
void paint_layout_page(Ui& ui, const RECT& content);
void paint_comfort(Ui& ui, const RECT& content);
void paint_tracking(Ui& ui, const RECT& content);
void paint_diagnostics(Ui& ui, const RECT& content);
void paint_settings(Ui& ui, const RECT& content);
// Header actions for the pages that have them (right-aligned at right).
void paint_layout_header_actions(Ui& ui, int right, int top, int height);
void paint_wizard(Ui& ui, const RECT& client);

// Matches the VERSIONINFO block in resources/rayneo_spatial.rc.
inline constexpr const wchar_t* kAppVersion = L"1.0.0";

}  // namespace gt::ui::detail
