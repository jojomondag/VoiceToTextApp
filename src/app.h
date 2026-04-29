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
    void on_hotkey(HotkeyAction action);
    void on_tray_action(TrayAction action);

    // Recording control
    void start_recording();
    void stop_recording();
    void cancel_recording();

    // Transcription
    void process_audio();

    // Language
    void switch_language();

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
    std::atomic<bool> recording_{false};
    std::atomic<bool> processing_{false};
    std::thread process_thread_;

    // Language cycling
    std::vector<std::string> languages_ = {"sv", "en", "auto"};
    int language_index_ = 0;
};

} // namespace vtt
