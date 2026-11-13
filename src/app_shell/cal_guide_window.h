#pragma once

// The live calibration guide on the glasses: a borderless, topmost,
// non-activating, click-through window covering the glasses monitor while a
// head calibration runs (the engine is never running then, so the glasses
// otherwise show the plain extended desktop). Black is see-through on the
// birdbath optics. It never changes display settings or moves other windows;
// without a glasses monitor it simply does not appear.

#include "app_shell/calibration_guide.h"
#include "app_shell/ui_cal_guide.h"

#include <windows.h>

namespace gt {

class CalGuideWindow {
public:
    CalGuideWindow() = default;
    ~CalGuideWindow();
    CalGuideWindow(const CalGuideWindow&) = delete;
    CalGuideWindow& operator=(const CalGuideWindow&) = delete;

    // Call whenever the run changes (and from a slow timer as a backstop).
    // Opens, refreshes or closes the window to match cal_guide_view().
    void update(const CalRunState& run, double now_s, float fov_h_deg);
    void close();
    bool open() const { return hwnd_ != nullptr; }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    bool create(const RECT& monitor);
    void on_frame();
    void paint();

    HWND hwnd_ = nullptr;
    RECT monitor_{};
    CalGuideView view_;
    float fov_h_deg_ = kCalGuideDefaultFovDeg;
    ui::GlassesGuideFonts fonts_;
    CalGuidePoint ball_{};
    bool ball_valid_ = false;
    LARGE_INTEGER last_frame_{};
    double next_monitor_probe_s_ = 0.0;
};

}  // namespace gt
