#pragma once
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <deque>
#include <mutex>
#include <windows.h>

#include "pianissimo_engine.h"
#include "speech_model_catalog.h"
#include "dictation_window.h"
#include "external_dictation_target.h"
#include "live_mode_processor.h"
#include "mode_switch_policy.h"
#include "dictation_diagnostics.h"
#include "audio_capture.h"
#include "hotkey_manager.h"
#include "text_injector.h"
#include "overlay_window.h"
#include "tray_icon.h"
#include "config_manager.h"
#include "llm_cleaner.h"
#include "gemma_runtime.h"
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
    void transcribe_live(unsigned long long session);
    void process_text(std::string text, unsigned long long session);
    void change_live_mode(const std::string& mode);
    void refresh_live_mode_settings();
    LiveModeProcessor::Result transform_live_mode(const LiveModeProcessor::Request& request);
    void queue_live_mode(bool urgent = false, bool final = false);
    void apply_live_mode_results();
    void refresh_live_mode_status();
    void complete_live_capture();

    // Language
    void switch_language();

    // Model selection
    void select_model(const std::string& model_size);
    void select_whisper_compute_mode(const std::string& mode);
    void select_gemma_compute_mode(const std::string& mode);
    void download_and_select_model(const std::string& model_id);
    void select_gemma_model(const std::string& model_id);
    void download_and_select_gemma_model(const std::string& model_id,
                                         const std::string& model_path);
    void configure_llm_cleaner();
    bool ensure_internal_llm_runtime();
    bool ensure_llama_server_available(std::string& server_path);
    std::string current_gemma_model_id() const;
    std::string gemma_model_path_for_id_config(const std::string& model_id) const;
    std::string find_existing_gemma_model_path(const std::string& model_id) const;
    std::string gemma_model_download_path(const std::string& model_id) const;
    std::vector<std::string> installed_gemma_model_ids() const;
    std::string resolve_llama_server_path(const std::string& server_path) const;
    std::string current_model_size() const;
    std::string current_compute_mode() const;
    std::string current_gemma_compute_mode() const;
    std::string resolve_model_path(const std::string& model_path) const;
    bool effective_use_gpu(const std::string& model_size,
                           const std::string& compute_mode) const;
    bool reload_engine(const std::string& model_id, bool use_gpu);
    void start_engine_warmup();
    void join_engine_warmup();

    // Window proc
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Components
    std::unique_ptr<ConfigManager> config_;
    std::unique_ptr<PianissimoEngine> engine_;
    std::unique_ptr<DictationWindow> dictation_window_;
    std::unique_ptr<ExternalDictationTarget> external_dictation_;
    std::unique_ptr<LiveModeProcessor> live_mode_processor_;
    std::shared_ptr<DictationDiagnostics> dictation_diagnostics_;
    std::unique_ptr<AudioCapture> audio_;
    std::unique_ptr<HotkeyManager> hotkeys_;
    std::unique_ptr<TextInjector> injector_;
    std::unique_ptr<OverlayWindow> overlay_;
    std::unique_ptr<TrayIcon> tray_;
    std::unique_ptr<LlmCleaner> llm_cleaner_;
    std::unique_ptr<GemmaRuntime> gemma_runtime_;
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
    std::atomic<bool> shutting_down_{false};
    std::atomic<unsigned long long> dictation_session_{0};
    HWND output_target_ = nullptr;
    DWORD output_target_pid_ = 0;
    bool manual_output_requested_ = false;
    std::uint64_t live_mode_generation_ = 0;
    std::uint64_t live_click_id_ = 0;
    std::uint64_t last_live_token_ = 0;
    std::string last_live_raw_;
    std::uint64_t applied_live_token_ = 0;
    std::string applied_live_raw_;
    std::string live_status_key_;
    ULONGLONG live_result_wait_started_ = 0;
    ULONGLONG last_live_mode_poll_ = 0;
    bool live_session_active_ = false;
    bool live_capture_finished_ = false;
    bool live_mode_flush_requested_ = false;
    bool live_error_reported_ = false;
    ModeSwitchPolicy mode_switch_policy_;
    std::deque<ModeSwitchPolicy::Replacement> live_results_;
    std::mutex live_settings_mutex_;
    AppConfig live_settings_;
    std::string live_corrections_;
    std::mutex llm_work_mutex_;
    std::atomic<bool> live_llm_busy_{false};
    std::thread process_thread_;
    std::thread model_download_thread_;
    std::thread warmup_thread_;
    std::thread history_append_thread_;

};

} // namespace vtt
