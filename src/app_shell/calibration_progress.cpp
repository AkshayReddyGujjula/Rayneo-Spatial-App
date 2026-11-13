#include "app_shell/calibration_progress.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace gt {
namespace {

using Json = nlohmann::json;

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

bool parse_flow(const std::string& text, CalFlow& flow) {
    if (text == "orientation") {
        flow = CalFlow::Orientation;
        return true;
    }
    if (text == "mag") {
        flow = CalFlow::Mag;
        return true;
    }
    return false;
}

bool parse_state(const std::string& text, CalStepState& state) {
    static const struct {
        const char* name;
        CalStepState state;
    } kStates[] = {
        {"waiting", CalStepState::Waiting}, {"recording", CalStepState::Recording},
        {"ok", CalStepState::Ok},           {"retry", CalStepState::Retry},
        {"failed", CalStepState::Failed},   {"done", CalStepState::Done},
    };
    for (const auto& entry : kStates) {
        if (text == entry.name) {
            state = entry.state;
            return true;
        }
    }
    return false;
}

bool read_optional_int(const Json& node, const char* key, int& out, std::string& error) {
    if (!node.contains(key) || node.at(key).is_null()) {
        return true;
    }
    const Json& value = node.at(key);
    if (!value.is_number_integer()) {
        error = std::string("'") + key + "' must be an integer";
        return false;
    }
    const long long raw = value.get<long long>();
    out = static_cast<int>(std::clamp<long long>(raw, 0, 1000));
    return true;
}

bool read_optional_string(const Json& node, const char* key, std::string& out,
                          std::string& error) {
    if (!node.contains(key) || node.at(key).is_null()) {
        return true;
    }
    const Json& value = node.at(key);
    if (!value.is_string()) {
        error = std::string("'") + key + "' must be a string";
        return false;
    }
    out = value.get<std::string>();
    if (out.size() > 400) {
        out.resize(400);
    }
    return true;
}

}  // namespace

const char* cal_flow_name(CalFlow flow) {
    return flow == CalFlow::Mag ? "mag" : "orientation";
}

const char* cal_step_state_name(CalStepState state) {
    switch (state) {
        case CalStepState::Recording:
            return "recording";
        case CalStepState::Ok:
            return "ok";
        case CalStepState::Retry:
            return "retry";
        case CalStepState::Failed:
            return "failed";
        case CalStepState::Done:
            return "done";
        case CalStepState::Waiting:
        default:
            return "waiting";
    }
}

CalLineKind parse_cal_progress_line(std::string_view line, CalProgressEvent& event,
                                    std::string& error) {
    error.clear();
    const std::string_view text = trim(line);
    const std::string_view prefix = kCalProgressPrefix;
    if (text.size() < prefix.size() || text.substr(0, prefix.size()) != prefix) {
        return CalLineKind::NotProgress;
    }
    std::string_view payload = text.substr(prefix.size());
    if (!payload.empty() && payload.front() != ' ' && payload.front() != '\t' &&
        payload.front() != '{') {
        // "@@progressive ..." is console text that merely starts alike.
        return CalLineKind::NotProgress;
    }
    payload = trim(payload);
    const Json node = Json::parse(payload.begin(), payload.end(), nullptr, false);
    if (node.is_discarded() || !node.is_object()) {
        error = "progress payload is not a JSON object";
        return CalLineKind::Malformed;
    }
    CalProgressEvent parsed;
    if (!node.contains("flow") || !node.at("flow").is_string() ||
        !parse_flow(node.at("flow").get<std::string>(), parsed.flow)) {
        error = "'flow' must be \"orientation\" or \"mag\"";
        return CalLineKind::Malformed;
    }
    if (!node.contains("state") || !node.at("state").is_string() ||
        !parse_state(node.at("state").get<std::string>(), parsed.state)) {
        error = "'state' must be waiting, recording, ok, retry, failed or done";
        return CalLineKind::Malformed;
    }
    if (!read_optional_int(node, "step", parsed.step, error) ||
        !read_optional_int(node, "steps", parsed.steps, error) ||
        !read_optional_string(node, "prompt", parsed.prompt, error) ||
        !read_optional_string(node, "detail", parsed.detail, error)) {
        return CalLineKind::Malformed;
    }
    if (node.contains("quality") && !node.at("quality").is_null()) {
        const Json& quality = node.at("quality");
        if (!quality.is_number()) {
            error = "'quality' must be a number";
            return CalLineKind::Malformed;
        }
        const double value = quality.get<double>();
        parsed.quality = (std::isfinite(value) && value >= 0.0 && value <= 1.0)
                             ? static_cast<float>(value)
                             : -1.0f;
    }
    if (parsed.steps > 0) {
        parsed.step = std::min(parsed.step, parsed.steps);
    }
    event = std::move(parsed);
    return CalLineKind::Progress;
}

std::vector<std::string> calibration_tool_arguments(CalFlow flow, const std::string& output_path,
                                                    bool earbuds) {
    std::vector<std::string> arguments{"--progress-json", "--auto", "--auto-delay", std::to_string(kCalAutoDelayS)};
    if (flow == CalFlow::Mag) {
        arguments.push_back("--mag");
        if (earbuds) {
            arguments.push_back("--earbuds");
        }
    }
    arguments.push_back("--output");
    arguments.push_back(output_path);
    return arguments;
}

std::string cal_reason_text(const std::string& detail) {
    struct Known {
        const char* code;
        const char* text;
    };
    static const Known kKnown[] = {
        {"NO_DEVICE", "The glasses were not found. Connect them by USB and try again."},
        {"NO_HID", "The USB device layer could not start. Reconnect the glasses and try again."},
        {"OPEN_FAILED", "The glasses could not be opened. Close other apps using them (the workspace, "
                        "another calibration) and try again."},
        {"STREAM_FAILED", "The glasses did not start sending motion data. Close other apps using them, "
                          "reconnect and try again."},
        {"NO_ORIENTATION", "Do the head calibration first; the compass calibration builds on it."},
        {"SAVE_FAILED", "The calibration was measured but could not be saved. Check that the config "
                        "folder is writable."},
        {"LOG_FAILED", "The raw calibration log could not be opened."},
    };
    // The tool appends its raw axis metrics to a failure; they are diagnostics.
    const auto tidy = [](std::string text) {
        const size_t metrics = text.find(" (up_vs_yaw ");
        if (metrics != std::string::npos) {
            const size_t close = text.find(')', metrics);
            text.erase(metrics, close == std::string::npos ? std::string::npos : close + 1 - metrics);
        }
        return cal_user_units(text);
    };
    const size_t colon = detail.find(": ");
    if (colon == std::string::npos || colon == 0) {
        return tidy(detail);
    }
    const std::string code = detail.substr(0, colon);
    for (char c : code) {
        if (!((c >= 'A' && c <= 'Z') || c == '_')) {
            return tidy(detail);
        }
    }
    for (const Known& known : kKnown) {
        if (code == known.code) {
            return known.text;
        }
    }
    std::string rest = detail.substr(colon + 2);
    if (!rest.empty() && rest.front() >= 'a' && rest.front() <= 'z') {
        rest.front() = static_cast<char>(rest.front() - 'a' + 'A');
    }
    return tidy(rest);
}

std::string cal_user_units(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    size_t i = 0;
    while (i < text.size()) {
        // " deg" after a digit, followed by "/s", the end or a non-letter.
        if (text.compare(i, 4, " deg") == 0 && i > 0 && text[i - 1] >= '0' && text[i - 1] <= '9') {
            const size_t after = i + 4;
            const bool word_ends =
                after >= text.size() || !((text[after] >= 'a' && text[after] <= 'z') ||
                                          (text[after] >= 'A' && text[after] <= 'Z'));
            if (word_ends) {
                out += "°";
                i = after;
                continue;
            }
        }
        out += text[i];
        ++i;
    }
    return out;
}

std::string calibration_saved_text(CalFlow flow, float quality, const std::string& detail) {
    // Same bands as gt::calibration_quality_label (Good >= 0.7, Fair >= 0.4).
    const bool known = std::isfinite(quality) && quality >= 0.0f;
    const bool good = known && quality >= 0.7f;
    const bool fair = known && !good && quality >= 0.4f;
    std::string text;
    if (flow == CalFlow::Mag) {
        text = "The heading lock is on from the next start, so the screens stay put instead of slowly "
               "turning.";
        const char* kAdvice = "for a better result repeat and ";
        const size_t at = detail.find(kAdvice);
        if (at != std::string::npos) {
            std::string advice = detail.substr(at + std::char_traits<char>::length(kAdvice));
            const size_t end = advice.find(';');
            if (end != std::string::npos) {
                advice.erase(end);
            }
            if (!advice.empty()) {
                text += " Coverage was thin: for a better result, run it again and " + advice + ".";
            }
        }
        return text;
    }
    if (good) {
        text = "The steps agreed closely, so your head axes are straight. The new calibration is in use "
               "from the next start.";
    } else if (fair) {
        text = "The steps agreed reasonably well. The new calibration is in use from the next start; if "
               "the screens tilt when you nod, run it again.";
    } else if (known) {
        text = "The steps disagreed with each other. The calibration was saved, but running it again "
               "should give a straighter result.";
    } else {
        text = "The new calibration is in use from the next start.";
    }
    if (detail.find("next: calibrate the magnetometer") != std::string::npos) {
        text += " Next, calibrate the compass for drift-free heading.";
    }
    return text;
}

void CalRunState::begin(CalFlow run_flow, bool auto_mode, double delay_s) {
    *this = CalRunState{};
    flow = run_flow;
    phase = Phase::Running;
    auto_advance = auto_mode;
    auto_delay_s = delay_s;
}

double CalRunState::countdown_s(double now_s) const {
    if (!auto_advance || !awaiting_user()) {
        return -1.0;
    }
    return std::max(0.0, auto_delay_s - (now_s - state_since_s));
}

void CalRunState::apply(const CalProgressEvent& event, double now_s) {
    if (phase != Phase::Running) {
        return;
    }
    if (!has_event || event.step != step || event.state != state) {
        state_since_s = now_s;
    }
    if (has_event && event.step != step) {
        retry_reason.clear();
    }
    if (event.state == CalStepState::Retry) {
        retry_reason = cal_reason_text(event.detail);
    } else if (event.state == CalStepState::Ok) {
        retry_reason.clear();
    }
    has_event = true;
    step = event.step;
    if (event.steps > 0) {
        steps = event.steps;
    }
    state = event.state;
    if (!event.prompt.empty()) {
        prompt = event.prompt;
    }
    detail = event.detail;
    quality = event.quality;
    if (event.state == CalStepState::Done) {
        phase = Phase::Succeeded;
    } else if (event.state == CalStepState::Failed) {
        if (event.detail == "cancelled") {
            // The tool saw a cancel this window did not send (stdin closed,
            // console closed): nothing was changed either way.
            phase = Phase::Cancelled;
            return;
        }
        phase = Phase::Failed;
        failure = !event.detail.empty() ? cal_reason_text(event.detail)
                  : !event.prompt.empty()
                      ? event.prompt
                      : std::string("The calibration tool reported a failure.");
    }
}

CalLineKind CalRunState::consume_line(const std::string& line, double now_s) {
    CalProgressEvent event;
    std::string error;
    const CalLineKind kind = parse_cal_progress_line(line, event, error);
    if (kind == CalLineKind::Progress) {
        apply(event, now_s);
    } else if (kind == CalLineKind::Malformed) {
        ++malformed_lines;
    } else {
        const std::string_view text = trim(line);
        if (!text.empty()) {
            recent_output.emplace_back(text.substr(0, 200));
            while (recent_output.size() > kRecentLines) {
                recent_output.pop_front();
            }
        }
    }
    return kind;
}

void CalRunState::process_exited(int exit_code) {
    if (phase != Phase::Running) {
        return;
    }
    phase = Phase::Failed;
    if (exit_code == 2) {
        // Exit code 2 = bad arguments: a tool from an older build that does
        // not know --progress-json / --auto.
        failure = "This orientation_calibrate.exe does not understand the in-app options (exit code 2). "
                  "Rebuild or re-stage it together with this app, or use Open in console.";
        return;
    }
    failure = "The calibration tool closed before it finished (exit code " +
              std::to_string(exit_code) + ").";
    if (!has_event) {
        failure += " It printed no progress: it may predate --progress-json, or the glasses "
                   "were not found.";
    }
}

void CalRunState::start_failed(const std::string& reason) {
    *this = CalRunState{};
    phase = Phase::Failed;
    failure = reason;
}

void CalRunState::cancel() {
    if (phase == Phase::Running) {
        phase = Phase::Cancelled;
    }
}

float CalRunState::fraction() const {
    if (phase == Phase::Succeeded) {
        return 1.0f;
    }
    if (steps <= 0) {
        return 0.0f;
    }
    float within = 0.0f;
    switch (state) {
        case CalStepState::Recording:
            within = 0.5f;
            break;
        case CalStepState::Ok:
        case CalStepState::Done:
            within = 1.0f;
            break;
        case CalStepState::Waiting:
        case CalStepState::Retry:
        case CalStepState::Failed:
        default:
            within = 0.0f;
            break;
    }
    const float base = static_cast<float>(std::max(0, step - 1));
    return std::clamp((base + within) / static_cast<float>(steps), 0.0f, 1.0f);
}

}  // namespace gt
