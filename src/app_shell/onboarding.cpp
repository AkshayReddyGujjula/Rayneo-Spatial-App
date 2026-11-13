#include "app_shell/onboarding.h"

#include "app_shell/app_config.h"

namespace gt {

const wchar_t* onboarding_step_label(OnboardingStep step) {
    switch (step) {
        case OnboardingStep::Welcome:
            return L"Welcome";
        case OnboardingStep::GlassesCheck:
            return L"Glasses";
        case OnboardingStep::HeadCalibration:
            return L"Head calibration";
        case OnboardingStep::MagCalibration:
            return L"Compass";
        case OnboardingStep::LayoutPreset:
            return L"Layout";
        case OnboardingStep::Done:
            return L"Done";
        case OnboardingStep::Count:
        default:
            return L"";
    }
}

const wchar_t* onboarding_step_title(OnboardingStep step) {
    switch (step) {
        case OnboardingStep::Welcome:
            return L"Welcome to RayNeo Spatial";
        case OnboardingStep::GlassesCheck:
            return L"Check your glasses";
        case OnboardingStep::HeadCalibration:
            return L"Calibrate head tracking";
        case OnboardingStep::MagCalibration:
            return L"Calibrate the compass (optional)";
        case OnboardingStep::LayoutPreset:
            return L"Choose a starting layout";
        case OnboardingStep::Done:
            return L"You are all set";
        case OnboardingStep::Count:
        default:
            return L"";
    }
}

void OnboardingFlow::start() {
    *this = OnboardingFlow{};
    active = true;
}

void OnboardingFlow::next() {
    if (!active || step == OnboardingStep::Done) {
        return;
    }
    if (step == OnboardingStep::HeadCalibration && head == StepOutcome::Pending) {
        head = StepOutcome::Skipped;
    }
    if (step == OnboardingStep::MagCalibration && mag == StepOutcome::Pending) {
        mag = StepOutcome::Skipped;
    }
    step = static_cast<OnboardingStep>(static_cast<int>(step) + 1);
}

void OnboardingFlow::back() {
    if (can_go_back()) {
        step = static_cast<OnboardingStep>(static_cast<int>(step) - 1);
    }
}

void OnboardingFlow::skip_step() {
    if (!step_skippable()) {
        return;
    }
    if (step == OnboardingStep::HeadCalibration && head != StepOutcome::Completed) {
        head = StepOutcome::Skipped;
    }
    if (step == OnboardingStep::MagCalibration && mag != StepOutcome::Completed) {
        mag = StepOutcome::Skipped;
    }
    next();
}

bool OnboardingFlow::can_go_back() const {
    return active && step != OnboardingStep::Welcome;
}

bool OnboardingFlow::step_skippable() const {
    return active && (step == OnboardingStep::HeadCalibration ||
                      step == OnboardingStep::MagCalibration);
}

bool onboarding_should_show(const AppConfig& config) {
    return !config.onboarding_complete;
}

bool onboarding_close(OnboardingFlow& flow, AppConfig& config, bool config_file_valid) {
    flow.active = false;
    config.onboarding_complete = true;
    return config_file_valid;
}

}  // namespace gt
