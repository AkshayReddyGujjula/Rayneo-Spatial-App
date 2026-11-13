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

CalLiveLineKind parse_cal_live_line(std::string_view line, CalLiveSample& sample, std::string& error) {
    error.clear();
    const std::string_view text = trim(line);
    const std::string_view prefix = kCalLivePrefix;
    if (text.size() < prefix.size() || text.substr(0, prefix.size()) != prefix) {
        return CalLiveLineKind::NotLive;
    }
    std::string_view payload = text.substr(prefix.size());
    if (!payload.empty() && payload.front() != ' ' && payload.front() != '\t' && payload.front() != '{') {
        return CalLiveLineKind::NotLive;
    }
    payload = trim(payload);
    const Json node = Json::parse(payload.begin(), payload.end(), nullptr, false);
    if (node.is_discarded() || !node.is_object()) {
        error = "live payload is not a JSON object";
        return CalLiveLineKind::Malformed;
    }
    CalLiveSample parsed;
    const auto string_of = [&](const char* key) -> std::string {
        if (!node.contains(key) || !node.at(key).is_string()) {
            return {};
        }
        return node.at(key).get<std::string>();
    };
    const std::string kind = string_of("kind");
    if (kind == "still") {
        parsed.kind = CalGuideKind::Still;
    } else if (kind == "turn") {
        parsed.kind = CalGuideKind::Turn;
    } else if (kind == "nod") {
        parsed.kind = CalGuideKind::Nod;
    } else if (kind == "tilt") {
        parsed.kind = CalGuideKind::Tilt;
    } else {
        error = "'kind' must be still, turn, nod or tilt";
        return CalLiveLineKind::Malformed;
    }
    const std::string state = string_of("state");
    if (state != "waiting" && state != "recording") {
        error = "'state' must be waiting or recording";
        return CalLiveLineKind::Malformed;
    }
    parsed.waiting = state == "waiting";
    const auto number = [&](const char* key, float fallback, float limit) -> float {
        if (!node.contains(key) || !node.at(key).is_number()) {
            return fallback;
        }
        const double value = node.at(key).get<double>();
        if (!std::isfinite(value)) {
            return fallback;
        }
        return static_cast<float>(std::clamp(value, -static_cast<double>(limit), static_cast<double>(limit)));
    };
    const auto integer = [&](const char* key) -> int {
        if (!node.contains(key) || !node.at(key).is_number_integer()) {
            return 0;
        }
        return static_cast<int>(std::clamp<long long>(node.at(key).get<long long>(), 0, 100));
    };
    parsed.step = integer("step");
    parsed.elapsed_s = number("elapsed", 0.0f, 3600.0f);
    parsed.remaining_s = number("remaining", -1.0f, 3600.0f);
    parsed.frame = string_of("frame").substr(0, 16);
    parsed.yaw_deg = number("yaw", 0.0f, 360.0f);
    parsed.pitch_deg = number("pitch", 0.0f, 360.0f);
    parsed.roll_deg = number("roll", 0.0f, 360.0f);
    parsed.along_deg = number("along", 0.0f, 360.0f);
    parsed.off_deg = std::max(0.0f, number("off", 0.0f, 360.0f));
    parsed.speed_degs = std::max(0.0f, number("speed", 0.0f, 5000.0f));
    parsed.still_rms_degs = std::max(0.0f, number("still_rms", 0.0f, 5000.0f));
    parsed.still_gate_degs = number("still_gate", 1.5f, 100.0f);
    parsed.gate_deg = number("gate", 12.0f, 180.0f);
    parsed.target_deg = number("target", 25.0f, 180.0f);
    parsed.peak_deg = number("peak", 0.0f, 360.0f);
    if (!(parsed.still_gate_degs > 0.0f)) {
        parsed.still_gate_degs = 1.5f;
    }
    if (!(parsed.gate_deg > 0.0f)) {
        parsed.gate_deg = 12.0f;
    }
    if (!(parsed.target_deg >= parsed.gate_deg)) {
        parsed.target_deg = parsed.gate_deg;
    }
    parsed.repeats = integer("repeats");
    parsed.repeats_required = integer("repeats_required");
    std::string hint = string_of("hint");
    bool hint_ok = !hint.empty() && hint.size() <= 24;
    for (const char c : hint) {
        if (!((c >= 'A' && c <= 'Z') || c == '_')) {
            hint_ok = false;
        }
    }
    parsed.hint = hint_ok ? hint : std::string();
    sample = std::move(parsed);
    return CalLiveLineKind::Live;
}

std::string cal_retry_tip(const CalLiveSample& last, int off_axis_count, int slower_count, int wrong_way_count) {
    // About half a second of a live hint (the tool sends ~22 lines/s).
    constexpr int kSeen = 10;
    if (wrong_way_count >= kSeen) {
        switch (last.kind) {
            case CalGuideKind::Turn:
                return "Turn to your LEFT first, then come back.";
            case CalGuideKind::Nod:
                return "Nod DOWN first (chin toward your chest), then come back up.";
            case CalGuideKind::Tilt:
                return "Tilt toward your RIGHT shoulder first, then come back.";
            case CalGuideKind::Still:
                break;
        }
    }
    if (off_axis_count >= kSeen) {
        switch (last.kind) {
            case CalGuideKind::Turn:
                return "Keep your chin level while you turn: no nodding or tilting. Follow the line with the ball.";
            case CalGuideKind::Nod:
                return "Nod straight down, as if saying yes: keep facing forward, no turning or tilting.";
            case CalGuideKind::Tilt:
                return "Keep your nose pointing straight ahead and bring your ear toward your shoulder: no turning "
                       "or nodding.";
            case CalGuideKind::Still:
                break;
        }
    }
    if (last.kind != CalGuideKind::Still && last.repeats_required > 0 && last.repeats < last.repeats_required) {
        return "Go a little further each time, past the first mark on the guide, and come all the way back to "
               "centre. Do it " +
               std::to_string(last.repeats_required) + (last.repeats_required == 1 ? " time." : " times.");
    }
    if (slower_count >= kSeen) {
        return "Move more slowly and smoothly.";
    }
    return {};
}

std::vector<std::string> calibration_tool_arguments(CalFlow flow, const std::string& output_path,
                                                    bool earbuds) {
    std::vector<std::string> arguments{"--progress-json", "--auto", "--auto-delay", std::to_string(kCalAutoDelayS)};
    if (flow == CalFlow::Mag) {
        arguments.push_back("--mag");
        if (earbuds) {
            arguments.push_back("--earbuds");
        }
    } else {
        arguments.push_back("--repeats");
        arguments.push_back(std::to_string(kCalHeadRepeats));
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
    if (has_event && event.step != step) {
        retry_tip.clear();
    }
    if (event.state == CalStepState::Retry) {
        retry_reason = cal_reason_text(event.detail);
        retry_tip = has_last_recording
                        ? cal_retry_tip(last_recording, live_off_axis, live_slower, live_wrong_way)
                        : std::string();
    } else if (event.state == CalStepState::Ok) {
        retry_reason.clear();
        retry_tip.clear();
    }
    if (event.state == CalStepState::Recording) {
        live_off_axis = 0;
        live_slower = 0;
        live_wrong_way = 0;
        has_last_recording = false;
    }
    // The guide shows the live head only while a step waits or records.
    if (event.state != CalStepState::Waiting && event.state != CalStepState::Recording) {
        has_live = false;
    } else if (has_event && event.step != step) {
        has_live = false;
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

void CalRunState::apply_live(const CalLiveSample& sample, double now_s) {
    if (phase != Phase::Running) {
        return;
    }
    tool_has_live = true;
    has_live = true;
    live = sample;
    live_at_s = now_s;
    if (!sample.waiting && sample.kind != CalGuideKind::Still) {
        if (sample.hint == "OFF_AXIS") {
            ++live_off_axis;
        } else if (sample.hint == "SLOWER") {
            ++live_slower;
        } else if (sample.hint == "WRONG_WAY") {
            ++live_wrong_way;
        }
        last_recording = sample;
        has_last_recording = true;
    }
}

CalLineKind CalRunState::consume_line(const std::string& line, double now_s) {
    {
        CalLiveSample sample;
        std::string live_error;
        const CalLiveLineKind live_kind = parse_cal_live_line(line, sample, live_error);
        if (live_kind == CalLiveLineKind::Live) {
            apply_live(sample, now_s);
            return CalLineKind::NotProgress;
        }
        if (live_kind == CalLiveLineKind::Malformed) {
            // Display-only data: a bad live line is dropped, never a failure.
            return CalLineKind::NotProgress;
        }
    }
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
