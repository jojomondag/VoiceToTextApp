#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <windows.h>

namespace vtt {

struct ProfileDefinition {
    std::string name;
    std::string instructions;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ProfileDefinition,
        name, instructions)
};

struct AppConfig {
    // Model settings
    std::string model_path = "models/ggml-model-large-q5_0.bin";
    std::string language = "sv";         // "sv", "en", "auto"
    bool use_gpu = true;
    std::string compute_mode = "auto";   // "auto", "gpu", "cpu"
    int n_threads = 0;                    // 0 = auto
    bool whisper_fast_mode = true;
    bool warm_up_model_on_load = true;

    // Audio settings
    std::string audio_device = "";       // empty = default
    float vad_threshold = 0.01f;
    int silence_duration_ms = 1500;
    int speech_min_duration_ms = 200;
    bool trim_silence_before_transcription = true;
    int trim_silence_padding_ms = 250;

    // Hotkey settings (stored as virtual key codes + modifiers)
    unsigned int toggle_modifiers = MOD_CONTROL | MOD_SHIFT;
    unsigned int toggle_key = VK_SPACE;
    unsigned int cancel_modifiers = 0;
    unsigned int cancel_key = VK_ESCAPE;

    // Text injection
    std::string injection_method = "auto"; // "auto", "clipboard", "keystrokes"
    int keystroke_delay_ms = 5;
    int pre_injection_delay_ms = 25;
    std::string paste_output_mode = "clean"; // "ai", "recorded", "clean"

    // Local LLM cleanup
    bool llm_cleanup_enabled = false;
    std::string llm_provider = "lmstudio"; // "lmstudio", "internal"
    std::string llm_endpoint = "http://127.0.0.1:1234/v1/chat/completions";
    std::string llm_model = "local-model";
    std::string llm_internal_model = "auto";
    std::string llm_internal_model_path = "models/llm/gemma-4-E2B-it-Q8_0.gguf";
    std::string llm_internal_compute_mode = "auto";
    std::string llm_internal_server_path = "llama-server.exe";
    int llm_internal_port = 8088;
    bool llm_internal_auto_start = true;
    std::string llm_pre_prompt =
        "You clean up dictated instructions before they are sent to an AI "
        "coding model. Fix transcription mistakes, wrong words that clearly "
        "do not fit the context, punctuation, casing, grammar, and structure. "
        "Make the text clear, explicit, and easy for a coding model to act on. "
        "Use coding-context corrections when obvious, for example LM Studio, "
        "app.cpp, C++, CMake, PowerShell, JSON, API, endpoint, and config. "
        "Preserve the user's intent, language, tone, names, paths, commands, "
        "code identifiers, numbers, and technical terms. Do not add new "
        "requirements or explanations. Return only the cleaned instruction.";
    int llm_timeout_ms = 30000;
    int llm_max_tokens = 4096;          // Dynamic per-request ceiling
    bool llm_corrections_enabled = true;
    std::string llm_corrections_path = "transcription_corrections.json";
    int llm_corrections_max_entries = 200;
    int llm_corrections_prompt_limit = 20;
    int llm_corrections_min_count = 1;
    bool llm_ai_language_enabled = true;
    std::string llm_ai_language_prompt =
        "Create an ultra-compact AI-readable instruction dialect from the "
        "user's cleaned instruction. Compress aggressively while preserving "
        "intent, constraints, entities, code names, paths, numbers, and action "
        "order. Remove repetition and filler. Prefer dense structured syntax "
        "with labels such as Goal, Context, Tasks, Constraints, Output. Use "
        "abbreviations only when unambiguous to a coding language model. Do "
        "not add new requirements. Return only the AI-language version.";
    bool llm_intent_profiles_enabled = true;
    std::string llm_intent_detection_prompt =
        u8"Identifiera anv\u00E4ndarens avsikt fr\u00E5n texten och v\u00E4lj exakt en profil: "
        "coding, email, discussion, translation eller general. Svara endast "
        "med strikt JSON: "
        "{\"profile\":\"coding|email|discussion|translation|general\"}.";
    std::string llm_profile_transform_prompt =
        u8"Transformera texten enligt vald profil. F\u00F6lj profilens egna "
        u8"instruktioner noggrant, bevara anv\u00E4ndarens avsikt och l\u00E4gg inte "
        u8"till nya krav. Svara endast med den transformerade texten.";
    std::vector<ProfileDefinition> llm_profiles = {
        {
            "coding",
            u8"G\u00F6r texten konkret, \u00E5tg\u00E4rdsbar och tydlig f\u00F6r en kodmodell."
        },
        {
            "email",
            u8"G\u00F6r texten till ett tydligt mailutkast med passande ton. "
            u8"L\u00E4gg inte till placeholders, signatur eller \u00E4mnesrad om "
            u8"anv\u00E4ndaren inte bad om det."
        },
        {
            "discussion",
            u8"Bevara samtalston och g\u00F6r resonemanget klart."
        },
        {
            "translation",
            u8"\u00D6vers\u00E4tt eller f\u00F6rbered \u00F6vers\u00E4ttning enligt anv\u00E4ndarens "
            u8"beg\u00E4ran och bevara betydelsen."
        },
        {
            "general",
            u8"St\u00E4da texten utan s\u00E4rskild formatering."
        },
    };

    // UI
    bool show_overlay = true;
    bool start_minimized = true;
    bool auto_start = false;
    std::string transcript_history_path = "transcript_history.json";
    int transcript_history_max_entries = 100;

    // Serialize most fields automatically; modifiers handled separately
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(AppConfig,
        model_path, language, use_gpu, compute_mode, n_threads,
        whisper_fast_mode, warm_up_model_on_load,
        audio_device, vad_threshold, silence_duration_ms, speech_min_duration_ms,
        trim_silence_before_transcription, trim_silence_padding_ms,
        toggle_key, cancel_key,
        injection_method, keystroke_delay_ms, pre_injection_delay_ms,
        paste_output_mode,
        llm_cleanup_enabled, llm_provider, llm_endpoint, llm_model,
        llm_internal_model, llm_internal_model_path,
        llm_internal_compute_mode, llm_internal_server_path,
        llm_internal_port, llm_internal_auto_start, llm_pre_prompt,
        llm_timeout_ms, llm_max_tokens, llm_corrections_enabled,
        llm_corrections_path, llm_corrections_max_entries,
        llm_corrections_prompt_limit, llm_corrections_min_count,
        llm_ai_language_enabled, llm_ai_language_prompt,
        llm_intent_profiles_enabled, llm_intent_detection_prompt,
        llm_profile_transform_prompt, llm_profiles,
        show_overlay, start_minimized, auto_start,
        transcript_history_path, transcript_history_max_entries)
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
