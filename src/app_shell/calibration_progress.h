#pragma once

// Machine-readable progress from orientation_calibrate.exe --progress-json.
//
// The tool prints exactly one line per state change:
//
//   @@progress {"flow":"orientation"|"mag","step":<int>,"steps":<int>,
//               "state":"waiting"|"recording"|"ok"|"retry"|"failed"|"done",
//               "prompt":"<instruction>","detail":"<optional>","quality":<0..1 or -1>}
//
// Every other stdout line is human-readable console output and is ignored by
// the parser (the run model keeps the last few for a failure report). The run
// model turns the event stream plus the process exit into what the UI shows,
// including the "tool exited without a done/failed line" failure.

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace gt {

enum class CalFlow : int {
    Orientation = 0,
    Mag,
};

enum class CalStepState : int {
    Waiting = 0,  // waiting for the wearer (the tool reads ENTER on stdin)
    Recording,
    Ok,
    Retry,
    Failed,
    Done,
};

const char* cal_flow_name(CalFlow flow);
const char* cal_step_state_name(CalStepState state);

struct CalProgressEvent {
    CalFlow flow = CalFlow::Orientation;
    int step = 0;
    int steps = 0;
    CalStepState state = CalStepState::Waiting;
    std::string prompt;
    std::string detail;
    float quality = -1.0f;  // 0..1, or -1 when the tool has no quality figure
};

enum class CalLineKind {
    NotProgress,  // ordinary console output
    Progress,     // a valid @@progress line, event filled
    Malformed,    // starts with @@progress but cannot be used; error filled
};

inline constexpr const char* kCalProgressPrefix = "@@progress";

// The in-app run uses the tool's --auto mode: the wearer is holding a pose
// and cannot reach the keyboard, so each step starts after a visible
// countdown. An empty stdin line starts the step early ("Start now"), "q"
// cancels through the tool's own path (IMU stream off, exit 1).
inline constexpr int kCalAutoDelayS = 5;

// orientation_calibrate.exe arguments for an in-app run:
//   --progress-json --auto --auto-delay 5 [--mag [--earbuds]] --output FILE
std::vector<std::string> calibration_tool_arguments(CalFlow flow, const std::string& output_path,
                                                    bool earbuds);

// Plain-language text for a tool failure/retry detail ("CODE: message ...").
// Known setup codes (no device, stream, save, ...) get a fix-it sentence;
// otherwise the code prefix is dropped and the tool's own sentence is kept.
std::string cal_reason_text(const std::string& detail);

// flow and state are required. step/steps default to 0, prompt/detail to
// empty, quality to -1; a present field of the wrong type makes the line
// malformed. Trailing CR/LF and surrounding blanks are ignored. Steps are
// clamped so 0 <= step <= steps when steps > 0; quality outside 0..1 reads
// as -1.
CalLineKind parse_cal_progress_line(std::string_view line, CalProgressEvent& event,
                                    std::string& error);

// What the calibration view renders for one run of the tool.
struct CalRunState {
    enum class Phase {
        Idle = 0,
        Running,
        Succeeded,
        Failed,
        Cancelled,
    };

    Phase phase = Phase::Idle;
    CalFlow flow = CalFlow::Orientation;
    bool has_event = false;
    int step = 0;
    int steps = 0;
    CalStepState state = CalStepState::Waiting;
    std::string prompt;
    std::string detail;
    float quality = -1.0f;
    std::string failure;  // plain-language reason when phase == Failed
    int malformed_lines = 0;
    std::deque<std::string> recent_output;  // last console lines (non-progress)
    // --auto mode: each waiting step starts by itself after auto_delay_s.
    bool auto_advance = false;
    double auto_delay_s = 0.0;
    double state_since_s = 0.0;  // when the current step/state began (caller clock)
    // Why the last attempt of this step was retried; the tool's next line
    // ("waiting") would otherwise erase it before anyone could read it.
    std::string retry_reason;

    static constexpr size_t kRecentLines = 6;

    void begin(CalFlow run_flow, bool auto_mode = false, double delay_s = 0.0);
    void apply(const CalProgressEvent& event, double now_s = 0.0);
    // Parses one stdout line: progress lines update the state, everything
    // else lands in recent_output. Returns the line kind.
    CalLineKind consume_line(const std::string& line, double now_s = 0.0);
    // The process ended. Without a prior done/failed event this is a
    // failure that names the exit code.
    void process_exited(int exit_code);
    // The tool could not be started at all.
    void start_failed(const std::string& reason);
    void cancel();

    bool running() const { return phase == Phase::Running; }
    bool finished() const {
        return phase == Phase::Succeeded || phase == Phase::Failed || phase == Phase::Cancelled;
    }
    // Waiting for the wearer to confirm (the UI shows a Continue button).
    bool awaiting_user() const { return running() && has_event && state == CalStepState::Waiting; }
    // Seconds until an --auto step starts by itself (0 when due), or -1 when
    // no countdown is running.
    double countdown_s(double now_s) const;
    // 0..1 overall progress for the bar.
    float fraction() const;
};

}  // namespace gt
