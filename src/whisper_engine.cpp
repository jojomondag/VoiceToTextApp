#include "whisper_engine.h"
#include "whisper.h"

#include <sstream>
#include <algorithm>

namespace vtt {

WhisperEngine::WhisperEngine() = default;

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
    result.text = "";
    result.start_ms = 0;
    result.end_ms = 0;
    result.is_partial = false;

    if (!ctx_ || audio_data.empty()) {
        return result;
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    params.n_threads        = n_threads_;
    params.single_segment   = true;
    params.no_context       = true;
    params.no_timestamps    = false;
    params.print_progress   = false;
    params.print_realtime   = false;
    params.print_special    = false;
    params.print_timestamps = false;
    params.translate        = translate_;

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
    int64_t first_start = whisper_full_get_segment_t0(ctx_, 0) * 10; // convert to ms
    int64_t last_end    = 0;

    for (int i = 0; i < n_segments; ++i) {
        const char* seg_text = whisper_full_get_segment_text(ctx_, i);
        if (seg_text) {
            oss << seg_text;
        }
        last_end = whisper_full_get_segment_t1(ctx_, i) * 10; // convert to ms
    }

    result.text     = oss.str();
    result.start_ms = first_start;
    result.end_ms   = last_end;
    result.is_partial = false;

    // Trim leading whitespace that whisper sometimes produces
    auto pos = result.text.find_first_not_of(" \t\n\r");
    if (pos != std::string::npos && pos > 0) {
        result.text = result.text.substr(pos);
    }

    if (callback_) {
        callback_(result);
    }

    return result;
}

void WhisperEngine::set_callback(TranscriptionCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void WhisperEngine::set_language(const std::string& lang) {
    std::lock_guard<std::mutex> lock(mutex_);
    language_ = lang;
}

void WhisperEngine::set_translate(bool translate) {
    std::lock_guard<std::mutex> lock(mutex_);
    translate_ = translate;
}

void WhisperEngine::set_n_threads(int threads) {
    std::lock_guard<std::mutex> lock(mutex_);
    n_threads_ = std::max(1, threads);
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
