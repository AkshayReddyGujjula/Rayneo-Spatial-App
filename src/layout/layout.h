#pragma once

#include <array>
#include <string>
#include <vector>

namespace gt {

struct ScreenLayout {
    std::string id;
    int vdd_index = 0;
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;
    float distance_m = 2.0f;
    float width_m = 1.6f;
    float height_m = 0.9f;
    std::array<float, 3> color{0.24f, 0.45f, 0.95f};
};

struct CapturePolicy {
    int active_fps = 120;
    int mid_fps = 30;
    int idle_fps = 1;
    float enter_deg = 30.0f;
    float leave_deg = 22.0f;
};

struct Layout {
    int version = 1;
    float fov_deg = 46.0f;
    std::vector<ScreenLayout> screens;
    CapturePolicy capture_policy;
};

Layout default_layout();
bool validate_layout(const Layout& layout, std::string& error);
bool load_layout(const std::string& path, Layout& layout, std::string& error);
bool save_layout(const std::string& path, const Layout& layout, std::string& error);

}  // namespace gt
