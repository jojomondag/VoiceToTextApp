#include "dictation_stream.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace vtt {
namespace {

constexpr std::size_t kRate = 16000;
constexpr std::size_t kMinimum = kRate * 400 / 1000;
constexpr std::size_t kStep = kRate * 320 / 1000;
constexpr std::size_t kWindow = kRate * 8;
constexpr std::size_t kCommit = kRate * 6;
constexpr std::size_t kPause = kRate * 600 / 1000;

std::string trim_word(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

DictationStream::DictationStream(float voice_threshold)
    : voice_threshold_(std::isfinite(voice_threshold)
        ? (std::clamp)(voice_threshold, 0.0005f, 0.003f) : 0.001f) {}

void DictationStream::append_audio(const std::vector<float>& samples) {
    if (finished_) return;
    const auto chunk_start = base_ + pending_.size();
    for (std::size_t offset = 0; offset < samples.size(); offset += 160) {
        const auto end = (std::min)(offset + 160, samples.size());
        double energy = 0;
        for (auto index = offset; index < end; ++index) {
            energy += static_cast<double>(samples[index]) * samples[index];
        }
        if (energy / (end - offset) > static_cast<double>(voice_threshold_) * voice_threshold_) {
            last_voice_ = chunk_start + end;
        }
    }
    pending_.insert(pending_.end(), samples.begin(), samples.end());
}

void DictationStream::discard_prefix(std::size_t count) {
    pending_.erase(pending_.begin(), pending_.begin() + count);
    base_ += count;
}

bool DictationStream::process_available(bool stopping, const Recognize& recognize, const Publish& publish) {
    if (finished_) return false;
    bool processed = false;
    while (pending_.size() > context_) {
        const bool speech_ended = last_voice_ > base_ + context_ &&
            base_ + pending_.size() >= last_voice_ + kPause;
        auto count = (std::min)(pending_.size(), kWindow);
        if (speech_ended) count = (std::min)(count, last_voice_ - base_ + kRate / 5);
        const bool full_window = count >= kWindow;
        const bool final = (stopping || speech_ended) && !full_window;
        if (!final && !full_window &&
            (count < kMinimum || base_ + count <= last_processed_ ||
             base_ + count - last_processed_ < kStep)) break;

        std::vector<float> input(pending_.begin(), pending_.begin() + count);
        double energy = 0;
        for (const float sample : input) energy += static_cast<double>(sample) * sample;
        TranscriptionResult result;
        if (count >= kRate / 4 && energy / count > 0.00000001) result = recognize(input);
        if (!result.error.empty()) throw std::runtime_error(result.error);

        std::string text = result.text;
        if (context_) {
            text.clear();
            for (const auto& word : result.words) {
                if ((word.start_seconds + word.end_seconds) * kRate / 2 <= context_) continue;
                const auto trimmed = trim_word(word.text);
                if (trimmed.empty()) continue;
                if (!text.empty()) text += ' ';
                text += trimmed;
            }
        }
        last_processed_ = base_ + count;
        processed = true;
        if (final || full_window) {
            publish(utterance_++, text, true);
            auto advance = count;
            context_ = 0;
            if (full_window) {
                // Match TLH: words already delivered are committed, and 320 ms
                // before that boundary survives solely as acoustic context.
                const double last_word_end = result.words.empty()
                    ? count / static_cast<double>(kRate) : result.words.back().end_seconds;
                const double samples = std::isfinite(last_word_end)
                    ? (std::clamp)(last_word_end * kRate, static_cast<double>(kCommit),
                                   static_cast<double>(count))
                    : static_cast<double>(count);
                advance = static_cast<std::size_t>(samples);
                context_ = kStep;
                advance -= context_;
            }
            discard_prefix(advance);
        } else if (!text.empty()) {
            publish(utterance_, text, false);
        } else if (count >= kRate && energy / count <= 0.00000001) {
            // A quiet lead-in must not fill the next recognition window.
            discard_prefix(count - kStep);
            context_ = 0;
        } else {
            break;
        }
    }
    if (stopping) {
        // Any remaining overlap was already committed by the preceding window.
        pending_.clear();
        context_ = 0;
        finished_ = true;
    }
    return processed;
}

} // namespace vtt
