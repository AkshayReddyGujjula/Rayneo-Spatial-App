#include "app_shell/ui_anim.h"

#include <algorithm>
#include <cmath>

namespace gt::anim {

float ease_out_cubic(float t) {
    const float x = 1.0f - std::clamp(t, 0.0f, 1.0f);
    return 1.0f - x * x * x;
}

float ease_in_out_cubic(float t) {
    const float x = std::clamp(t, 0.0f, 1.0f);
    if (x < 0.5f) {
        return 4.0f * x * x * x;
    }
    const float y = -2.0f * x + 2.0f;
    return 1.0f - y * y * y / 2.0f;
}

float ease_out_back(float t) {
    const float x = std::clamp(t, 0.0f, 1.0f);
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float y = x - 1.0f;
    return 1.0f + c3 * y * y * y + c1 * y * y;
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float approach(float current, float target, float dt_s, float tau_s, float epsilon) {
    if (!(tau_s > 0.0f) || !(dt_s > 0.0f)) {
        return dt_s > 0.0f ? target : current;
    }
    const float blend = 1.0f - std::exp(-dt_s / tau_s);
    const float next = current + (target - current) * blend;
    if (std::fabs(target - next) <= epsilon) {
        return target;
    }
    return next;
}

float Transition::progress(double now_s) const {
    if (start_s < 0.0 || !(duration_s > 0.0)) {
        return 1.0f;
    }
    const double t = (now_s - start_s) / duration_s;
    return static_cast<float>(std::clamp(t, 0.0, 1.0));
}

void Animator::begin_frame() {
    ++frame_;
    wants_frames_ = false;
}

void Animator::end_frame() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.seen != frame_) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

float Animator::track(uint64_t id, float target, float tau_s) {
    auto found = entries_.find(id);
    if (found == entries_.end()) {
        Entry entry;
        entry.value = target;
        entry.target = target;
        entry.tau = tau_s;
        entry.seen = frame_;
        entries_.emplace(id, entry);
        return target;
    }
    Entry& entry = found->second;
    entry.target = target;
    entry.tau = tau_s;
    entry.seen = frame_;
    if (reduce_motion_) {
        entry.value = target;
    }
    return entry.value;
}

void Animator::snap(uint64_t id, float value) {
    Entry& entry = entries_[id];
    entry.value = value;
    entry.target = value;
    entry.seen = frame_;
}

bool Animator::step(double dt_s) {
    const float dt = static_cast<float>(std::clamp(dt_s, 0.0, 0.1));
    for (auto& [id, entry] : entries_) {
        (void)id;
        entry.value = reduce_motion_ ? entry.target
                                     : approach(entry.value, entry.target, dt, entry.tau);
    }
    return animating();
}

bool Animator::animating() const {
    for (const auto& [id, entry] : entries_) {
        (void)id;
        if (entry.value != entry.target) {
            return true;
        }
    }
    return false;
}

}  // namespace gt::anim
