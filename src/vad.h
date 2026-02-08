#pragma once
#include <vector>

namespace vtt {

class VoiceActivityDetector {
public:
    VoiceActivityDetector();

    // Configure
    void set_threshold(float energy_threshold); // 0.0 - 1.0, default 0.01
    void set_silence_duration_ms(int ms); // how long silence before stop, default 1500ms
    void set_speech_duration_ms(int ms); // minimum speech to trigger, default 200ms

    // Process audio chunk, returns true if speech is detected
    bool process(const std::vector<float>& audio_chunk, int sample_rate = 16000);

    // State
    bool is_speech_active() const;
    int silence_duration_ms() const;

    void reset();

private:
    float energy_threshold_ = 0.01f;
    int silence_threshold_ms_ = 1500;
    int speech_threshold_ms_ = 200;
    bool speech_active_ = false;
    int silence_counter_ms_ = 0;
    int speech_counter_ms_ = 0;
    float smoothed_energy_ = 0.0f;
};

} // namespace vtt
