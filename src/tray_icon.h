#pragma once
#include <functional>
#include <string>
#include <windows.h>

namespace vtt {

enum class TrayAction {
    ToggleRecording,
    OpenSettings,
    OpenHistory,
    SwitchLanguage,
    Quit
};

using TrayCallback = std::function<void(TrayAction)>;

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    bool initialize(HWND hwnd, HINSTANCE hInstance);
    void shutdown();

    void set_callback(TrayCallback callback);
    void set_recording(bool recording);
    void set_tooltip(const std::string& tooltip);
    void show_notification(const std::string& title, const std::string& message);

    // Call from WndProc for WM_APP+1 (tray messages)
    void handle_message(WPARAM wParam, LPARAM lParam);

private:
    void show_context_menu();
    HICON create_color_icon(COLORREF color);

    HWND hwnd_ = nullptr;
    HWND menu_hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    TrayCallback callback_;
    bool recording_ = false;
    bool initialized_ = false;
    bool menu_open_ = false;
    HICON icon_idle_ = nullptr;
    HICON icon_recording_ = nullptr;
};

} // namespace vtt
