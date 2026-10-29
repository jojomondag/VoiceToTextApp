#include "pianissimo_engine.h"
#include "speech_model_catalog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <transcribe.h>
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace vtt {
namespace {

std::filesystem::path runtime_directory() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length == buffer.size()) throw std::runtime_error("Unable to locate speech runtime directory.");
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}
HMODULE load_runtime(const wchar_t* name) {
    const auto path = runtime_directory() / name;
    auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) throw std::runtime_error("Unable to load Pianissimo runtime " + path.filename().u8string() +
                                         " (Windows error " + std::to_string(GetLastError()) + "). Reinstall the complete application package.");
    return module;
}
template <typename T> T symbol(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(std::string("Speech runtime is missing API ") + name + ".");
    return reinterpret_cast<T>(address);
}

// The module stays loaded for the process lifetime: ggml's registered backend
// modules and ORT's API table can outlive an individual loaded model.
struct TranscribeApi {
    HMODULE module = nullptr;
#define VTT_TRANSCRIBE_FN(name) decltype(&transcribe_##name) name = nullptr
    VTT_TRANSCRIBE_FN(version);
    VTT_TRANSCRIBE_FN(init_backends_default);
    VTT_TRANSCRIBE_FN(backend_available);
    VTT_TRANSCRIBE_FN(model_load_params_init);
    VTT_TRANSCRIBE_FN(session_params_init);
    VTT_TRANSCRIBE_FN(run_params_init);
    VTT_TRANSCRIBE_FN(open);
    VTT_TRANSCRIBE_FN(session_free);
    VTT_TRANSCRIBE_FN(run);
    VTT_TRANSCRIBE_FN(full_text);
    VTT_TRANSCRIBE_FN(n_words);
    VTT_TRANSCRIBE_FN(word_init);
    VTT_TRANSCRIBE_FN(get_word);
    VTT_TRANSCRIBE_FN(status_string);
    VTT_TRANSCRIBE_FN(get_model);
    VTT_TRANSCRIBE_FN(model_backend);
#undef VTT_TRANSCRIBE_FN
    TranscribeApi() {
        module = load_runtime(L"transcribe.dll");
#define VTT_LOAD_FN(name) name = symbol<decltype(name)>(module, "transcribe_" #name)
        VTT_LOAD_FN(version); VTT_LOAD_FN(init_backends_default); VTT_LOAD_FN(backend_available);
        VTT_LOAD_FN(model_load_params_init); VTT_LOAD_FN(session_params_init); VTT_LOAD_FN(run_params_init);
        VTT_LOAD_FN(open); VTT_LOAD_FN(session_free); VTT_LOAD_FN(run); VTT_LOAD_FN(full_text);
        VTT_LOAD_FN(n_words); VTT_LOAD_FN(word_init); VTT_LOAD_FN(get_word); VTT_LOAD_FN(status_string);
        VTT_LOAD_FN(get_model); VTT_LOAD_FN(model_backend);
#undef VTT_LOAD_FN
        if (std::string(version()) != "0.2.3") throw std::runtime_error("Pianissimo requires transcribe.cpp 0.2.3; the packaged runtime version does not match.");
        const auto status = init_backends_default();
        if (status != TRANSCRIBE_OK) throw std::runtime_error(std::string("Unable to initialize speech backends: ") + status_string(status));
    }
};
TranscribeApi& transcribe_api() {
    static TranscribeApi api;
    return api;
}
void initialize_ort() {
    static std::once_flag once;
    std::call_once(once, [] {
        const auto module = load_runtime(L"onnxruntime.dll");
        const auto base = symbol<decltype(&OrtGetApiBase)>(module, "OrtGetApiBase")();
        const auto* api = base->GetApi(ORT_API_VERSION);
        if (!api) throw std::runtime_error("Pianissimo requires the packaged ONNX Runtime 1.28 API.");
        Ort::InitApi(api);
    });
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}
bool begins_word(const std::string& value) {
    if (value.empty()) return false;
    wchar_t character[2]{};
    WORD kind[2]{};
    const auto first = static_cast<unsigned char>(value[0]);
    const int length = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), std::min<int>(length, static_cast<int>(value.size())), character, 2);
    return count > 0 && GetStringTypeW(CT_CTYPE1, character, count, kind) && (kind[0] & (C1_ALPHA | C1_DIGIT));
}

struct OnnxModel {
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "Pianissimo"};
    Ort::Session preprocessor{nullptr}, encoder{nullptr}, decoder{nullptr};
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<std::string> vocabulary;
    int blank = -1;
    std::vector<int64_t> state1_shape, state2_shape;

    OnnxModel(const std::filesystem::path& directory, const std::string& quantization, int threads) {
        vocabulary.resize(8193);
        std::vector<bool> seen(vocabulary.size(), false);
        std::ifstream input(directory / L"vocab.txt", std::ios::binary);
        if (!input) throw std::runtime_error("Unable to read Pianissimo vocabulary.");
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto separator = line.find_last_of(' ');
            if (separator == std::string::npos) throw std::runtime_error("Invalid Pianissimo vocabulary row.");
            const auto id = std::stoul(line.substr(separator + 1));
            if (id >= vocabulary.size() || seen[id]) throw std::runtime_error("Invalid Pianissimo vocabulary ID.");
            auto token = line.substr(0, separator);
            for (auto position = token.find(u8"▁"); position != std::string::npos; position = token.find(u8"▁", position + 1))
                token.replace(position, 3, " ");
            vocabulary[id] = std::move(token);
            seen[id] = true;
            if (vocabulary[id] == "<blk>") blank = static_cast<int>(id);
        }
        if (blank < 0 || std::find(seen.begin(), seen.end(), false) != seen.end()) throw std::runtime_error("Incomplete Pianissimo vocabulary.");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(threads > 0 ? threads : std::clamp(static_cast<int>(std::thread::hardware_concurrency() / 2), 1, 6));
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        const auto suffix = quantization == "fp32" ? std::string() : "." + quantization;
        preprocessor = Ort::Session(environment, (directory / L"nemo128.onnx").c_str(), options);
        encoder = Ort::Session(environment, (directory / std::filesystem::u8path("encoder-model" + suffix + ".onnx")).c_str(), options);
        decoder = Ort::Session(environment, (directory / std::filesystem::u8path("decoder_joint-model" + suffix + ".onnx")).c_str(), options);
        state1_shape = state_shape("input_states_1");
        state2_shape = state_shape("input_states_2");
    }
    std::vector<int64_t> state_shape(const char* name) {
        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i = 0; i < decoder.GetInputCount(); ++i) {
            if (decoder.GetInputNameAllocated(i, allocator).get() != std::string(name)) continue;
            auto shape = decoder.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() != 3 || shape[0] <= 0 || shape[2] <= 0) break;
            shape[1] = 1;
            return shape;
        }
        throw std::runtime_error(std::string("Invalid Pianissimo decoder state: ") + name);
    }
    template <typename T> Ort::Value tensor(T* data, size_t count, const std::vector<int64_t>& shape) {
        return Ort::Value::CreateTensor<T>(memory, data, count, shape.data(), shape.size());
    }
    std::vector<SpeechWord> recognize_window(const float* samples, size_t sample_count) {
        int64_t sample_length = static_cast<int64_t>(sample_count);
        std::array<Ort::Value, 2> audio_inputs = {
            tensor(const_cast<float*>(samples), sample_count, {1, sample_length}),
            tensor(&sample_length, 1, {1})
        };
        const char* preprocess_input_names[] = {"waveforms", "waveforms_lens"};
        const char* preprocess_output_names[] = {"features", "features_lens"};
        auto features = preprocessor.Run(Ort::RunOptions{nullptr}, preprocess_input_names, audio_inputs.data(), audio_inputs.size(), preprocess_output_names, 2);
        const char* encoder_input_names[] = {"audio_signal", "length"};
        const char* encoder_output_names[] = {"outputs", "encoded_lengths"};
        auto encoded = encoder.Run(Ort::RunOptions{nullptr}, encoder_input_names, features.data(), features.size(), encoder_output_names, 2);
        const auto frame_shape = encoded[0].GetTensorTypeAndShapeInfo().GetShape();
        const auto* frames = encoded[0].GetTensorData<float>();
        const auto length = encoded[1].GetTensorData<int64_t>()[0];
        if (frame_shape.size() != 3 || frame_shape[0] != 1 || frame_shape[1] <= 0 || length < 0 || length > frame_shape[2])
            throw std::runtime_error("Invalid Pianissimo encoder output.");
        const auto state_size = [](const auto& shape) { return static_cast<size_t>(std::accumulate(shape.begin(), shape.end(), int64_t{1}, std::multiplies<int64_t>())); };
        std::vector<float> state1(state_size(state1_shape)), state2(state_size(state2_shape));
        std::vector<float> frame(static_cast<size_t>(frame_shape[1]));
        int32_t previous = blank, target_length = 1;
        const char* decoder_input_names[] = {"encoder_outputs", "targets", "target_length", "input_states_1", "input_states_2"};
        const char* decoder_output_names[] = {"outputs", "output_states_1", "output_states_2"};
        std::vector<std::pair<std::string, double>> pieces;
        int64_t time = 0;
        int emitted = 0;
        while (time < length) {
            for (size_t channel = 0; channel < frame.size(); ++channel) frame[channel] = frames[channel * static_cast<size_t>(frame_shape[2]) + static_cast<size_t>(time)];
            std::array<Ort::Value, 5> inputs = {
                tensor(frame.data(), frame.size(), {1, frame_shape[1], 1}),
                tensor(&previous, 1, {1, 1}), tensor(&target_length, 1, {1}),
                tensor(state1.data(), state1.size(), state1_shape), tensor(state2.data(), state2.size(), state2_shape)
            };
            auto decoded = decoder.Run(Ort::RunOptions{nullptr}, decoder_input_names, inputs.data(), inputs.size(), decoder_output_names, 3);
            const auto count = decoded[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (count != vocabulary.size() + 5) throw std::runtime_error("Invalid Pianissimo TDT logits.");
            const auto* logits = decoded[0].GetTensorData<float>();
            const auto token = static_cast<int>(std::max_element(logits, logits + vocabulary.size()) - logits);
            const auto step = static_cast<int>(std::max_element(logits + vocabulary.size(), logits + count) - (logits + vocabulary.size()));
            if (token != blank) {
                if (decoded[1].GetTensorTypeAndShapeInfo().GetElementCount() != state1.size() ||
                    decoded[2].GetTensorTypeAndShapeInfo().GetElementCount() != state2.size())
                    throw std::runtime_error("Invalid Pianissimo decoder state output.");
                std::copy_n(decoded[1].GetTensorData<float>(), state1.size(), state1.begin());
                std::copy_n(decoded[2].GetTensorData<float>(), state2.size(), state2.begin());
                previous = token;
                pieces.emplace_back(vocabulary[token], time * 0.08);
                ++emitted;
            }
            if (step > 0) { time += step; emitted = 0; }
            else if (token == blank || emitted == 10) { ++time; emitted = 0; }
        }
        std::vector<SpeechWord> words;
        std::string text;
        double start = 0, end = 0;
        const auto flush = [&] {
            auto word = trim(text);
            if (!word.empty()) words.push_back({std::move(word), start, std::min(sample_count / 16000.0, end)});
            text.clear();
        };
        for (const auto& piece : pieces) {
            const auto token_text = trim(piece.first);
            const bool lexical = begins_word(token_text);
            if (!text.empty() && !piece.first.empty() && piece.first[0] == ' ' && lexical) flush();
            if (text.empty()) start = piece.second;
            // SentencePiece's leading space before punctuation is discarded,
            // matching TLH's DecodeSpaces word reconstruction.
            text += !token_text.empty() && !lexical ? token_text : piece.first;
            // TDT may emit sentence punctuation in a much later silent frame.
            // Attach it to the lexical word without stretching that word's
            // spoken interval, which would delay real time commitment.
            if (lexical || end < start) end = piece.second + 0.08;
        }
        flush();
        return words;
    }
    TranscriptionResult recognize(const std::vector<float>& audio) {
        constexpr size_t window = 30 * 16000;
        constexpr size_t overlap = 320 * 16;
        TranscriptionResult result;
        for (size_t start = 0; start < audio.size();) {
            const auto count = std::min(window, audio.size() - start);
            auto words = recognize_window(audio.data() + start, count);
            for (auto& word : words) {
                if (start && (word.start_seconds + word.end_seconds) / 2 <= overlap / 16000.0) continue;
                word.start_seconds += start / 16000.0;
                word.end_seconds += start / 16000.0;
                if (!result.text.empty()) result.text += ' ';
                result.text += word.text;
                result.words.push_back(std::move(word));
            }
            if (start + count == audio.size()) break;
            start += count - overlap;
        }
        return result;
    }
};

} // namespace

struct PianissimoEngine::Impl {
    transcribe_session* session = nullptr;
    std::unique_ptr<OnnxModel> onnx;
    std::string model_id;
    std::string backend;
    ~Impl() { if (session) transcribe_api().session_free(session); }
};
PianissimoEngine::PianissimoEngine() = default;
PianissimoEngine::~PianissimoEngine() { shutdown(); }
bool PianissimoEngine::initialize(const std::string& model_id, const std::string& model_root, bool use_gpu, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    try {
        const auto* model = find_speech_model(model_id);
        if (!model) throw std::runtime_error("Unknown Pianissimo model: " + model_id);
        if (!speech_model_files_exist(model_root, model_id)) throw std::runtime_error("Pianissimo model files are missing or incomplete. Download the selected model first.");
        // Release the previous model before loading a new one to bound memory
        // when switching between the 2.56 GB fp32 and quantized variants.
        initialized_ = false;
        impl_.reset();
        auto candidate = std::make_unique<Impl>();
        candidate->model_id = model_id;
        const auto directory = std::filesystem::u8path(speech_model_directory(model_root, model_id));
        if (model->is_onnx()) {
            initialize_ort();
            candidate->onnx = std::make_unique<OnnxModel>(directory, model->quantization, n_threads_);
            candidate->backend = "ONNX Runtime 1.28 / CPU";
        } else {
            auto& api = transcribe_api();
            transcribe_model_load_params load;
            api.model_load_params_init(&load);
            load.backend = use_gpu ? TRANSCRIBE_BACKEND_AUTO : TRANSCRIBE_BACKEND_CPU;
            transcribe_session_params session;
            api.session_params_init(&session);
            session.n_threads = n_threads_;
            const auto path = (directory / std::filesystem::u8path(model->files.front().filename)).u8string();
            const auto status = api.open(path.c_str(), &load, &session, &candidate->session);
            if (status != TRANSCRIBE_OK) throw std::runtime_error(std::string("Unable to load Pianissimo GGUF: ") + api.status_string(status));
            const auto* backend = api.model_backend(api.get_model(candidate->session));
            candidate->backend = std::string("transcribe.cpp 0.2.3 / ") + (backend ? backend : "unknown");
        }
        impl_ = std::move(candidate);
        initialized_ = true;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        initialized_ = false;
        return false;
    }
}
void PianissimoEngine::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    impl_.reset();
}
bool PianissimoEngine::is_initialized() const { return initialized_.load(); }
TranscriptionResult PianissimoEngine::transcribe(const std::vector<float>& audio_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    TranscriptionResult result;
    if (!impl_ || !initialized_) { result.error = "Pianissimo model is not loaded."; return result; }
    if (audio_data.empty()) return result;
    if (audio_data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) { result.error = "Audio is too large for the speech runtime."; return result; }
    try {
        if (impl_->onnx) return impl_->onnx->recognize(audio_data);
        auto& api = transcribe_api();
        transcribe_run_params parameters;
        api.run_params_init(&parameters);
        parameters.timestamps = TRANSCRIBE_TIMESTAMPS_WORD;
        const auto status = api.run(impl_->session, audio_data.data(), static_cast<int>(audio_data.size()), &parameters);
        if (status != TRANSCRIBE_OK) throw std::runtime_error(std::string("Pianissimo recognition failed: ") + api.status_string(status));
        const char* text = api.full_text(impl_->session);
        if (text) result.text = trim(text);
        const auto count = api.n_words(impl_->session);
        result.words.reserve(static_cast<size_t>(std::max(count, 0)));
        for (int i = 0; i < count; ++i) {
            transcribe_word word;
            api.word_init(&word);
            const auto word_status = api.get_word(impl_->session, i, &word);
            if (word_status != TRANSCRIBE_OK) throw std::runtime_error("Unable to read Pianissimo word timestamps.");
            if (word.text && *word.text) result.words.push_back({word.text, std::max(0.0, word.t0_ms / 1000.0), std::max(0.0, word.t1_ms / 1000.0)});
        }
    } catch (const std::exception& exception) {
        result = {};
        result.error = exception.what();
    }
    return result;
}
bool PianissimoEngine::warm_up(int duration_ms) {
    const std::vector<float> silence(static_cast<size_t>(std::max(1600, duration_ms * 16)), 0.0f);
    return transcribe(silence).error.empty();
}
void PianissimoEngine::set_n_threads(int threads) {
    std::lock_guard<std::mutex> lock(mutex_);
    n_threads_ = std::max(0, threads);
}
std::string PianissimoEngine::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_ ? "Pianissimo " + impl_->model_id + " (" + impl_->backend + ")" : "Pianissimo: model not loaded";
}
bool PianissimoEngine::gpu_available() {
    try {
        auto& api = transcribe_api();
        return api.backend_available(TRANSCRIBE_BACKEND_VULKAN) || api.backend_available(TRANSCRIBE_BACKEND_CUDA) || api.backend_available(TRANSCRIBE_BACKEND_ROCM);
    } catch (...) { return false; }
}

} // namespace vtt
