// RayNeo Spatial controller (WIN32 subsystem).
//
// Startup order matters for the taskbar identity: SetCurrentProcessExplicitAppUserModelID
// runs before any window exists, so the taskbar button, the notification-area
// icon and the Start Menu shortcut all resolve to the same stable identity and
// the user can pin the running app.

#include "app/engine_protocol.h"
#include "app_shell/app_state.h"
#include "app_shell/app_window.h"
#include "app_shell/clock.h"
#include "app_shell/diagnostics.h"

#include <windows.h>
#include <shobjidl.h>

#include <string>

namespace {

void enable_dpi_awareness() {
    using SetDpiAwarenessFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        auto fn =
            reinterpret_cast<SetDpiAwarenessFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn != nullptr && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    SetProcessDPIAware();
}

void show_fatal(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), L"RayNeo Spatial", MB_OK | MB_ICONERROR);
}

bool bootstrap(gt::AppState& state, std::string& fatal_error) {
    if (!gt::resolve_base_paths(state.paths, fatal_error)) {
        return false;
    }
    state.config = gt::AppConfig{};
    std::string config_error;
    if (!gt::load_app_config(state.paths.app_config_path, state.config, config_error)) {
        state.config = gt::AppConfig{};
        state.config_valid = false;
        state.set_banner(L"config/app.json is invalid (" + gt::wide_from_utf8(config_error) +
                             L"); defaults are in use. Fix or delete the file, then press "
                             L"Refresh.",
                         gt::Severity::Warning);
    }
    gt::apply_config_paths(state.paths, state.config);

    state.now_s = gt::steady_now_s();
    state.layout = gt::preset_layout(gt::LayoutPreset::TripleArc);
    gt::Layout loaded;
    std::string layout_error;
    if (gt::load_layout(gt::narrow_utf8(state.paths.layout_path), loaded, layout_error)) {
        state.layout = std::move(loaded);
    } else if (state.banner.empty()) {
        state.set_banner(L"the layout file could not be loaded (" +
                             gt::wide_from_utf8(layout_error) +
                             L"); the triple-arc default is loaded. Press Save and reload to "
                             L"write it.",
                         gt::Severity::Warning);
    }
    if (state.selected_screen >= state.layout.screens.size()) {
        state.selected_screen = 0;
    }
    gt::run_expensive_diagnostics(state);
    return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    enable_dpi_awareness();
    SetCurrentProcessExplicitAppUserModelID(gt::app_user_model_id());

    HANDLE instance_mutex = CreateMutexW(nullptr, FALSE, L"Local\\RayNeoSpatialController");
    if (instance_mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        // The first instance may still be starting: give its window up to 5 s
        // to appear before handing off (or giving up quietly).
        HWND existing = nullptr;
        for (int attempt = 0; attempt < 50 && existing == nullptr; ++attempt) {
            existing = FindWindowW(gt::kAppWindowClass, nullptr);
            if (existing == nullptr) {
                Sleep(100);
            }
        }
        if (existing != nullptr) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(instance_mutex);
        return 0;
    }

    gt::ui::AppWindow window;
    std::string error;
    if (!bootstrap(window.state(), error)) {
        show_fatal(gt::wide_from_utf8(error));
        if (instance_mutex != nullptr) {
            CloseHandle(instance_mutex);
        }
        return 1;
    }
    if (!window.create(instance, error)) {
        show_fatal(gt::wide_from_utf8(error));
        if (instance_mutex != nullptr) {
            CloseHandle(instance_mutex);
        }
        return 1;
    }
    window.run();
    gt::shutdown_hid();
    if (instance_mutex != nullptr) {
        CloseHandle(instance_mutex);
    }
    return 0;
}
