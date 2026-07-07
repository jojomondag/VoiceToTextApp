#pragma once
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <windows.h>

#include "whisper_engine.h"
#include "audio_capture.h"
#include "vad.h"
#include "hotkey_manager.h"
#include "text_injector.h"
#include "overlay_window.h"
#include "tray_icon.h"
#include "config_manager.h"
#include "llm_cleaner.h"
#include "correction_store.h"
#include "transcript_history.h"
#include "history_window.h"

namespace vtt {

class App {
public:
    App();
    ~App();

    bool initialize(HINSTANCE hInstance);
    int run();
    void shutdown();

private:
    // Event handlers
    void initialize_tray();
    void open_history_window();
    void on_hotkey(HotkeyAction action);
    void on_tray_action(TrayAction action, const std::string& value = "");

    // Recording control
    void start_recording();
    void stop_recording();
    void cancel_recording();

    // Transcription
    void process_audio();

    // Language
    void switch_language();

    // Model selection
    void select_model(const std::string& model_size);
    void select_compute_mode(const std::string& compute_mode);
    void download_and_select_model(const std::string& model_size,
                                   const std::string& model_path);
    bool ensure_startup_model_available();
    std::string current_model_size() const;
    std::string current_compute_mode() const;
    std::string resolve_model_path(const std::string& model_path) const;
    std::string model_path_for_size(const std::string& model_size) const;
    std::string model_url_for_size(const std::string& model_size) const;
    bool effective_use_gpu(const std::string& model_size,
                           const std::string& compute_mode) const;
    bool reload_engine(const std::string& model_path, bool use_gpu);
    void update_tray_compute_info();
    void start_engine_warmup();
    void join_engine_warmup();

    // Window proc
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Components
    std::unique_ptr<ConfigManager> config_;
    std::unique_ptr<WhisperEngine> engine_;
    std::unique_ptr<AudioCapture> audio_;
    std::unique_ptr<VoiceActivityDetector> vad_;
    std::unique_ptr<HotkeyManager> hotkeys_;
    std::unique_ptr<TextInjector> injector_;
    std::unique_ptr<OverlayWindow> overlay_;
    std::unique_ptr<TrayIcon> tray_;
    std::unique_ptr<LlmCleaner> llm_cleaner_;
    std::unique_ptr<CorrectionStore> correction_store_;
    std::unique_ptr<TranscriptHistory> transcript_history_;
    std::unique_ptr<HistoryWindow> history_window_;

    // State
    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    UINT taskbar_created_msg_ = 0;
    std::atomic<bool> recording_{false};
    std::atomic<bool> processing_{false};
    std::atomic<bool> downloading_model_{false};
    std::thread process_thread_;
    std::thread model_download_thread_;
    std::thread warmup_thread_;
    std::thread history_append_thread_;

    // Language cycling
    std::vector<std::string> languages_ = {"sv", "en", "auto"};
    int language_index_ = 0;
};

} // namespace vtt
