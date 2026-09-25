#include "app/engine_protocol.h"

namespace gt {

const char* engine_message_name(unsigned message) {
    switch (message) {
        case kEngineMessageQuery:
            return "query";
        case kEngineMessageQuit:
            return "quit";
        case kEngineMessageRecenter:
            return "recenter";
        case kEngineMessageToggleYaw:
            return "toggle-yaw";
        case kEngineMessageTogglePitch:
            return "toggle-pitch";
        case kEngineMessageReloadLayout:
            return "reload-layout";
        case kEngineMessageSetStabilise:
            return "set-stabilise";
        case kEngineMessageSetDimMode:
            return "set-dim-mode";
        case kEngineMessageSetScreenBrightness:
            return "set-screen-brightness";
        case kEngineMessageSetNightTint:
            return "set-night-tint";
        case kEngineMessageCursorToCenter:
            return "cursor-to-center";
        default:
            return "unknown";
    }
}

const wchar_t* app_user_model_id() {
    return kAppUserModelId;
}

std::string engine_status_sanitize(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char character : value) {
        const unsigned char code = static_cast<unsigned char>(character);
        if (code == '\r' || code == '\n' || code == '=') {
            out.push_back(' ');
        } else if (code < 0x20) {
            out.push_back(' ');
        } else {
            out.push_back(character);
        }
    }
    if (out.size() > 512) {
        out.resize(512);
    }
    return out;
}

}  // namespace gt
