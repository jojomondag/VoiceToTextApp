#pragma once
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

// Forward declare
struct whisper_context;

namespace vtt {

struct TranscriptionResult {
    std::string text;
    int64_t start_ms;
    int64_t end_ms;
    bool is_partial;
};

using TranscriptionCallback = std::function<void(const TranscriptionResult&)>;

class WhisperEngine {
public:
    WhisperEngine();
    ~WhisperEngine();

    bool initialize(const std::string& model_path, bool use_gpu = true);
    void shutdown();
    bool is_initialized() const;

    // Process audio samples (16kHz mono float32)
    // Returns transcription result
    TranscriptionResult transcribe(const std::vector<float>& audio_data);
    bool warm_up(int duration_ms = 500);

    // Set callback for real-time results
    void set_callback(TranscriptionCallback callback);

    // Configuration
    void set_language(const std::string& lang); // "sv" for Swedish, "en" for English, "auto" for auto-detect
    void set_translate(bool translate); // translate to English
    void set_n_threads(int threads);
    void set_fast_mode(bool fast_mode);

    std::string get_system_info() const;

private:
    whisper_context* ctx_ = nullptr;
    TranscriptionCallback callback_;
    std::string language_ = "sv";
    bool translate_ = false;
    bool fast_mode_ = true;
    int n_threads_ = 4;
    std::mutex mutex_;
    std::atomic<bool> initialized_{false};
};

} // namespace vtt
