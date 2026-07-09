#pragma once

#include <string>
#include <vector>

namespace vtt {

struct LlmProfileDefinition {
    std::string name;
    std::string instructions;
};

struct LlmCleanerConfig {
    bool enabled = false;
    std::string endpoint = "http://127.0.0.1:1234/v1/chat/completions";
    std::string model = "local-model";
    std::string pre_prompt =
        "You clean up dictated instructions before they are sent to an AI "
        "coding model. Fix transcription mistakes, wrong words that clearly "
        "do not fit the context, punctuation, casing, grammar, and structure. "
        "Make the text clear, explicit, and easy for a coding model to act on. "
        "Use coding-context corrections when obvious, for example LM Studio, "
        "app.cpp, C++, CMake, PowerShell, JSON, API, endpoint, and config. "
        "Preserve the user's intent, language, tone, names, paths, commands, "
        "code identifiers, numbers, and technical terms. Do not add new "
        "requirements or explanations. Return only the cleaned instruction.";
    int timeout_ms = 30000;
    int max_tokens = 4096;
    bool intent_profiles_enabled = true;
    std::string intent_detection_prompt =
        "Detect the user's intent from the dictated text. Choose exactly one "
        "profile: coding, email, discussion, translation, or general. Return "
        "strict JSON only: {\"profile\":\"coding|email|discussion|translation|general\"}.";
    std::string profile_transform_prompt =
        "Transform the dictated text according to the selected profile. "
        "coding: make it precise, actionable, and implementation-ready for a "
        "coding model. email: format as a clear email draft with appropriate "
        "tone; do not add placeholders, signature, or subject unless the user "
        "asked for them. discussion: preserve conversational "
        "nuance and make the point clear. translation: translate or prepare "
        "translation according to the user's request; preserve meaning and "
        "target language. general: clean without special formatting. Preserve "
        "intent and do not add new requirements. Return only the transformed text.";
    std::vector<LlmProfileDefinition> profiles = {
        {"coding", "Make it precise, actionable, and implementation-ready for a coding model."},
        {"email", "Format it as a clear email draft with appropriate tone."},
        {"discussion", "Preserve conversational nuance and make the point clear."},
        {"translation", "Translate or prepare translation according to the user's request."},
        {"general", "Clean without special formatting."},
    };
};

struct LlmProfileResult {
    std::string text;
    std::string profile = "general";
};

class LlmCleaner {
public:
    void configure(const LlmCleanerConfig& config);

    // Returns true only when text was successfully cleaned by the local LLM.
    bool clean_text(const std::string& input,
                    const std::string& learned_corrections,
                    std::string& output) const;
    bool clean_text_with_profile(const std::string& input,
                                 const std::string& learned_corrections,
                                 LlmProfileResult& result) const;
    bool detect_profile(const std::string& input, std::string& profile) const;
    bool transform_text_with_profile(const std::string& input,
                                     const std::string& learned_corrections,
                                     const std::string& profile,
                                     std::string& output) const;
    bool transform_text(const std::string& input,
                        const std::string& system_prompt,
                        std::string& output) const;
    bool warm_up() const;

private:
    bool request_completion(const std::string& input,
                            const std::string& system_prompt,
                            std::string& output) const;

    LlmCleanerConfig config_;
};

} // namespace vtt
