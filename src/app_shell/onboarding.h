#pragma once

// First-run setup guide (onboarding) state machine.
//
// Pure model: which step is showing, what was done or skipped, and the
// completion flag persisted in app.json. The guide never starts the engine
// and never touches displays; the only side effects it may cause are
// launching the calibration tool (on an explicit click) and, on Finish,
// saving the layout preset the user picked.

#include <string>

namespace gt {

struct AppConfig;

enum class OnboardingStep : int {
    Welcome = 0,
    GlassesCheck,
    HeadCalibration,
    MagCalibration,
    LayoutPreset,
    Done,
    Count,
};

inline constexpr int kOnboardingStepCount = static_cast<int>(OnboardingStep::Count);

const wchar_t* onboarding_step_label(OnboardingStep step);  // stepper caption
const wchar_t* onboarding_step_title(OnboardingStep step);  // heading on the card

enum class StepOutcome : int {
    Pending = 0,
    Completed,
    Skipped,
};

struct OnboardingFlow {
    bool active = false;
    OnboardingStep step = OnboardingStep::Welcome;
    StepOutcome head = StepOutcome::Pending;
    StepOutcome mag = StepOutcome::Pending;
    // -1 keeps the current layout; otherwise a LayoutPreset index.
    int preset_choice = -1;

    void start();
    // Moves forward; a pending optional step passed with Next counts as
    // skipped. Next on Done is a no-op (finish() closes the guide).
    void next();
    void back();
    void skip_step();
    void mark_head_calibrated() { head = StepOutcome::Completed; }
    void mark_mag_calibrated() { mag = StepOutcome::Completed; }

    bool can_go_back() const;
    bool on_last_step() const { return step == OnboardingStep::Done; }
    int index() const { return static_cast<int>(step); }
    // Steps with a Skip affordance (the calibrations).
    bool step_skippable() const;
};

// Show the guide at launch when it has never been completed or dismissed.
bool onboarding_should_show(const AppConfig& config);
// Finish or "Skip setup": closes the guide and marks completion in the
// config. Returns whether the caller may save app.json: only when the file on
// disk loaded (config_file_valid). An app.json that failed to load is still
// the user's file, untouched on disk (the banner asks them to fix or delete
// it); the defaults in memory must not overwrite it, so the guide then closes
// for this session only.
bool onboarding_close(OnboardingFlow& flow, AppConfig& config, bool config_file_valid);

}  // namespace gt
