#include "dictation_stream.h"

#include <algorithm>
#include <cctype>
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
// A word that ends this close to the end of a full window may continue past
// it (and is then cut, "skärgård"). It is left to the next window, which hears
// it whole.
constexpr double kEdgeGuardSeconds = 0.5;
// A word the next window repeats from the end of the previous one is dropped
// when it starts this soon after its acoustic context.
constexpr double kRepeatWindowSeconds = 0.8;

std::string trim_word(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Comparable form of a word: ASCII lower case, without ASCII punctuation.
std::string word_key(const std::string& word) {
    std::string key;
    for (const char c : word) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x80 && (std::ispunct(byte) || std::isspace(byte))) continue;
        key.push_back(byte < 0x80 ? static_cast<char>(std::tolower(byte)) : c);
    }
    return key;
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

        const auto& words = result.words;
        // Words that are new in this window: after the acoustic context kept
        // from the previous window, and not a repeat of what it committed.
        std::vector<std::size_t> fresh;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto& word = words[index];
            if (context_ && (word.start_seconds + word.end_seconds) * kRate / 2 <= context_) continue;
            if (trim_word(word.text).empty()) continue;
            fresh.push_back(index);
        }
        if (context_ && !committed_tail_.empty()) {
            for (auto n = (std::min)({committed_tail_.size(), fresh.size(), std::size_t{3}}); n > 0; --n) {
                if (words[fresh[n - 1]].start_seconds * kRate >
                    context_ + kRepeatWindowSeconds * kRate) continue;
                bool same = true;
                for (std::size_t k = 0; k < n && same; ++k) {
                    same = word_key(words[fresh[k]].text) ==
                           committed_tail_[committed_tail_.size() - n + k];
                }
                if (same) {
                    fresh.erase(fresh.begin(), fresh.begin() + static_cast<std::ptrdiff_t>(n));
                    break;
                }
            }
        }
        const auto join = [&](std::size_t limit) {
            std::string joined;
            for (const auto index : fresh) {
                if (index >= limit) break;
                if (!joined.empty()) joined += ' ';
                joined += trim_word(words[index].text);
            }
            return joined;
        };
        std::string text = context_ ? join(words.size()) : result.text;

        // A full window commits only the words that end clearly before its
        // edge; the rest is heard again, whole, in the next window.
        std::size_t commit_words = words.size();
        double commit_seconds = -1;
        if (full_window && !words.empty()) {
            const double window_seconds = static_cast<double>(count) / kRate;
            std::size_t keep = words.size();
            while (keep > 0 &&
                   !(words[keep - 1].end_seconds <= window_seconds - kEdgeGuardSeconds)) {
                --keep;
            }
            if (keep > 0 && words[keep - 1].end_seconds * kRate >
                                static_cast<double>(context_ + kStep + kRate)) {
                commit_words = keep;
                commit_seconds = words[keep - 1].end_seconds;
                text = join(commit_words);
            }
        }
        last_processed_ = base_ + count;
        processed = true;
        if (final || full_window) {
            publish(utterance_++, text, true);
            std::vector<std::string> tail;
            for (const auto index : fresh) {
                if (index < commit_words) tail.push_back(word_key(words[index].text));
            }
            if (tail.size() > 3) tail.erase(tail.begin(), tail.end() - 3);
            committed_tail_ = full_window ? tail : std::vector<std::string>{};
            auto advance = count;
            context_ = 0;
            if (full_window) {
                // Match TLH: words already delivered are committed, and 320 ms
                // before that boundary survives solely as acoustic context.
                double samples = 0;
                if (commit_seconds >= 0) {
                    samples = commit_seconds * kRate;
                } else {
                    const double last_word_end = result.words.empty()
                        ? count / static_cast<double>(kRate) : result.words.back().end_seconds;
                    samples = std::isfinite(last_word_end)
                        ? (std::clamp)(last_word_end * kRate, static_cast<double>(kCommit),
                                       static_cast<double>(count))
                        : static_cast<double>(count);
                }
                advance = (std::min)(static_cast<std::size_t>(samples), count);
                context_ = kStep;
                advance -= context_;
            }
            discard_prefix(advance);
        } else if (!text.empty()) {
            // Close to a full window, a preview shows only what that window
            // will commit, so no word is shown that is later heard again.
            if (count + kRate > kWindow) {
                const double shown_until = static_cast<double>(count) / kRate - kEdgeGuardSeconds;
                std::size_t limit = 0;
                while (limit < words.size() && words[limit].end_seconds <= shown_until) ++limit;
                text = join(limit);
            }
            if (!text.empty()) publish(utterance_, text, false);
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
