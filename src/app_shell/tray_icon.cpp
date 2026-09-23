#include "app_shell/tray_icon.h"

#include <shellapi.h>

#include <cstring>

namespace gt::ui {
namespace {

NOTIFYICONDATAW make_icon_data(HWND owner, UINT callback_message, HICON icon,
                               const std::wstring& tooltip) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = callback_message;
    data.hIcon = icon;
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
    return data;
}

}  // namespace

TrayIcon::~TrayIcon() {
    remove();
}

bool TrayIcon::add(HWND owner, UINT callback_message, HICON icon, const std::wstring& tooltip,
                   std::string& error) {
    if (owner == nullptr || icon == nullptr) {
        error = "tray icon needs a window and an icon";
        return false;
    }
    remove();
    owner_ = owner;
    callback_message_ = callback_message;
    tooltip_ = tooltip;
    NOTIFYICONDATAW data = make_icon_data(owner, callback_message, icon, tooltip);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        error = "Shell_NotifyIcon(NIM_ADD) failed";
        return false;
    }
    data.uVersion = NOTIFYICON_VERSION;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    added_ = true;
    return true;
}

void TrayIcon::remove() {
    if (!added_ || owner_ == nullptr) {
        added_ = false;
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    added_ = false;
}

void TrayIcon::set_tooltip(const std::wstring& tooltip) {
    if (!added_ || owner_ == nullptr || tooltip == tooltip_) {
        return;
    }
    tooltip_ = tooltip;
    NOTIFYICONDATAW data = make_icon_data(owner_, callback_message_, nullptr, tooltip);
    data.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::notify(const std::wstring& title, const std::wstring& text) const {
    if (!added_ || owner_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = 1;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

TrayCommand TrayIcon::show_menu(HWND owner, POINT point, bool engine_running) const {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return TrayCommand::None;
    }
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(TrayCommand::Show), L"&Show window");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (engine_running ? MF_GRAYED : 0),
                static_cast<UINT_PTR>(TrayCommand::StartWorkspace), L"Start &workspace");
    AppendMenuW(menu, MF_STRING | (engine_running ? MF_GRAYED : 0),
                static_cast<UINT_PTR>(TrayCommand::StartPreview), L"Start &preview");
    AppendMenuW(menu, MF_STRING | (engine_running ? 0 : MF_GRAYED),
                static_cast<UINT_PTR>(TrayCommand::Stop), L"S&top engine");
    AppendMenuW(menu, MF_STRING | (engine_running ? 0 : MF_GRAYED),
                static_cast<UINT_PTR>(TrayCommand::Recenter), L"&Recenter");
    AppendMenuW(menu, MF_STRING | (engine_running ? 0 : MF_GRAYED),
                static_cast<UINT_PTR>(TrayCommand::ReloadLayout), L"Reload la&yout");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(TrayCommand::Quit), L"&Quit");

    SetForegroundWindow(owner);
    const UINT chosen = static_cast<UINT>(
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x, point.y, 0,
                       owner, nullptr));
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);
    return static_cast<TrayCommand>(chosen);
}

}  // namespace gt::ui
