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
    // Prompts and profiles are supplied from AppConfig when configured.
    std::string pre_prompt;
    int timeout_ms = 30000;
    int max_tokens = 4096;
    bool intent_profiles_enabled = true;
    std::string intent_detection_prompt;
    std::string profile_transform_prompt;
    std::vector<LlmProfileDefinition> profiles;
};

struct LlmProfileResult {
    std::string text;
    std::string profile = "general";
};

class LlmCleaner {
public:
    void configure(const LlmCleanerConfig& config);

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
                        std::string& output,
                        bool enable_thinking = false) const;
    bool warm_up() const;

private:
    bool request_completion(const std::string& input,
                            const std::string& system_prompt,
                            std::string& output,
                            bool enable_thinking = false) const;

    LlmCleanerConfig config_;
};

} // namespace vtt
