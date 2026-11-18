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
    // Sent as "Authorization: Bearer ..." when not empty (hosted APIs).
    std::string api_key;
    // Names the caller in the per-request log line ("live ai", "history").
    std::string log_label;
    // Lets requests use the cacheable layout (see uses_cache_layout()). Off:
    // the plain body, and the server is never asked what it is.
    bool cache_layout = false;
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

    // The system prompt transform_text() sends for `system_prompt`. A prime
    // must send exactly this (one changed character makes the server's cache
    // miss), so both build it here.
    static std::string rewrite_system_prompt(const std::string& system_prompt);

    // True when the configuration allows the cacheable layout and the
    // endpoint is a TensorFold server. Requests to it then put the
    // fixed instruction in front of a short fixed exchange, a position where
    // the server can resume a cached prompt, so the instruction is not read
    // again for every dictation. Asked once per endpoint (GET .../models,
    // short timeout); a server that did not answer is asked again later.
    // Blocking: may wait for that answer.
    bool uses_cache_layout() const;
    // Makes the server keep the cached state of `system_prompt` (the final
    // prompt as sent, e.g. from rewrite_system_prompt()) for the next request.
    // Three tiny requests; results are ignored. Does nothing unless the
    // endpoint is a TensorFold server, or while a prime of the same prompt
    // is running. Blocking: call it on a background thread.
    bool prime(const std::string& system_prompt, bool enable_thinking = false) const;
    // Per-request log lines go here (none until set). Set once at startup.
    using LogSink = void (*)(const char* line);
    static void set_log_sink(LogSink sink);

    // Why the calling thread's last request produced no usable text (empty
    // after a success). Kept per thread, so a shared cleaner stays race-free.
    // For diagnostics only.
    static const std::string& last_failure();

private:
    bool request_completion(const std::string& input,
                            const std::string& system_prompt,
                            std::string& output,
                            bool enable_thinking,
                            const char* kind) const;
    bool server_allows_cache_layout(bool ask_again_now) const;

    LlmCleanerConfig config_;
};

} // namespace vtt
