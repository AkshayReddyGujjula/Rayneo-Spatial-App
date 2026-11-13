#pragma once

// Immediate-mode animation for the custom-drawn UI.
//
// Widgets declare a target every paint (track(key, target)); the animator
// eases each value toward its target on the animation timer. A value seen
// for the first time snaps (nothing animates in from nowhere), keys that a
// paint did not track are dropped, and the window runs its timer only while
// animating() or wants_frames() is true, so an idle window burns no CPU.
// With reduce_motion (Windows "Show animations" off) every value snaps.

#include <cstdint>
#include <unordered_map>

namespace gt::anim {

inline constexpr float kPi = 3.14159265358979323846f;

// Easing curves over t in [0, 1] (clamped).
float ease_out_cubic(float t);
float ease_in_out_cubic(float t);
float ease_out_back(float t);  // slight overshoot, for small pops
float lerp(float a, float b, float t);

// Frame-rate independent exponential approach: after tau_s the remaining
// distance is 1/e. Lands exactly on the target once within epsilon.
float approach(float current, float target, float dt_s, float tau_s,
               float epsilon = 0.0015f);

// Time-based one-shot transition (page change, toast entrance).
struct Transition {
    double start_s = -1.0;
    double duration_s = 0.22;

    void begin(double now_s, double duration) {
        start_s = now_s;
        duration_s = duration;
    }
    // 0..1 linear progress; 1 when never started or finished.
    float progress(double now_s) const;
    bool active(double now_s) const { return start_s >= 0.0 && progress(now_s) < 1.0f; }
};

// Channel namespaces for keys (one value per channel and element id).
enum Channel : uint32_t {
    kHover = 1,
    kPress = 2,
    kFocus = 3,
    kToggle = 4,
    kSegment = 5,
    kSlider = 6,
    kProgress = 7,
    kNav = 8,
    kMisc = 9,
};

inline uint64_t key(uint32_t channel, int32_t id) {
    return (static_cast<uint64_t>(channel) << 32) | static_cast<uint32_t>(id);
}

inline constexpr float kTauHover = 0.06f;
inline constexpr float kTauToggle = 0.07f;
inline constexpr float kTauSlide = 0.08f;
inline constexpr float kTauProgress = 0.20f;

// Whether the window's animation timer should run. Something must be moving
// AND the window must be able to paint: a hidden (tray) or minimized window
// receives no WM_PAINT, so a frame request (cleared only by the next paint)
// would never clear and the timer would tick at 66 Hz for as long as the
// window stays in the tray. Showing the window paints, which re-evaluates.
struct AnimationDemand {
    bool window_paints = true;  // visible and not minimized
    bool starting = false;      // engine start in progress
    bool toast = false;         // a toast is on screen
    bool animating = false;     // Animator::animating()
    bool wants_frames = false;  // Animator::wants_frames()
    bool transition = false;    // page transition running
    bool fade_pending = false;  // the old page's snapshot is still held
};
bool animation_timer_wanted(const AnimationDemand& demand);

class Animator {
public:
    // Call at the start of each paint; keys not tracked before the next
    // begin_frame() are dropped by end_frame().
    void begin_frame();
    void end_frame();
    // Registers the target and returns the current eased value.
    float track(uint64_t id, float target, float tau_s = kTauHover);
    // Jumps a value (e.g. while a slider is being dragged).
    void snap(uint64_t id, float value);
    // Advances every value; returns animating().
    bool step(double dt_s);
    bool animating() const;
    // A paint that shows a time-driven effect (pulse, spinner) asks for
    // frames; cleared by begin_frame().
    void request_frames() { wants_frames_ = !reduce_motion_; }
    bool wants_frames() const { return wants_frames_; }

    void set_reduce_motion(bool reduce) { reduce_motion_ = reduce; }
    bool reduce_motion() const { return reduce_motion_; }
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        float value = 0.0f;
        float target = 0.0f;
        float tau = kTauHover;
        uint32_t seen = 0;
    };
    std::unordered_map<uint64_t, Entry> entries_;
    uint32_t frame_ = 1;
    bool wants_frames_ = false;
    bool reduce_motion_ = false;
};

}  // namespace gt::anim
