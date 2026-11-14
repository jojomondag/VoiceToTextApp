// Linux smoke test for the speech slice: provisions a Pianissimo model through
// the catalog, loads it in the engine and transcribes a 16 kHz mono WAV file.
//
//   linux_speech_smoke <model-id> <audio.wav> [--cpu] [--root <model directory>] [--skip-ensure]
//
// The default model root is the application's own, so downloads are shared.
// --skip-ensure loads the files already in the root without verifying them.
#include "pianissimo_engine.h"
#include "speech_model_catalog.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::uint32_t little_endian(const std::vector<char>& bytes, size_t offset, size_t width) {
    std::uint32_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return value;
}

std::vector<float> load_wav(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to open " + path);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("Not a RIFF/WAVE file: " + path);
    std::uint32_t format = 0, channels = 0, rate = 0, bits = 0;
    for (size_t offset = 12; offset + 8 <= bytes.size();) {
        const auto size = little_endian(bytes, offset + 4, 4);
        const auto body = offset + 8;
        const auto available = std::min<size_t>(size, bytes.size() - body);
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && available >= 16) {
            format = little_endian(bytes, body, 2);
            channels = little_endian(bytes, body + 2, 2);
            rate = little_endian(bytes, body + 4, 4);
            bits = little_endian(bytes, body + 14, 2);
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            if (channels != 1 || rate != 16000) throw std::runtime_error("Expected 16 kHz mono audio.");
            std::vector<float> samples;
            if (format == 1 && bits == 16) {
                samples.resize(available / 2);
                for (size_t i = 0; i < samples.size(); ++i)
                    samples[i] = static_cast<std::int16_t>(little_endian(bytes, body + i * 2, 2)) / 32768.0f;
            } else if (format == 3 && bits == 32) {
                samples.resize(available / 4);
                std::memcpy(samples.data(), bytes.data() + body, samples.size() * 4);
            } else {
                throw std::runtime_error("Expected 16-bit PCM or 32-bit float samples.");
            }
            return samples;
        }
        offset = body + size + (size & 1);
    }
    throw std::runtime_error("No audio data in " + path);
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

int main(int argc, char** argv) {
    std::string model_id, wav, root;
    bool use_gpu = true, ensure = true;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--cpu") use_gpu = false;
        else if (argument == "--skip-ensure") ensure = false;
        else if (argument == "--root" && i + 1 < argc) root = argv[++i];
        else if (model_id.empty()) model_id = argument;
        else if (wav.empty()) wav = argument;
    }
    if (model_id.empty() || wav.empty()) {
        std::cerr << "usage: linux_speech_smoke <model-id> <16 kHz mono wav> [--cpu] [--root <model directory>] [--skip-ensure]\nmodels:";
        for (const auto& model : vtt::speech_models()) std::cerr << ' ' << model.id;
        std::cerr << '\n';
        return 2;
    }
    if (root.empty()) {
        const char* data = std::getenv("XDG_DATA_HOME");
        const char* home = std::getenv("HOME");
        root = (data && *data ? std::string(data) : std::string(home ? home : ".") + "/.local/share") + "/VoiceToText/models";
    }
    try {
        const auto audio = load_wav(wav);
        std::printf("audio: %s, %.2f s\nmodel root: %s\n", wav.c_str(), audio.size() / 16000.0, root.c_str());

        std::string error;
        auto started = std::chrono::steady_clock::now();
        const auto progress = [](const std::string& message) { std::printf("  %s\n", message.c_str()); std::fflush(stdout); };
        if (ensure) {
            if (!vtt::ensure_speech_model(root, model_id, progress, nullptr, error)) throw std::runtime_error("ensure_speech_model: " + error);
            std::printf("ensure_speech_model: %.2f s\n", seconds_since(started));
        }

        std::printf("gpu_available: %s\n", vtt::PianissimoEngine::gpu_available() ? "true" : "false");
        vtt::PianissimoEngine engine;
        started = std::chrono::steady_clock::now();
        if (!engine.initialize(model_id, root, use_gpu, error)) throw std::runtime_error("initialize: " + error);
        std::printf("initialize (use_gpu=%s): %.2f s\nsystem info: %s\n", use_gpu ? "true" : "false", seconds_since(started), engine.get_system_info().c_str());

        started = std::chrono::steady_clock::now();
        if (!engine.warm_up()) throw std::runtime_error("warm_up failed");
        std::printf("warm_up: %.2f s\n", seconds_since(started));

        vtt::TranscriptionResult result;
        for (int pass = 1; pass <= 2; ++pass) {
            started = std::chrono::steady_clock::now();
            result = engine.transcribe(audio);
            if (!result.error.empty()) throw std::runtime_error("transcribe: " + result.error);
            std::printf("transcribe pass %d: %.3f s\n", pass, seconds_since(started));
        }
        std::printf("text: %s\nwords (%zu):\n", result.text.c_str(), result.words.size());
        for (const auto& word : result.words) std::printf("  %7.2f - %7.2f  %s\n", word.start_seconds, word.end_seconds, word.text.c_str());
        return result.text.empty() ? 1 : 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "FAILED: %s\n", exception.what());
        return 1;
    }
}
