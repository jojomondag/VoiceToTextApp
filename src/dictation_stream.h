#pragma once

#include "pianissimo_engine.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace vtt {

// Portable streaming state, shared by live capture and synthetic verification.
// Audio is mono 16 kHz; callbacks run synchronously on the calling worker thread.
class DictationStream {
public:
    using Recognize = std::function<TranscriptionResult(const std::vector<float>&)>;
    using Publish = std::function<void(int utterance_id, const std::string& text, bool final)>;

    explicit DictationStream(float voice_threshold = 0.001f);
    void append_audio(const std::vector<float>& samples);
    // Drains full windows and final tails. A non-final preview waits for 320 ms
    // of fresh audio, except that a full eight-second window always finalizes.
    // Once the microphone is stopped, append its last samples and call with true.
    bool process_available(bool stopping, const Recognize& recognize, const Publish& publish);

private:
    void discard_prefix(std::size_t count);

    float voice_threshold_;
    std::vector<float> pending_;
    std::size_t base_ = 0;
    std::size_t context_ = 0;
    std::size_t last_processed_ = 0;
    std::size_t last_voice_ = 0;
    int utterance_ = 0;
    bool finished_ = false;
};

} // namespace vtt
