#include "vad.h"
#include <cmath>

namespace vtt {

VoiceActivityDetector::VoiceActivityDetector() = default;

void VoiceActivityDetector::set_threshold(float energy_threshold) {
    energy_threshold_ = energy_threshold;
}

void VoiceActivityDetector::set_silence_duration_ms(int ms) {
    silence_threshold_ms_ = ms;
}

void VoiceActivityDetector::set_speech_duration_ms(int ms) {
    speech_threshold_ms_ = ms;
}

bool VoiceActivityDetector::process(const std::vector<float>& audio_chunk, int sample_rate) {
    if (audio_chunk.empty()) return speech_active_;

    // Calculate RMS energy of the chunk
    float sum_sq = 0.0f;
    for (float sample : audio_chunk) {
        sum_sq += sample * sample;
    }
    float rms = std::sqrt(sum_sq / static_cast<float>(audio_chunk.size()));

    // Exponential moving average for smoothing
    float alpha = 0.3f;
    smoothed_energy_ = smoothed_energy_ + alpha * (rms - smoothed_energy_);

    // Calculate chunk duration in ms
    int chunk_duration_ms = static_cast<int>(
        (static_cast<float>(audio_chunk.size()) / static_cast<float>(sample_rate)) * 1000.0f
    );

    // Use hysteresis: higher threshold to start speech, lower to maintain it
    float activate_threshold = energy_threshold_;
    float deactivate_threshold = energy_threshold_ * 0.6f;

    bool energy_above = speech_active_
        ? (smoothed_energy_ > deactivate_threshold)
        : (smoothed_energy_ > activate_threshold);

    if (energy_above) {
        silence_counter_ms_ = 0;

        if (!speech_active_) {
            speech_counter_ms_ += chunk_duration_ms;
            if (speech_counter_ms_ >= speech_threshold_ms_) {
                speech_active_ = true;
            }
        }
    } else {
        speech_counter_ms_ = 0;

        if (speech_active_) {
            silence_counter_ms_ += chunk_duration_ms;
            if (silence_counter_ms_ >= silence_threshold_ms_) {
                speech_active_ = false;
            }
        }
    }

    return speech_active_;
}

bool VoiceActivityDetector::is_speech_active() const {
    return speech_active_;
}

int VoiceActivityDetector::silence_duration_ms() const {
    return silence_counter_ms_;
}

void VoiceActivityDetector::reset() {
    speech_active_ = false;
    silence_counter_ms_ = 0;
    speech_counter_ms_ = 0;
    smoothed_energy_ = 0.0f;
}

} // namespace vtt
