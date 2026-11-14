#pragma once
#include <vector>
#include <string>
#include <memory>

namespace vtt {

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    bool initialize();
    void shutdown();

    bool select_device(const std::string& device_id); // empty = default

    // Recording control
    bool prepare_device();
    bool start_recording();
    void stop_recording();
    bool is_recording() const;
    // A stable, human-readable reason for the last initialization/start
    // failure.  The UI uses this instead of collapsing every backend error
    // into the unhelpful "Mic error" message.
    std::string last_error() const;

    // Get accumulated audio data and clear buffer
    // Drain 16 kHz audio. Keep the resampling phase between live drains;
    // final=true flushes the last interpolation sample after capture stops.
    std::vector<float> get_audio_buffer(bool final = true);

    // Get current audio level (0.0 - 1.0) for UI
    float get_audio_level() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vtt
