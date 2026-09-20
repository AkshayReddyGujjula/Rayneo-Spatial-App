#pragma once

#include "layout/layout.h"

#include <array>

namespace gt {

struct ScreenGeometryVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

std::array<ScreenGeometryVertex, 6> make_screen_quad(const ScreenLayout& screen);

}  // namespace gt
