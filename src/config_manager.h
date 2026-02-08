#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include <windows.h>

namespace vtt {

struct AppConfig {
    // Model settings
    std::string model_path = "models/ggml-model-q5_0.bin";
    std::string language = "sv";         // "sv", "en", "auto"
    bool use_gpu = true;
    int n_threads = 4;

    // Audio settings
    std::string audio_device = "";       // empty = default
    float vad_threshold = 0.01f;
    int silence_duration_ms = 1500;
    int speech_min_duration_ms = 200;

    // Hotkey settings (stored as virtual key codes + modifiers)
    unsigned int toggle_modifiers = MOD_CONTROL | MOD_SHIFT;
    unsigned int toggle_key = VK_SPACE;
    unsigned int cancel_modifiers = 0;
    unsigned int cancel_key = VK_ESCAPE;

    // Text injection
    std::string injection_method = "auto"; // "auto", "clipboard", "keystrokes"
    int keystroke_delay_ms = 5;

    // UI
    bool show_overlay = true;
    bool start_minimized = true;
    bool auto_start = false;

    // Serialize most fields automatically; modifiers handled separately
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(AppConfig,
        model_path, language, use_gpu, n_threads,
        audio_device, vad_threshold, silence_duration_ms, speech_min_duration_ms,
        toggle_key, cancel_key,
        injection_method, keystroke_delay_ms,
        show_overlay, start_minimized, auto_start)
};

class ConfigManager {
public:
    ConfigManager();

    bool load(const std::string& path = "");
    bool save(const std::string& path = "");

    AppConfig& config();
    const AppConfig& config() const;

    std::string get_config_path() const;

private:
    AppConfig config_;
    std::string config_path_;
    std::string get_default_path() const;
};

} // namespace vtt
