#pragma once
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <string>
#include <memory>

namespace vtt {

struct AudioDevice {
    std::string name;
    std::string id;
    bool is_default;
};

using AudioCallback = std::function<void(const std::vector<float>&)>;

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    bool initialize();
    void shutdown();

    // Device management
    std::vector<AudioDevice> get_devices() const;
    bool select_device(const std::string& device_id); // empty = default

    // Recording control
    bool start_recording();
    void stop_recording();
    bool is_recording() const;

    // Get accumulated audio data and clear buffer
    std::vector<float> get_audio_buffer();

    // Set callback for real-time audio chunks
    void set_audio_callback(AudioCallback callback);

    // Get current audio level (0.0 - 1.0) for UI
    float get_audio_level() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vtt
