#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vtt {

inline constexpr const char* kDefaultSpeechModelId = "onnx-int8";
inline constexpr const char* kSpeechModelSourceUrl = "https://huggingface.co/KlangAI/pianissimo-sv-onnx";

struct SpeechModelFile {
    std::string filename;
    std::string url;
    std::string sha256;
    std::uint64_t minimum_bytes = 0;
};

struct SpeechModelInfo {
    std::string id;
    std::string name;
    std::string label;
    std::string quantization; // Empty for the GGUF provider.
    std::vector<SpeechModelFile> files;
    bool is_onnx() const { return !quantization.empty(); }
};

const std::vector<SpeechModelInfo>& speech_models();
const SpeechModelInfo* find_speech_model(const std::string& id);
std::string normalize_speech_model_id(const std::string& id);
// root/PianissimoOnnx for ONNX, root for GGUF; matches TeachersLittleHelper.
std::string speech_model_directory(const std::string& root, const std::string& model_id);
bool speech_model_files_exist(const std::string& root, const std::string& model_id);
using SpeechModelProgress = std::function<void(const std::string&)>;
// Verifies cached/downloaded files with SHA-256 and promotes complete files atomically.
// Cancellation is observed during hashing, downloading, and between files.
bool ensure_speech_model(const std::string& root, const std::string& model_id,
                         const SpeechModelProgress& progress, const std::atomic<bool>* cancel,
                         std::string& error);

} // namespace vtt
