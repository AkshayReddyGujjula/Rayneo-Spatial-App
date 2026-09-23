#pragma once

// Notification-area lifecycle. The icon is added once the window exists and
// removed on destroy; explorer restarts are handled by re-adding it when the
// shell broadcasts "TaskbarCreated".

#include <windows.h>

#include <string>

namespace gt::ui {

enum class TrayCommand : unsigned {
    None = 0,
    Show = 1,
    StartWorkspace = 2,
    StartPreview = 3,
    Stop = 4,
    Recenter = 5,
    ReloadLayout = 6,
    Quit = 7,
};

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool add(HWND owner, UINT callback_message, HICON icon, const std::wstring& tooltip,
             std::string& error);
    void remove();
    bool added() const { return added_; }
    void set_tooltip(const std::wstring& tooltip);
    void notify(const std::wstring& title, const std::wstring& text) const;
    // Modal popup menu; returns the chosen command (None when dismissed).
    TrayCommand show_menu(HWND owner, POINT point, bool engine_running) const;

private:
    HWND owner_ = nullptr;
    UINT callback_message_ = 0;
    bool added_ = false;
    std::wstring tooltip_;
};

}  // namespace gt::ui
