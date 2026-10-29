#pragma once
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace vtt {

enum class TrayAction {
    ToggleRecording,
    OpenDictation,
    OpenSettings,
    OpenHistory,
    SwitchLanguage,
    SelectModel,
    SelectGemmaModel,
    SelectWhisperComputeMode,
    SelectGemmaComputeMode,
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
    void set_current_compute_mode(const std::string& whisper_mode,
                                  const std::string& gemma_mode);
    void set_current_gemma_model(const std::string& model_id,
                                 const std::string& recommended_model_id);
    void set_installed_gemma_models(const std::vector<std::string>& model_ids);
    void set_tooltip(const std::string& tooltip);
    void show_notification(const std::string& title, const std::string& message);

    // Call from WndProc for WM_APP+1 (tray messages)
    void handle_message(LPARAM lParam);

private:
    void show_context_menu();
    std::string model_size_from_command(UINT cmd) const;
    std::string gemma_model_from_command(UINT cmd) const;
    std::string whisper_compute_from_command(UINT cmd) const;
    std::string gemma_compute_from_command(UINT cmd) const;

    HWND hwnd_ = nullptr;
    HWND menu_hwnd_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    TrayCallback callback_;
    bool recording_ = false;
    bool initialized_ = false;
    bool menu_open_ = false;
    std::string current_model_size_ = "onnx-int8";
    std::string current_whisper_compute_mode_ = "auto";
    std::string current_gemma_compute_mode_ = "auto";
    std::string current_gemma_model_ = "auto";
    std::string recommended_gemma_model_ = "e2b";
    std::vector<std::string> installed_gemma_models_;
    HICON icon_idle_ = nullptr;
    HICON icon_recording_ = nullptr;
};

} // namespace vtt
