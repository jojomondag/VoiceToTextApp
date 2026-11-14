#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "win_compat.h"

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

// System tray icon (StatusNotifierItem through Qt) with the same menu as the
// Windows build. Every method may be called from any thread.
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

    // Tray events arrive through Qt, never as window messages.
    void handle_message(LPARAM /*lParam*/) {}

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace vtt
