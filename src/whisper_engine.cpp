#include "whisper_engine.h"
#include "whisper.h"

#include <sstream>
#include <algorithm>
#include <thread>

namespace vtt {

WhisperEngine::~WhisperEngine() {
    shutdown();
}

bool WhisperEngine::initialize(const std::string& model_path, bool use_gpu) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (ctx_) {
        whisper_free(ctx_);
        ctx_ = nullptr;
    }

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = use_gpu;
    cparams.flash_attn = use_gpu;

    ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx_) {
        initialized_ = false;
        return false;
    }

    initialized_ = true;
    return true;
}

void WhisperEngine::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (ctx_) {
        whisper_free(ctx_);
        ctx_ = nullptr;
    }
    initialized_ = false;
}

bool WhisperEngine::is_initialized() const {
    return initialized_.load();
}

TranscriptionResult WhisperEngine::transcribe(const std::vector<float>& audio_data) {
    std::lock_guard<std::mutex> lock(mutex_);

    TranscriptionResult result;

    if (!ctx_ || audio_data.empty()) {
        return result;
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    params.n_threads        = n_threads_;
    params.single_segment   = true;
    params.no_context       = true;
    params.no_timestamps    = fast_mode_;
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_special    = false;
    params.print_timestamps = false;
    params.translate        = false;

    if (fast_mode_) {
        params.suppress_blank  = true;
        params.suppress_nst    = true;
    }

    if (language_ == "auto") {
        params.language = nullptr;  // auto-detect
    } else {
        params.language = language_.c_str();
    }

    int ret = whisper_full(ctx_, params, audio_data.data(), static_cast<int>(audio_data.size()));
    if (ret != 0) {
        return result;
    }

    int n_segments = whisper_full_n_segments(ctx_);
    if (n_segments <= 0) {
        return result;
    }

    std::ostringstream oss;

    for (int i = 0; i < n_segments; ++i) {
        const char* seg_text = whisper_full_get_segment_text(ctx_, i);
        if (seg_text) {
            oss << seg_text;
        }
    }

    result.text = oss.str();

    // Trim leading whitespace that whisper sometimes produces
    auto pos = result.text.find_first_not_of(" \t\n\r");
    if (pos != std::string::npos && pos > 0) {
        result.text = result.text.substr(pos);
    }

    return result;
}

bool WhisperEngine::warm_up(int duration_ms) {
    int samples = std::max(1600, duration_ms * 16);
    std::vector<float> silence(static_cast<size_t>(samples), 0.0f);
    transcribe(silence);
    return is_initialized();
}

void WhisperEngine::set_language(const std::string& lang) {
    std::lock_guard<std::mutex> lock(mutex_);
    language_ = lang;
}

void WhisperEngine::set_n_threads(int threads) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (threads <= 0) {
        unsigned int hw_threads = std::thread::hardware_concurrency();
        n_threads_ = static_cast<int>(std::max(1u, hw_threads));
        return;
    }
    n_threads_ = std::max(1, threads);
}

void WhisperEngine::set_fast_mode(bool fast_mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    fast_mode_ = fast_mode;
}

std::string WhisperEngine::get_system_info() const {
    std::ostringstream oss;
    oss << "whisper.cpp system info:\n";
    oss << "  " << whisper_print_system_info() << "\n";
    if (ctx_) {
        oss << "  Model loaded\n";
    } else {
        oss << "  Model not loaded\n";
    }
    return oss.str();
}

} // namespace vtt
