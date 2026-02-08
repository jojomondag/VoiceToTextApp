#include "app.h"
#include <sstream>

#define WM_TRAYICON (WM_APP + 1)

static const wchar_t* APP_CLASS_NAME = L"VoiceToTextMainWindow";

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

    // Register hotkeys from config
    hotkeys_->register_hotkey(HotkeyAction::ToggleRecording,
                              cfg.toggle_modifiers, cfg.toggle_key);
    hotkeys_->register_hotkey(HotkeyAction::CancelRecording,
                              cfg.cancel_modifiers, cfg.cancel_key);
    hotkeys_->register_hotkey(HotkeyAction::SwitchLanguage,
                              MOD_CONTROL | MOD_SHIFT, 'L');

    // --- 7. Initialize text injector ---
    injector_ = std::make_unique<TextInjector>();
    injector_->initialize();
    injector_->set_method(cfg.injection_method);
    injector_->set_keystroke_delay_ms(cfg.keystroke_delay_ms);

    // --- 8. Initialize overlay ---
    overlay_ = std::make_unique<OverlayWindow>();
    if (cfg.show_overlay) {
        overlay_->initialize(hInstance_);
    }

    // --- 9. Initialize system tray ---
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
    switch (action) {
        case HotkeyAction::ToggleRecording:
            if (processing_.load()) {
                // Currently processing, ignore
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

        case TrayAction::Quit:
            if (recording_.load()) {
                cancel_recording();
            }
            PostMessage(hwnd_, WM_CLOSE, 0, 0);
            break;
    }
}

void App::start_recording() {
    if (recording_.load() || processing_.load()) return;

    vad_->reset();

    if (!audio_->start_recording()) {
        if (overlay_) overlay_->show(OverlayState::Error, "Mic error");
        tray_->show_notification("VoiceToText", "Failed to start recording. Check microphone.");
        return;
    }

    recording_ = true;

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
    if (!recording_.load()) return;

    recording_ = false;
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
    audio_->stop_recording();

    // Discard the audio buffer
    audio_->get_audio_buffer();

    if (overlay_) overlay_->hide();
    if (tray_) tray_->set_recording(false);

    tray_->set_tooltip("VoiceToText - Ready (cancelled)");
}

void App::process_audio() {
    // Get all recorded audio
    std::vector<float> audio_data = audio_->get_audio_buffer();

    if (audio_data.empty()) {
        if (overlay_) overlay_->hide();
        return;
    }

    // Check minimum audio length (at least 0.5 seconds at 16kHz)
    if (audio_data.size() < 8000) {
        if (overlay_) overlay_->hide();
        tray_->set_tooltip("VoiceToText - Too short");
        return;
    }

    // Run transcription
    TranscriptionResult result = engine_->transcribe(audio_data);

    if (overlay_) overlay_->hide();

    if (!result.text.empty()) {
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
        tray_->set_tooltip("VoiceToText - No speech detected");
    }
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
