#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vtt {

struct SpeechWord {
    std::string text;
    double start_seconds = 0;
    double end_seconds = 0;
};

struct TranscriptionResult {
    std::string text;
    std::vector<SpeechWord> words;
    std::string error;
};

class PianissimoEngine {
public:
    PianissimoEngine();
    ~PianissimoEngine();
    PianissimoEngine(const PianissimoEngine&) = delete;
    PianissimoEngine& operator=(const PianissimoEngine&) = delete;

    // Provision with ensure_speech_model first. ONNX uses CPU, GGUF uses the
    // best available GPU when requested, with the library's CPU fallback.
    bool initialize(const std::string& model_id, const std::string& model_root,
                    bool use_gpu, std::string& error);
    void shutdown();
    bool is_initialized() const;
    TranscriptionResult transcribe(const std::vector<float>& audio_data);
    bool warm_up(int duration_ms = 500);
    void set_n_threads(int threads);
    std::string get_system_info() const;
    static bool gpu_available();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    mutable std::mutex mutex_;
    std::atomic<bool> initialized_{false};
    int n_threads_ = 0;
};

} // namespace vtt
