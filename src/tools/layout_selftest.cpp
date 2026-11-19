#include "layout/layout.h"
#include "render/screen_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <process.h>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) {
        ++failures;
    }
}

float distance(const gt::ScreenGeometryVertex& a, const gt::ScreenGeometryVertex& b) {
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    const float z = a.z - b.z;
    return std::sqrt(x * x + y * y + z * z);
}

}  // namespace

int main() {
    std::printf("layout_selftest: persistence and validation\n");
    const gt::Layout original = gt::default_layout();
    std::string error;
    check(gt::validate_layout(original, error), "default layout is valid");

    // Per-process name: concurrent runs (two build trees testing at once)
    // must not share the file or its ".tmp" sibling.
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("rayneo-layout-selftest-" + std::to_string(_getpid()) + ".json");
    check(gt::save_layout(path.string(), original, error), "layout saves atomically");
    gt::Layout loaded;
    check(gt::load_layout(path.string(), loaded, error), "layout loads");
    check(loaded.screens.size() == 3 && loaded.screens[1].id == "centre" &&
              std::fabs(loaded.screens[2].yaw_deg - 45.0f) < 1e-6f,
          "layout round-trip preserves screens");

    std::printf("layout_selftest: portrait screens\n");
    const auto read_text = [](const std::filesystem::path& file) {
        std::ifstream input(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const auto count_of = [](const std::string& text, const std::string& needle) {
        size_t count = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
            ++count;
        }
        return count;
    };
    check(count_of(read_text(path), "portrait") == 0, "a landscape-only layout writes no portrait key");
    check(!loaded.screens[0].portrait && !loaded.screens[1].portrait && !loaded.screens[2].portrait,
          "an absent portrait key loads as false");
    gt::Layout tall = original;
    tall.screens[0].portrait = true;
    std::swap(tall.screens[0].width_m, tall.screens[0].height_m);
    check(gt::save_layout(path.string(), tall, error), "a layout with a portrait screen saves");
    check(count_of(read_text(path), "\"portrait\": true") == 1 && count_of(read_text(path), "portrait") == 1,
          "only the portrait screen writes the key (true), false is never written");
    gt::Layout tall_loaded;
    check(gt::load_layout(path.string(), tall_loaded, error) && tall_loaded.screens.size() == 3 &&
              tall_loaded.screens[0].portrait && !tall_loaded.screens[1].portrait &&
              tall_loaded.screens[0].width_m == tall.screens[0].width_m &&
              tall_loaded.screens[0].height_m == tall.screens[0].height_m,
          "portrait round-trips with its swapped size");
    {
        // A hand-written portrait screen without height_m defaults to 9:16, not 16:9.
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << R"({"version": 1, "fov_deg": 46.0, "screens": [)"
               << R"({"id": "side", "vdd_index": 1, "yaw_deg": -45, "pitch_deg": 0, "roll_deg": 0,)"
               << R"( "distance_m": 2.0, "width_m": 0.9, "portrait": true},)"
               << R"({"id": "main", "vdd_index": 2, "yaw_deg": 0, "pitch_deg": 0, "roll_deg": 0,)"
               << R"( "distance_m": 2.0, "width_m": 1.6}]})";
    }
    gt::Layout implicit;
    check(gt::load_layout(path.string(), implicit, error) && implicit.screens.size() == 2,
          "a hand-written layout with a portrait screen loads");
    check(implicit.screens.size() == 2 && implicit.screens[0].portrait &&
              std::fabs(implicit.screens[0].height_m - 1.6f) < 1e-5f &&
              !implicit.screens[1].portrait && std::fabs(implicit.screens[1].height_m - 0.9f) < 1e-5f,
          "a missing height_m follows the orientation (9:16 portrait, 16:9 landscape)");

    gt::Layout duplicate = original;
    duplicate.screens[1].id = duplicate.screens[0].id;
    check(!gt::validate_layout(duplicate, error) && error == "screen ids must be unique",
          "duplicate screen IDs are rejected");
    gt::Layout bad_policy = original;
    bad_policy.capture_policy.leave_deg = bad_policy.capture_policy.enter_deg;
    check(!gt::validate_layout(bad_policy, error), "invalid capture hysteresis is rejected");

    std::printf("layout_selftest: three-dimensional screen geometry\n");
    gt::ScreenLayout screen = original.screens[1];
    auto quad = gt::make_screen_quad(screen);
    check(std::fabs(distance(quad[0], quad[1]) - screen.width_m) < 1e-5f,
          "quad width matches layout");
    check(std::fabs(distance(quad[1], quad[2]) - screen.height_m) < 1e-5f,
          "quad height matches layout");

    screen.yaw_deg = -45.0f;
    quad = gt::make_screen_quad(screen);
    const float centre_x = (quad[0].x + quad[1].x + quad[2].x + quad[5].x) * 0.25f;
    check(centre_x < -1.0f, "negative yaw moves a screen to the left");

    screen.yaw_deg = 0.0f;
    screen.pitch_deg = 20.0f;
    quad = gt::make_screen_quad(screen);
    const float centre_y = (quad[0].y + quad[1].y + quad[2].y + quad[5].y) * 0.25f;
    check(centre_y > 0.5f, "positive pitch moves a screen upward");

    screen.pitch_deg = 0.0f;
    screen.roll_deg = 30.0f;
    quad = gt::make_screen_quad(screen);
    check(quad[1].y > quad[0].y, "positive roll raises the right edge");

    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    std::printf("layout_selftest: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
