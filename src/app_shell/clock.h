#pragma once

// Monotonic seconds since an unspecified epoch, shared by the controller's
// timers and diagnostics.
#include <chrono>

namespace gt {

inline double steady_now_s() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace gt
