#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

// Forward declare
struct whisper_context;

namespace vtt {

struct TranscriptionResult {
    std::string text;
};

class WhisperEngine {
public:
    ~WhisperEngine();

    bool initialize(const std::string& model_path, bool use_gpu = true);
    void shutdown();
    bool is_initialized() const;

    // Process audio samples (16kHz mono float32)
    // Returns transcription result
    TranscriptionResult transcribe(const std::vector<float>& audio_data);
    bool warm_up(int duration_ms = 500);

    // Configuration
    void set_language(const std::string& lang); // "sv" for Swedish, "en" for English, "auto" for auto-detect
    void set_n_threads(int threads);
    void set_fast_mode(bool fast_mode);

    std::string get_system_info() const;

private:
    whisper_context* ctx_ = nullptr;
    std::string language_ = "sv";
    bool fast_mode_ = true;
    int n_threads_ = 4;
    std::mutex mutex_;
    std::atomic<bool> initialized_{false};
};

} // namespace vtt
