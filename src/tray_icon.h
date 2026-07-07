#pragma once
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace vtt {

enum class TrayAction {
    ToggleRecording,
    OpenSettings,
    OpenHistory,
    SwitchLanguage,
    SelectModel,
    SelectComputeMode,
    Quit
};

using TrayCallback = std::function<void(TrayAction, const std::string&)>;

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    bool initialize(HWND hwnd, HINSTANCE hInstance);
    void shutdown();

    void set_callback(TrayCallback callback);
    void set_recording(bool recording);
    void set_current_model(const std::string& model_size);
    void set_current_compute_mode(const std::string& compute_mode);
    void set_compute_info(const std::string& active_backend,
                          const std::string& cpu_label,
                          const std::string& gpu_label,
                          const std::string& reason);
    void set_tooltip(const std::string& tooltip);
    void show_notification(const std::string& title, const std::string& message);

    // Call from WndProc for WM_APP+1 (tray messages)
    void handle_message(WPARAM wParam, LPARAM lParam);

private:
    void show_context_menu();
    HICON create_color_icon(COLORREF color);
    std::string model_size_from_command(UINT cmd) const;
    std::string compute_mode_from_command(UINT cmd) const;

    HWND hwnd_ = nullptr;
    HWND menu_hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    TrayCallback callback_;
    bool recording_ = false;
    bool initialized_ = false;
    bool menu_open_ = false;
    std::string current_model_size_ = "tiny";
    std::string current_compute_mode_ = "auto";
    std::string active_compute_backend_ = "CPU";
    std::string cpu_label_ = "Unknown CPU";
    std::string gpu_label_ = "No GPU detected";
    std::string compute_reason_ = "Auto";
    HICON icon_idle_ = nullptr;
    HICON icon_recording_ = nullptr;
};

} // namespace vtt
