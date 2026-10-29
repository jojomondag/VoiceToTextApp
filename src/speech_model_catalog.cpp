#include "speech_model_catalog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace vtt {
namespace {

constexpr const char* onnx_root = "https://huggingface.co/KlangAI/pianissimo-sv-onnx/resolve/63730c6021234f26b9bbae9a07a04fec39e7a52e/";
SpeechModelFile onnx_file(const char* name, std::uint64_t bytes, const char* hash) {
    return {name, std::string(onnx_root) + name, hash, bytes};
}
SpeechModelFile preprocessor() {
    return onnx_file("nemo128.onnx", 138824, "5b4a84c52eeaa615dc46d781cc7e4598f9b432184831d57493c48adcd01371a9");
}
SpeechModelFile vocabulary() {
    return onnx_file("vocab.txt", 93939, "d58544679ea4bc6ac563d1f545eb7d474bd6cfa467f0a6e2c1dc1c7d37e3c35d");
}
std::wstring wide(const std::string& input) {
    if (input.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Invalid UTF-8 model URL.");
    std::wstring value(size, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), value.data(), size);
    return value;
}
void check_cancel(const std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("Model download cancelled.");
}
void report(const SpeechModelProgress& progress, const std::string& value) {
    if (progress) progress(value);
}
struct HttpCloser { void operator()(void* value) const { if (value) WinHttpCloseHandle(value); } };
using HttpHandle = std::unique_ptr<void, HttpCloser>;
void http_check(bool success, const char* operation) {
    if (!success) throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ").");
}
struct Hash {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Hash() {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("Unable to initialize SHA-256.");
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
            throw std::runtime_error("Unable to create SHA-256 hash.");
        }
    }
    ~Hash() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    void add(char* bytes, ULONG size) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes), size, 0) < 0)
            throw std::runtime_error("Unable to hash model file.");
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("Unable to finish model hash.");
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (auto byte : digest) output << std::setw(2) << static_cast<int>(byte);
        return output.str();
    }
};
bool verify(const std::filesystem::path& path, const SpeechModelFile& file, const std::atomic<bool>* cancel) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) < file.minimum_bytes || ec) return false;
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to read " + file.filename + ".");
    Hash hash;
    std::array<char, 128 * 1024> buffer{};
    while (input) {
        check_cancel(cancel);
        input.read(buffer.data(), buffer.size());
        if (input.gcount() > 0) hash.add(buffer.data(), static_cast<ULONG>(input.gcount()));
    }
    if (!input.eof()) throw std::runtime_error("Unable to read complete model file " + file.filename + ".");
    return hash.finish() == file.sha256;
}
void download(const std::filesystem::path& path, const SpeechModelFile& file,
              const SpeechModelProgress& progress, const std::atomic<bool>* cancel) {
    const auto url = wide(file.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    http_check(WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts), "Invalid model URL");
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) throw std::runtime_error("Model downloads require HTTPS.");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring request_path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) request_path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HttpHandle session(WinHttpOpen(L"VoiceToText-Pianissimo/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    http_check(session != nullptr, "Unable to open model download");
    http_check(WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 30000), "Unable to set download timeout");
    HttpHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    http_check(connection != nullptr, "Unable to connect to model host");
    HttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", request_path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    http_check(request != nullptr, "Unable to open model request");
    check_cancel(cancel);
    http_check(WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0), "Unable to request model");
    http_check(WinHttpReceiveResponse(request.get(), nullptr), "Unable to receive model");
    DWORD status = 0, size = sizeof(status);
    http_check(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                   WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX), "Unable to read model HTTP status");
    if (status != 200) throw std::runtime_error(file.filename + ": HTTP " + std::to_string(status) + ".");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Unable to write " + file.filename + ".");
    std::array<char, 128 * 1024> buffer{};
    std::uint64_t received = 0;
    auto last_report = std::chrono::steady_clock::now();
    for (;;) {
        check_cancel(cancel);
        DWORD count = 0;
        http_check(WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count), "Model download interrupted");
        if (!count) break;
        output.write(buffer.data(), count);
        if (!output) throw std::runtime_error("Unable to save model; check available disk space.");
        received += count;
        if (std::chrono::steady_clock::now() - last_report >= std::chrono::milliseconds(500)) {
            report(progress, file.filename + ": " + std::to_string(received / 1000000) + " / " + std::to_string(file.minimum_bytes / 1000000) + " MB");
            last_report = std::chrono::steady_clock::now();
        }
    }
    output.close();
    if (!output) throw std::runtime_error("Unable to finish writing model file.");
}

} // namespace

const std::vector<SpeechModelInfo>& speech_models() {
    static const std::vector<SpeechModelInfo> models = {
        {"gguf-q8", "Pianissimo Swedish Q8", "Q8 - GPU (~740 MB)", "", {
            {"pianissimo-sv-Q8_0.gguf", "https://huggingface.co/nub235/pianissimo-sv-gguf/resolve/main/pianissimo-sv-Q8_0.gguf",
             "350bd38ea3849e0d6c05e680595ef81c185678e2d28eec0524ef704f4c345a4f", 650000000}}},
        {"onnx-int4", "Pianissimo Swedish int4", "int4 - smallest (~515 MB)", "int4", {preprocessor(), vocabulary(),
            onnx_file("decoder_joint-model.int4.onnx", 28378227, "bf9b99051419623751e34d542c0076ab50f84bbde4f68f095850b2d4cefdafcd"),
            onnx_file("encoder-model.int4.onnx", 486421211, "689b18b1c58df738e4a309757a817f4a8fcdad8e99d770f6810b3449be95b729")}},
        {"onnx-int8", "Pianissimo Swedish int8", "int8 - recommended CPU (~660 MB)", "int8", {preprocessor(), vocabulary(),
            onnx_file("decoder_joint-model.int8.onnx", 30025652, "0f9213242acd8874f2717c5e3cdb888d4e7673ddf3abb9ee3a2fd2eaf0acdd03"),
            onnx_file("encoder-model.int8.onnx", 630313965, "13288a5f4f009bf6bf8a3260044d098f6922e8caf1c564521d4962fa5749f66c")}},
        {"onnx-fp16", "Pianissimo Swedish fp16", "fp16 - higher precision (~1.28 GB)", "fp16", {preprocessor(), vocabulary(),
            onnx_file("decoder_joint-model.fp16.onnx", 36267462, "a3df1639600cfee598f3a003f4bdee0f76170d0f104effb88885236749b0c4aa"),
            onnx_file("encoder-model.fp16.onnx", 1246604787, "1e7c36488dc3946eb2f0ea487135beaf1408b6f1967bad1be56908a7f12c3466")}},
        {"onnx-fp32", "Pianissimo Swedish fp32", "fp32 - full precision (~2.56 GB)", "fp32", {preprocessor(), vocabulary(),
            onnx_file("decoder_joint-model.onnx", 72520893, "1b796d178bd5f0e59fe4dd636f2a0d9be027c935d105ceedda3babc15c06d2ac"),
            onnx_file("encoder-model.onnx", 356069854, "8939fa038d96baef7567e8e07de56acbec28815ab0ebcbefb9eff009ccf70261"),
            onnx_file("encoder-model.onnx.data", 2133430272, "266f27c808431df1316bdb773bbf8284ffb8fae40dd847afba52da0bcfe656de")}},
    };
    return models;
}
const SpeechModelInfo* find_speech_model(const std::string& id) {
    const auto& models = speech_models();
    const auto match = std::find_if(models.begin(), models.end(), [&](const auto& model) { return model.id == id; });
    return match == models.end() ? nullptr : &*match;
}
std::string normalize_speech_model_id(const std::string& id) {
    return find_speech_model(id) ? id : kDefaultSpeechModelId;
}
std::string speech_model_directory(const std::string& root, const std::string& model_id) {
    const auto* model = find_speech_model(model_id);
    const auto path = std::filesystem::u8path(root);
    return (model && model->is_onnx() ? path / L"PianissimoOnnx" : path).u8string();
}
bool speech_model_files_exist(const std::string& root, const std::string& model_id) {
    const auto* model = find_speech_model(model_id);
    if (!model) return false;
    const auto directory = std::filesystem::u8path(speech_model_directory(root, model_id));
    for (const auto& file : model->files) {
        std::error_code ec;
        const auto path = directory / std::filesystem::u8path(file.filename);
        if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) < file.minimum_bytes || ec) return false;
    }
    return true;
}
bool ensure_speech_model(const std::string& root, const std::string& model_id,
                         const SpeechModelProgress& progress, const std::atomic<bool>* cancel,
                         std::string& error) {
    error.clear();
    try {
        const auto* model = find_speech_model(model_id);
        if (!model) throw std::runtime_error("Unknown Pianissimo model: " + model_id);
        if (root.empty()) throw std::runtime_error("Pianissimo model directory is empty.");
        const auto directory = std::filesystem::u8path(speech_model_directory(root, model_id));
        std::filesystem::create_directories(directory);
        for (const auto& file : model->files) {
            check_cancel(cancel);
            const auto path = directory / std::filesystem::u8path(file.filename);
            report(progress, "Verifying " + file.filename + "...");
            if (verify(path, file, cancel)) continue;
            auto temporary = path; temporary += L".downloading";
            try {
                report(progress, "Downloading " + file.filename + "...");
                download(temporary, file, progress, cancel);
                report(progress, "Verifying " + file.filename + "...");
                if (!verify(temporary, file, cancel)) throw std::runtime_error("SHA-256 verification failed for " + file.filename + ".");
                check_cancel(cancel);
                http_check(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Unable to activate model file");
            } catch (...) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                throw;
            }
        }
        report(progress, model->name + " ready.");
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace vtt
