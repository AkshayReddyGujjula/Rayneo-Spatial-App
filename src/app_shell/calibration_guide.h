#pragma once

// Pure model of the live calibration guide (no Win32 drawing): what the guide
// says, where its ball sits and what colour it is. The same view is drawn
// large on the glasses (cal_guide_window) and small in the PC calibration
// panel (ui_cal_guide). See docs/overnight/r5-calibration-guide.md.

#include "app_shell/calibration_progress.h"

#include <string>

namespace gt {

// The glasses' horizontal field of view when no layout says otherwise
// (Layout::fov_deg default).
inline constexpr float kCalGuideDefaultFovDeg = 46.0f;

enum class CalGuideTone : int {
    Neutral = 0,  // countdown, between steps
    Good,         // on the axis / steady
    Warn,         // off-axis, wrong way, too fast, moving during the still step
};

CalGuideTone cal_guide_tone(const CalLiveSample& sample);

// Short step name shown above the guide ("Turn left and back").
std::string cal_guide_title(CalGuideKind kind);
// How to do the step, in one or two plain sentences (shown during the countdown).
std::string cal_guide_demo_text(CalGuideKind kind, int repeats_required);
// The big live instruction for a hint code ("Turn further", "Keep your head
// level", "Back to centre", "Hold still"); unknown codes give "".
std::string cal_guide_hint_text(CalGuideKind kind, const std::string& hint);

// Degrees -> pixels. The scale follows the glasses' field of view (pixels per
// degree of the real display) but is reduced when needed so the step's
// recommended target plus 5 deg of overshoot still fits inside 90% of the
// half-extent along the motion axis; the same scale serves both axes so the
// ball's off-axis displacement is to scale. The tilt step draws an arc whose
// angle is the head roll times arc_gain.
struct CalGuideScale {
    float px_per_deg = 1.0f;
    float arc_radius = 0.0f;
    float arc_gain = 2.0f;
};
CalGuideScale cal_guide_scale(CalGuideKind kind, float width, float height, float fov_h_deg, float target_deg);

struct CalGuidePoint {
    float x = 0.0f;
    float y = 0.0f;
};
// The ball: where the head points relative to the step's start pose (turn
// left -> left, nod down -> down; tilt right -> clockwise along the arc),
// clamped inside the area with margin.
CalGuidePoint cal_guide_ball(const CalLiveSample& sample, const CalGuideScale& scale, float cx, float cy,
                             float width, float height);
// A point `deg` along the step's commanded direction (for target ticks).
CalGuidePoint cal_guide_axis_point(CalGuideKind kind, float deg, const CalGuideScale& scale, float cx, float cy);
// Still step: the steadiness ring tightens from r_max (at or above the 1.5
// deg/s gate) to r_min (motionless).
float cal_guide_ring_radius(float still_rms_degs, float gate_degs, float r_min, float r_max);

// Everything one frame of the guide needs.
struct CalGuideView {
    bool visible = false;  // a head calibration (or the compass's still step) is running
    bool has_live = false;
    CalLiveSample live;
    int step = 0;
    int steps = 0;
    std::string title;     // step name
    std::string headline;  // the big line
    std::string subline;   // demo text, retry reason, or "Good" feedback
    std::string tip;       // a specific correction after a retry
    int countdown = -1;    // whole seconds before a waiting step starts, -1 none
    CalGuideTone tone = CalGuideTone::Neutral;
};
CalGuideView cal_guide_view(const CalRunState& run, double now_s);

}  // namespace gt
