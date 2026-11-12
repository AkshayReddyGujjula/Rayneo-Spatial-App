#include "app_shell/ui_model.h"

#include <cctype>

namespace gt {
namespace {

std::string lower_ascii(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

const char* app_page_key(AppPage page) {
    switch (page) {
        case AppPage::Home:
            return "home";
        case AppPage::Layout:
            return "layout";
        case AppPage::Comfort:
            return "comfort";
        case AppPage::Tracking:
            return "tracking";
        case AppPage::Diagnostics:
            return "diagnostics";
        case AppPage::Settings:
            return "settings";
        case AppPage::Count:
        default:
            return "home";
    }
}

bool parse_app_page(const std::string& text, AppPage& page) {
    const std::string key = lower_ascii(text);
    for (int i = 0; i < kAppPageCount; ++i) {
        if (key == app_page_key(static_cast<AppPage>(i))) {
            page = static_cast<AppPage>(i);
            return true;
        }
    }
    return false;
}

const wchar_t* app_page_label(AppPage page) {
    switch (page) {
        case AppPage::Home:
            return L"Home";
        case AppPage::Layout:
            return L"Layout";
        case AppPage::Comfort:
            return L"Comfort";
        case AppPage::Tracking:
            return L"Tracking";
        case AppPage::Diagnostics:
            return L"Diagnostics";
        case AppPage::Settings:
            return L"Settings";
        case AppPage::Count:
        default:
            return L"";
    }
}

const wchar_t* app_page_title(AppPage page) {
    switch (page) {
        case AppPage::Home:
            return L"Spatial workspace";
        case AppPage::Layout:
            return L"Workspace layout";
        case AppPage::Comfort:
            return L"View comfort";
        case AppPage::Tracking:
            return L"Tracking and calibration";
        case AppPage::Diagnostics:
            return L"Diagnostics";
        case AppPage::Settings:
            return L"Settings";
        case AppPage::Count:
        default:
            return L"";
    }
}

const wchar_t* app_page_subtitle(AppPage page) {
    switch (page) {
        case AppPage::Home:
            return L"Start, steer and check your glasses workspace.";
        case AppPage::Layout:
            return L"Arrange the virtual screens around you, then save to apply.";
        case AppPage::Comfort:
            return L"Steadier text, softer screens and a warmer evening tint.";
        case AppPage::Tracking:
            return L"Head tracking behaviour and the sensor calibrations it relies on.";
        case AppPage::Diagnostics:
            return L"Every health check, the controller event log and recovery tools.";
        case AppPage::Settings:
            return L"Appearance, window behaviour, the setup guide and version details.";
        case AppPage::Count:
        default:
            return L"";
    }
}

const char* theme_mode_name(ThemeMode mode) {
    switch (mode) {
        case ThemeMode::Light:
            return "light";
        case ThemeMode::System:
            return "system";
        case ThemeMode::Dark:
        case ThemeMode::Count:
        default:
            return "dark";
    }
}

const wchar_t* theme_mode_label(ThemeMode mode) {
    switch (mode) {
        case ThemeMode::Light:
            return L"Light";
        case ThemeMode::System:
            return L"Match Windows";
        case ThemeMode::Dark:
        case ThemeMode::Count:
        default:
            return L"Dark";
    }
}

bool parse_theme_mode(const std::string& text, ThemeMode& mode) {
    const std::string key = lower_ascii(text);
    for (int i = 0; i < kThemeModeCount; ++i) {
        if (key == theme_mode_name(static_cast<ThemeMode>(i))) {
            mode = static_cast<ThemeMode>(i);
            return true;
        }
    }
    return false;
}

bool theme_is_dark(ThemeMode mode, bool system_uses_light_theme) {
    switch (mode) {
        case ThemeMode::Light:
            return false;
        case ThemeMode::System:
            return !system_uses_light_theme;
        case ThemeMode::Dark:
        case ThemeMode::Count:
        default:
            return true;
    }
}

bool parse_heading_lock_state(const std::string& text, HeadingLockState& state) {
    const std::string key = lower_ascii(text);
    if (key == "off") {
        state = HeadingLockState::Off;
    } else if (key == "locked") {
        state = HeadingLockState::Locked;
    } else if (key == "disturbed") {
        state = HeadingLockState::Disturbed;
    } else if (key == "relearning" || key == "re-learning") {
        state = HeadingLockState::Relearning;
    } else if (key == "unknown" || key.empty()) {
        state = HeadingLockState::Unknown;
    } else {
        return false;
    }
    return true;
}

const wchar_t* heading_lock_label(HeadingLockState state) {
    switch (state) {
        case HeadingLockState::Off:
            return L"Off";
        case HeadingLockState::Locked:
            return L"Locked";
        case HeadingLockState::Disturbed:
            return L"Disturbed";
        case HeadingLockState::Relearning:
            return L"Re-learning";
        case HeadingLockState::Unknown:
        default:
            return L"Not reported";
    }
}

const wchar_t* heading_lock_detail(HeadingLockState state) {
    switch (state) {
        case HeadingLockState::Off:
            return L"No magnetic calibration is in use, so heading relies on the gyro alone.";
        case HeadingLockState::Locked:
            return L"Heading is held to the local magnetic field; slow sideways drift is removed.";
        case HeadingLockState::Disturbed:
            return L"A changing magnetic field was detected nearby; the lock is paused until it settles.";
        case HeadingLockState::Relearning:
            return L"The surroundings changed; the lock is learning the new field without a jump.";
        case HeadingLockState::Unknown:
        default:
            return L"Shown while the engine runs and reports the heading lock.";
    }
}

}  // namespace gt
