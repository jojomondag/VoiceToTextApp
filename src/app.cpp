#include "app.h"
#include <sstream>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <filesystem>

#define WM_TRAYICON (WM_APP + 1)

static const wchar_t* APP_CLASS_NAME = L"VoiceToTextMainWindow";

namespace {
// Diagnostic logging — writes to voicetotext.log next to the exe so the user
// can see recording/transcription flow even when launched detached from a
// console. Thread-safe.
std::mutex g_log_mutex;
FILE* g_log_file = nullptr;

FILE* log_file() {
    if (!g_log_file) {
        char exe_path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        std::string log_path(exe_path);
        auto pos = log_path.find_last_of("\\/");
        if (pos != std::string::npos) log_path = log_path.substr(0, pos + 1);
        log_path += "voicetotext.log";
        g_log_file = fopen(log_path.c_str(), "a");
    }
    return g_log_file;
}

void vtlog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    FILE* f = log_file();
    if (!f) return;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count() % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    fprintf(f, "[%02d:%02d:%02d.%03lld] ",
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
            static_cast<long long>(ms));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}
}  // anon namespace

namespace vtt {

App::App() = default;

App::~App() {
    shutdown();
}

bool App::initialize(HINSTANCE hInstance) {
    hInstance_ = hInstance;

    // --- 1. Load configuration ---
    config_ = std::make_unique<ConfigManager>();
    config_->load();

    const auto& cfg = config_->config();

    // --- 2. Create hidden message window ---
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance_;
    wc.lpszClassName  = APP_CLASS_NAME;

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    hwnd_ = CreateWindowExW(0, APP_CLASS_NAME, L"VoiceToText",
                            0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, hInstance_, this);
    if (!hwnd_) return false;

    // --- 3. Initialize Whisper engine ---
    engine_ = std::make_unique<WhisperEngine>();
    engine_->set_language(cfg.language);
    engine_->set_n_threads(cfg.n_threads);

    if (!engine_->initialize(cfg.model_path, cfg.use_gpu)) {
        // Try relative to exe directory
        char exe_path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        std::string exe_dir(exe_path);
        auto pos = exe_dir.find_last_of("\\/");
        if (pos != std::string::npos) {
            exe_dir = exe_dir.substr(0, pos + 1);
        }
        std::string alt_path = exe_dir + cfg.model_path;
        if (!engine_->initialize(alt_path, cfg.use_gpu)) {
            MessageBoxW(nullptr,
                        L"Failed to load Whisper model.\n"
                        L"Please download a model using:\n"
                        L"  .\\scripts\\download_model.ps1",
                        L"VoiceToText", MB_ICONWARNING);
            return false;
        }
    }

    // --- 4. Initialize audio capture ---
    audio_ = std::make_unique<AudioCapture>();
    if (!audio_->initialize()) {
        MessageBoxW(nullptr, L"Failed to initialize audio capture.",
                    L"VoiceToText", MB_ICONWARNING);
        return false;
    }

    if (!cfg.audio_device.empty()) {
        audio_->select_device(cfg.audio_device);
    }

    // --- 5. Initialize VAD ---
    vad_ = std::make_unique<VoiceActivityDetector>();
    vad_->set_threshold(cfg.vad_threshold);
    vad_->set_silence_duration_ms(cfg.silence_duration_ms);
    vad_->set_speech_duration_ms(cfg.speech_min_duration_ms);

    // --- 6. Initialize hotkey manager ---
    hotkeys_ = std::make_unique<HotkeyManager>();
    if (!hotkeys_->initialize(hwnd_)) {
        MessageBoxW(nullptr, L"Failed to initialize hotkeys.",
                    L"VoiceToText", MB_ICONWARNING);
        return false;
    }

    hotkeys_->set_callback([this](HotkeyAction action) {
        on_hotkey(action);
    });

    // Register hotkeys from config.
    // NOTE: CancelRecording (ESC by default) is registered *only* while
    // recording is active. A bare-ESC global hotkey would otherwise swallow
    // every ESC press system-wide and break the key in other applications.
    hotkeys_->register_hotkey(HotkeyAction::ToggleRecording,
                              cfg.toggle_modifiers, cfg.toggle_key);
    hotkeys_->register_hotkey(HotkeyAction::SwitchLanguage,
                              MOD_CONTROL | MOD_SHIFT, 'L');

    // --- 7. Initialize text injector ---
    injector_ = std::make_unique<TextInjector>();
    injector_->initialize();
    injector_->set_method(cfg.injection_method);
    injector_->set_keystroke_delay_ms(cfg.keystroke_delay_ms);

    // --- 8. Initialize local LLM cleanup ---
    llm_cleaner_ = std::make_unique<LlmCleaner>();
    llm_cleaner_->configure({
        cfg.llm_cleanup_enabled,
        cfg.llm_endpoint,
        cfg.llm_model,
        cfg.llm_pre_prompt,
        cfg.llm_timeout_ms,
        cfg.llm_max_tokens,
        cfg.llm_intent_profiles_enabled,
        cfg.llm_intent_detection_prompt,
        cfg.llm_profile_transform_prompt,
    });

    correction_store_ = std::make_unique<CorrectionStore>();
    std::filesystem::path correction_path(cfg.llm_corrections_path);
    if (correction_path.is_relative()) {
        correction_path = std::filesystem::path(config_->get_config_path())
                              .parent_path() / correction_path;
    }
    correction_store_->initialize(correction_path.string(),
                                  cfg.llm_corrections_max_entries);

    transcript_history_ = std::make_unique<TranscriptHistory>();
    std::filesystem::path history_path(cfg.transcript_history_path);
    if (history_path.is_relative()) {
        history_path = std::filesystem::path(config_->get_config_path())
                           .parent_path() / history_path;
    }
    transcript_history_->initialize(history_path.string(),
                                    cfg.transcript_history_max_entries);

    history_window_ = std::make_unique<HistoryWindow>();
    history_window_->initialize(hInstance_, transcript_history_.get());

    // --- 9. Initialize overlay ---
    overlay_ = std::make_unique<OverlayWindow>();
    if (cfg.show_overlay) {
        overlay_->initialize(hInstance_);
    }

    // --- 10. Initialize system tray ---
    tray_ = std::make_unique<TrayIcon>();
    if (!tray_->initialize(hwnd_, hInstance_)) {
        MessageBoxW(nullptr, L"Failed to create system tray icon.",
                    L"VoiceToText", MB_ICONWARNING);
        // Non-fatal: continue without tray
    }

    tray_->set_callback([this](TrayAction action) {
        on_tray_action(action);
    });

    // Find initial language index
    for (size_t i = 0; i < languages_.size(); ++i) {
        if (languages_[i] == cfg.language) {
            language_index_ = static_cast<int>(i);
            break;
        }
    }

    // Show startup notification
    std::string lang_name;
    if (cfg.language == "sv") lang_name = "Swedish";
    else if (cfg.language == "en") lang_name = "English";
    else lang_name = "Auto-detect";

    std::string toggle_desc = hotkeys_->get_binding_description(HotkeyAction::ToggleRecording);

    tray_->show_notification("VoiceToText Ready",
        "Press " + toggle_desc + " to start recording.\n"
        "Language: " + lang_name + "\n"
        "GPU: " + (cfg.use_gpu ? "Enabled" : "Disabled"));

    return true;
}

int App::run() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void App::shutdown() {
    // Wait for processing thread to finish
    if (process_thread_.joinable()) {
        process_thread_.join();
    }

    if (audio_ && audio_->is_recording()) {
        audio_->stop_recording();
    }

    if (tray_) tray_->shutdown();
    if (history_window_) history_window_->shutdown();
    if (overlay_) overlay_->shutdown();
    if (hotkeys_) hotkeys_->shutdown();
    if (injector_) injector_->shutdown();
    if (audio_) audio_->shutdown();
    if (engine_) engine_->shutdown();

    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    UnregisterClassW(APP_CLASS_NAME, hInstance_);
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    App* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_HOTKEY:
            self->hotkeys_->handle_hotkey(static_cast<int>(wParam));
            return 0;

        case WM_TRAYICON:
            if (self->tray_) {
                self->tray_->handle_message(wParam, lParam);
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void App::on_hotkey(HotkeyAction action) {
    vtlog("on_hotkey: action=%d recording=%d processing=%d",
          static_cast<int>(action), recording_.load(), processing_.load());
    switch (action) {
        case HotkeyAction::ToggleRecording:
            if (processing_.load()) {
                vtlog("on_hotkey: ignored (processing in progress)");
                break;
            }
            if (recording_.load()) {
                stop_recording();
            } else {
                start_recording();
            }
            break;

        case HotkeyAction::CancelRecording:
            if (recording_.load()) {
                cancel_recording();
            }
            break;

        case HotkeyAction::SwitchLanguage:
            switch_language();
            break;

        case HotkeyAction::OpenSettings:
            // Open the config file in the default editor
            ShellExecuteA(nullptr, "open", config_->get_config_path().c_str(),
                         nullptr, nullptr, SW_SHOW);
            break;
    }
}

void App::on_tray_action(TrayAction action) {
    switch (action) {
        case TrayAction::ToggleRecording:
            on_hotkey(HotkeyAction::ToggleRecording);
            break;

        case TrayAction::SwitchLanguage:
            switch_language();
            break;

        case TrayAction::OpenSettings:
            on_hotkey(HotkeyAction::OpenSettings);
            break;

        case TrayAction::OpenHistory:
            if (history_window_) {
                history_window_->show();
            }
            break;

        case TrayAction::Quit:
            if (recording_.load()) {
                cancel_recording();
            }
            PostMessage(hwnd_, WM_CLOSE, 0, 0);
            break;
    }
}

void App::start_recording() {
    vtlog("start_recording: enter (recording=%d processing=%d)",
          recording_.load(), processing_.load());
    if (recording_.load() || processing_.load()) {
        vtlog("start_recording: early-exit (already active)");
        return;
    }

    vad_->reset();

    bool audio_ok = audio_->start_recording();
    vtlog("start_recording: audio_->start_recording() returned %d", audio_ok);
    if (!audio_ok) {
        if (overlay_) overlay_->show(OverlayState::Error, "Mic error");
        tray_->show_notification("VoiceToText", "Failed to start recording. Check microphone.");
        return;
    }

    recording_ = true;

    // Register the cancel hotkey only while recording, so a bare ESC binding
    // does not block the ESC key in other applications.
    const auto& cfg = config_->config();
    hotkeys_->register_hotkey(HotkeyAction::CancelRecording,
                              cfg.cancel_modifiers, cfg.cancel_key);

    if (overlay_) overlay_->show(OverlayState::Listening);
    if (tray_) tray_->set_recording(true);

    // Start a thread that monitors audio level for the overlay
    // (The actual audio data is buffered by AudioCapture)
    std::thread([this]() {
        while (recording_.load()) {
            if (overlay_) {
                overlay_->set_audio_level(audio_->get_audio_level());
            }
            Sleep(50);
        }
    }).detach();
}

void App::stop_recording() {
    vtlog("stop_recording: enter");
    if (!recording_.load()) {
        vtlog("stop_recording: early-exit (not recording)");
        return;
    }

    recording_ = false;
    hotkeys_->unregister_hotkey(HotkeyAction::CancelRecording);
    audio_->stop_recording();

    if (overlay_) overlay_->show(OverlayState::Processing);
    if (tray_) tray_->set_recording(false);

    // Process in background thread to keep UI responsive
    if (process_thread_.joinable()) {
        process_thread_.join();
    }

    processing_ = true;
    process_thread_ = std::thread([this]() {
        process_audio();
        processing_ = false;
    });
}

void App::cancel_recording() {
    if (!recording_.load()) return;

    recording_ = false;
    hotkeys_->unregister_hotkey(HotkeyAction::CancelRecording);
    audio_->stop_recording();

    // Discard the audio buffer
    audio_->get_audio_buffer();

    if (overlay_) overlay_->hide();
    if (tray_) tray_->set_recording(false);

    tray_->set_tooltip("VoiceToText - Ready (cancelled)");
}

void App::process_audio() {
    vtlog("process_audio: enter");

    // Get all recorded audio
    std::vector<float> audio_data = audio_->get_audio_buffer();
    vtlog("process_audio: audio buffer size = %zu samples (%.2f s @16kHz)",
          audio_data.size(), audio_data.size() / 16000.0);

    if (audio_data.empty()) {
        vtlog("process_audio: BUFFER EMPTY -- audio capture produced no samples");
        if (overlay_) overlay_->hide();
        return;
    }

    // Check minimum audio length (at least 0.5 seconds at 16kHz)
    if (audio_data.size() < 8000) {
        vtlog("process_audio: too short (<0.5s), aborting");
        if (overlay_) overlay_->hide();
        tray_->set_tooltip("VoiceToText - Too short");
        return;
    }

    // Check if the audio has enough energy (skip if it's basically silence)
    {
        float sum_sq = 0.0f;
        for (float s : audio_data) {
            sum_sq += s * s;
        }
        float rms = std::sqrt(sum_sq / static_cast<float>(audio_data.size()));
        vtlog("process_audio: RMS=%.6f (threshold 0.0005)", rms);
        if (rms < 0.0005f) {
            vtlog("process_audio: silence detected, aborting");
            if (overlay_) overlay_->hide();
            tray_->set_tooltip("VoiceToText - No speech detected");
            return;
        }
    }

    // Run transcription
    vtlog("process_audio: calling whisper transcribe...");
    TranscriptionResult result = engine_->transcribe(audio_data);
    vtlog("process_audio: transcribe returned, text length = %zu", result.text.size());

    // Filter out common whisper hallucinations on silence/noise
    {
        std::string t = result.text;
        // Strip whitespace and punctuation for comparison
        std::string stripped;
        for (char c : t) {
            if (!std::isspace(static_cast<unsigned char>(c)) &&
                c != '.' && c != ',' && c != '!' && c != '?' &&
                c != '-' && c != '*' && c != '[' && c != ']' &&
                c != '(' && c != ')') {
                stripped += std::tolower(static_cast<unsigned char>(c));
            }
        }
        if (stripped.empty() || stripped == "blank" ||
            stripped == "blankaudio" || stripped == "silence" ||
            stripped == "inaudible" || stripped == "music" ||
            stripped == "applause" || stripped == "thankyou" ||
            stripped == "youtu" || stripped == "subscribenow" ||
            stripped == "thanksfowatching" || stripped == "thankyouforwatching") {
            result.text.clear();
        }
    }

    if (!result.text.empty()) {
        std::string recorded_text = result.text;
        std::string ai_language_text;
        std::string intent_profile = "general";

        if (config_->config().llm_cleanup_enabled && llm_cleaner_) {
            vtlog("process_audio: calling LM Studio profile + transform passes...");
            std::string learned_corrections;
            if (config_->config().llm_corrections_enabled && correction_store_) {
                learned_corrections = correction_store_->build_prompt_block(
                    config_->config().llm_corrections_prompt_limit,
                    config_->config().llm_corrections_min_count);
            }

            LlmProfileResult profile_result;
            if (llm_cleaner_->clean_text_with_profile(result.text,
                                                      learned_corrections,
                                                      profile_result)) {
                vtlog("process_audio: transform succeeded, profile=%s, text length = %zu",
                      profile_result.profile.c_str(), profile_result.text.size());
                result.text = profile_result.text;
                intent_profile = profile_result.profile;
                if (config_->config().llm_corrections_enabled &&
                    correction_store_) {
                    correction_store_->learn_from(recorded_text,
                                                  profile_result.text);
                }
            } else {
                vtlog("process_audio: cleanup failed, using raw transcription");
            }
        }

        if (config_->config().llm_cleanup_enabled &&
            config_->config().llm_ai_language_enabled && llm_cleaner_) {
            vtlog("process_audio: calling AI language transform...");
            if (llm_cleaner_->transform_text(result.text,
                                            config_->config().llm_ai_language_prompt,
                                            ai_language_text)) {
                vtlog("process_audio: AI language succeeded, text length = %zu",
                      ai_language_text.size());
            } else {
                vtlog("process_audio: AI language failed");
            }
        }

        if (transcript_history_) {
            transcript_history_->append(recorded_text, result.text,
                                        ai_language_text, intent_profile);
        }

        if (overlay_) overlay_->hide();
        vtlog("process_audio: injecting text: \"%s\"", result.text.c_str());
        // Small delay to let the user release the hotkey
        Sleep(100);

        // Inject the transcribed text into the focused application
        injector_->inject_text(result.text);

        // Update tray tooltip with last transcription info
        std::ostringstream oss;
        oss << "VoiceToText - Last: " << result.text.substr(0, 50);
        if (result.text.size() > 50) oss << "...";
        tray_->set_tooltip(oss.str());
    } else {
        if (overlay_) overlay_->hide();
        vtlog("process_audio: transcription text was empty (or filtered)");
        tray_->set_tooltip("VoiceToText - No speech detected");
    }
    vtlog("process_audio: exit");
}

void App::switch_language() {
    language_index_ = (language_index_ + 1) % static_cast<int>(languages_.size());
    const std::string& lang = languages_[language_index_];

    engine_->set_language(lang);

    // Update config
    config_->config().language = lang;
    config_->save();

    // Show notification
    std::string lang_name;
    if (lang == "sv") lang_name = "Swedish";
    else if (lang == "en") lang_name = "English";
    else lang_name = "Auto-detect";

    tray_->show_notification("Language Changed", "Now using: " + lang_name);
    tray_->set_tooltip("VoiceToText - " + lang_name);
}

} // namespace vtt
