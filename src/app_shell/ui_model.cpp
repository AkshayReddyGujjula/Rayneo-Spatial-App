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
    } else if (key == "acquiring") {
        state = HeadingLockState::Acquiring;
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
            return L"Gyro only";
        case HeadingLockState::Locked:
            return L"Locked";
        case HeadingLockState::Disturbed:
            return L"Interference";
        case HeadingLockState::Relearning:
            return L"Checking";
        case HeadingLockState::Acquiring:
            return L"Finding north";
        case HeadingLockState::Unknown:
        default:
            return L"Not reported";
    }
}

const wchar_t* heading_lock_detail(HeadingLockState state) {
    switch (state) {
        case HeadingLockState::Off:
            return L"Heading uses the gyro only. Calibrate the compass below to stop slow sideways drift.";
        case HeadingLockState::Locked:
            return L"Heading locked to the magnetic field, so the centre screen cannot slowly drift sideways.";
        case HeadingLockState::Disturbed:
            return L"Magnetic interference nearby: holding the current heading until the field settles.";
        case HeadingLockState::Relearning:
            return L"Checking a magnetic change (earbuds put in or taken out?) before trusting it again.";
        case HeadingLockState::Acquiring:
            return L"Finding magnetic north; this takes a few seconds after the start.";
        case HeadingLockState::Unknown:
        default:
            return L"Shown while the engine runs with head tracking and reports the heading lock.";
    }
}

EnginePhase parse_engine_phase(const std::string& text) {
    const std::string key = lower_ascii(text);
    if (key == "preparing") return EnginePhase::Preparing;
    if (key == "takeover") return EnginePhase::Takeover;
    if (key == "waiting_display") return EnginePhase::WaitingDisplay;
    if (key == "connecting") return EnginePhase::Connecting;
    if (key == "imu_warmup") return EnginePhase::ImuWarmup;
    if (key == "hold_still") return EnginePhase::HoldStill;
    if (key == "tracking") return EnginePhase::Tracking;
    if (key == "running") return EnginePhase::Running;
    if (key == "restoring") return EnginePhase::Restoring;
    return EnginePhase::Unknown;
}

StartupView startup_view(EnginePhase phase, double phase_progress, bool head_tracking) {
    StartupView view;
    const double progress = phase_progress < 0.0 ? -1.0 : (phase_progress > 1.0 ? 1.0 : phase_progress);
    switch (phase) {
        case EnginePhase::Preparing:
            view.active_step = 0;
            view.headline = L"Starting the engine";
            view.detail = L"Loading your layout and calibration.";
            break;
        case EnginePhase::Takeover:
            view.active_step = 0;
            view.headline = L"Setting up your screens";
            view.detail = L"Creating the virtual screens and arranging your displays. This can take up to "
                          L"half a minute, and the screens may flicker.";
            break;
        case EnginePhase::WaitingDisplay:
            view.active_step = 0;
            view.headline = L"Waiting for the glasses display";
            view.detail = L"The glasses display dropped out while the screens were arranged. Waiting up to "
                          L"15 seconds for it to come back.";
            break;
        case EnginePhase::Connecting:
            view.active_step = 1;
            view.headline = L"Connecting to the glasses sensor";
            view.detail = L"Waiting for the head-tracking sensor in the glasses to start. If this takes "
                          L"long, check the glasses cable.";
            break;
        case EnginePhase::ImuWarmup:
            view.active_step = 2;
            view.progress = progress;
            view.get_ready = progress >= kStartupGetReadyAt;
            if (view.get_ready) {
                view.headline = L"Get ready to hold still";
                view.detail = L"Look straight ahead. In a moment, keep your head still for about two "
                              L"seconds.";
            } else {
                view.headline = L"Warming up the sensor";
                view.detail = L"Put the glasses on and look straight ahead. You will be asked to keep "
                              L"still in a few seconds.";
            }
            break;
        case EnginePhase::HoldStill:
            view.active_step = 3;
            view.progress = progress < 0.0 ? 0.0 : progress;
            view.hold_still = true;
            view.headline = L"Hold your head still";
            view.detail = view.progress > 0.0
                              ? L"Measuring the sensor while you keep still. Any movement starts it again."
                              : L"Keep looking straight ahead without moving. Measuring starts as soon as "
                                L"your head is steady.";
            break;
        case EnginePhase::Tracking:
        case EnginePhase::Running:
        case EnginePhase::Restoring:
        case EnginePhase::Unknown:
        default:
            return view;
    }
    if (!head_tracking) {
        // Without head tracking only the screen setup is waited for; the
        // sensor steps never happen, so a sensor phase shows nothing.
        if (view.active_step > 0) {
            return StartupView{};
        }
        view.step_count = 0;
    } else {
        view.step_count = kStartupStepCount;
    }
    view.visible = true;
    return view;
}

int predict_ms_choice_index(int predict_ms) {
    for (int i = 0; i < kPredictMsChoiceCount; ++i) {
        if (kPredictMsChoices[i] == predict_ms) {
            return i;
        }
    }
    return -1;
}

}  // namespace gt
