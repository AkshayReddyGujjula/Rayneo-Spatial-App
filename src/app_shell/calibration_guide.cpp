#include "app_shell/calibration_guide.h"

#include <algorithm>
#include <cmath>

namespace gt {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSteadyRmsDegs = 1.0f;  // matches the tool's STEADY hint
constexpr float kOvershootDeg = 5.0f;
constexpr float kFill = 0.9f;
constexpr float kArcRadiusOfHalfHeight = 0.62f;

std::string times_text(int repeats) {
    if (repeats <= 1) {
        return std::string();
    }
    return " Do it " + std::to_string(repeats) + " times.";
}

CalGuideKind kind_for_step(const CalRunState& run) {
    if (run.flow != CalFlow::Orientation) {
        return CalGuideKind::Still;
    }
    switch (run.step) {
        case 2:
            return CalGuideKind::Turn;
        case 3:
            return CalGuideKind::Nod;
        case 4:
            return CalGuideKind::Tilt;
        default:
            return CalGuideKind::Still;
    }
}

}  // namespace

CalGuideTone cal_guide_tone(const CalLiveSample& sample) {
    if (sample.waiting) {
        return CalGuideTone::Neutral;
    }
    if (sample.kind == CalGuideKind::Still) {
        return sample.still_rms_degs < kSteadyRmsDegs ? CalGuideTone::Good : CalGuideTone::Warn;
    }
    if (sample.hint == "OFF_AXIS" || sample.hint == "WRONG_WAY" || sample.hint == "SLOWER") {
        return CalGuideTone::Warn;
    }
    return CalGuideTone::Good;
}

std::string cal_guide_title(CalGuideKind kind) {
    switch (kind) {
        case CalGuideKind::Still:
            return "Hold still";
        case CalGuideKind::Turn:
            return "Turn left and back";
        case CalGuideKind::Nod:
            return "Nod down and back";
        case CalGuideKind::Tilt:
            return "Tilt right and back";
    }
    return {};
}

std::string cal_guide_demo_text(CalGuideKind kind, int repeats_required) {
    switch (kind) {
        case CalGuideKind::Still:
            return "Sit upright, look at a fixed point straight ahead and keep your head still.";
        case CalGuideKind::Turn:
            return "Turn left as if saying no, chin level, until the ball passes the green mark (about 25 "
                   "degrees), then come back to centre." +
                   times_text(repeats_required);
        case CalGuideKind::Nod:
            return "Nod down as if saying yes, facing forward, until the ball reaches the green mark (about 20 "
                   "degrees), then come back up." +
                   times_text(repeats_required);
        case CalGuideKind::Tilt:
            return "Bring your right ear toward your right shoulder, nose pointing ahead, until the ball reaches "
                   "the green mark (about 20 degrees), then come back." +
                   times_text(repeats_required);
    }
    return {};
}

std::string cal_guide_hint_text(CalGuideKind kind, const std::string& hint) {
    const bool turn = kind == CalGuideKind::Turn;
    const bool nod = kind == CalGuideKind::Nod;
    if (hint == "GET_READY") {
        return "Get ready";
    }
    if (hint == "STEADY") {
        return "Good, stay still";
    }
    if (hint == "HOLD") {
        return "Hold still";
    }
    if (hint == "GO") {
        return turn ? "Turn left" : nod ? "Nod down" : "Tilt right";
    }
    if (hint == "FURTHER") {
        return turn ? "Turn further" : nod ? "Nod further" : "Tilt further";
    }
    if (hint == "BACK_TO_CENTRE") {
        return "Back to centre";
    }
    if (hint == "SLOWER") {
        return "Slower";
    }
    if (hint == "OFF_AXIS") {
        return turn ? "Keep your head level" : nod ? "Nod straight down" : "Keep facing forward";
    }
    if (hint == "WRONG_WAY") {
        return turn ? "Other way: turn left" : nod ? "Other way: nod down" : "Other way: tilt right";
    }
    if (hint == "DONE") {
        return "Done, stay at centre";
    }
    return {};
}

CalGuideScale cal_guide_scale(CalGuideKind kind, float width, float height, float fov_h_deg, float target_deg) {
    CalGuideScale scale;
    const float fov = fov_h_deg > 5.0f && fov_h_deg < 180.0f ? fov_h_deg : kCalGuideDefaultFovDeg;
    const float true_ppd = std::max(1.0f, width) / fov;
    const float reach = (kind == CalGuideKind::Still ? 10.0f : std::max(target_deg, 1.0f)) + kOvershootDeg;
    float half = 0.5f * std::min(width, height);
    if (kind == CalGuideKind::Turn) {
        half = 0.5f * width;
    } else if (kind == CalGuideKind::Nod || kind == CalGuideKind::Tilt) {
        half = 0.5f * height;
    }
    scale.px_per_deg = std::max(0.5f, std::min(true_ppd, kFill * half / reach));
    scale.arc_radius = kArcRadiusOfHalfHeight * 0.5f * std::min(height, width);
    scale.arc_gain = 2.0f;
    return scale;
}

CalGuidePoint cal_guide_axis_point(CalGuideKind kind, float deg, const CalGuideScale& scale, float cx, float cy) {
    switch (kind) {
        case CalGuideKind::Turn:
            return {cx - deg * scale.px_per_deg, cy};
        case CalGuideKind::Nod:
            return {cx, cy + deg * scale.px_per_deg};
        case CalGuideKind::Tilt: {
            const float a = deg * scale.arc_gain * kPi / 180.0f;
            return {cx + scale.arc_radius * std::sin(a), cy - scale.arc_radius * std::cos(a)};
        }
        case CalGuideKind::Still:
            break;
    }
    return {cx, cy};
}

CalGuidePoint cal_guide_ball(const CalLiveSample& sample, const CalGuideScale& scale, float cx, float cy,
                             float width, float height) {
    CalGuidePoint point{cx - sample.yaw_deg * scale.px_per_deg, cy - sample.pitch_deg * scale.px_per_deg};
    if (sample.kind == CalGuideKind::Tilt) {
        const CalGuidePoint on_arc = cal_guide_axis_point(CalGuideKind::Tilt, sample.roll_deg, scale, cx, cy);
        point.x = on_arc.x - sample.yaw_deg * scale.px_per_deg;
        point.y = on_arc.y - sample.pitch_deg * scale.px_per_deg;
    }
    const float margin = 0.04f * std::min(width, height);
    point.x = std::clamp(point.x, cx - 0.5f * width + margin, cx + 0.5f * width - margin);
    point.y = std::clamp(point.y, cy - 0.5f * height + margin, cy + 0.5f * height - margin);
    return point;
}

float cal_guide_ring_radius(float still_rms_degs, float gate_degs, float r_min, float r_max) {
    const float gate = gate_degs > 0.0f ? gate_degs : 1.5f;
    const float t = std::clamp(std::isfinite(still_rms_degs) ? still_rms_degs / gate : 1.0f, 0.0f, 1.0f);
    return r_min + (r_max - r_min) * t;
}

bool cal_guide_ease_ball(CalGuidePoint& ball, const CalGuidePoint& target, double dt_s) {
    const CalGuidePoint before = ball;
    const double dt = std::isfinite(dt_s) ? std::max(0.0, dt_s) : 0.0;
    const float k = static_cast<float>(1.0 - std::exp(-dt / kCalGuideBallTauS));
    ball.x += (target.x - ball.x) * k;
    ball.y += (target.y - ball.y) * k;
    if (std::fabs(target.x - ball.x) < 0.5f && std::fabs(target.y - ball.y) < 0.5f) {
        ball = target;
    }
    return ball.x != before.x || ball.y != before.y;
}

CalGuideView cal_guide_view(const CalRunState& run, double now_s) {
    CalGuideView view;
    view.visible = run.running() && run.tool_has_live && run.flow == CalFlow::Orientation;
    view.has_live = run.live_fresh(now_s);
    view.step = run.step;
    view.steps = run.steps;
    if (view.has_live) {
        view.live = run.live;
    } else {
        view.live.kind = kind_for_step(run);
        view.live.step = run.step;
        view.live.waiting = run.state == CalStepState::Waiting;
        view.live.repeats_required = view.live.kind == CalGuideKind::Still ? 0 : kCalHeadRepeats;
    }
    const CalGuideKind kind = view.live.kind;
    view.title = cal_guide_title(kind);
    view.tip = run.retry_tip;

    double countdown = run.countdown_s(now_s);
    if (view.has_live && view.live.waiting && view.live.remaining_s >= 0.0f) {
        countdown = view.live.remaining_s;
    }
    if (run.has_event && run.state == CalStepState::Waiting && countdown >= 0.0) {
        view.countdown = static_cast<int>(std::ceil(countdown - 1e-6));
    }

    if (!run.has_event) {
        view.headline = "Starting";
        view.subline = "Put the glasses on and sit upright.";
        return view;
    }
    switch (run.state) {
        case CalStepState::Waiting:
            view.headline = view.title;
            view.subline = cal_guide_demo_text(kind, view.live.repeats_required);
            break;
        case CalStepState::Recording:
            view.headline = view.has_live ? cal_guide_hint_text(kind, view.live.hint) : std::string();
            if (view.headline.empty()) {
                view.headline = view.title;
            }
            if (view.has_live && kind != CalGuideKind::Still && view.live.repeats_required > 0) {
                view.subline = std::to_string(std::min(view.live.repeats, view.live.repeats_required)) + " of " +
                               std::to_string(view.live.repeats_required);
            }
            break;
        case CalStepState::Ok:
            view.headline = "Step done";
            view.tone = CalGuideTone::Good;
            break;
        case CalStepState::Retry:
            view.headline = "Let's try that again";
            view.subline = run.retry_reason;
            view.tone = CalGuideTone::Warn;
            break;
        case CalStepState::Failed:
        case CalStepState::Done:
            break;
    }
    if (view.has_live && run.state != CalStepState::Ok && run.state != CalStepState::Retry) {
        view.tone = cal_guide_tone(view.live);
    }
    return view;
}

}  // namespace gt
