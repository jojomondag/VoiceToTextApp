#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio_capture.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

namespace vtt {

struct AudioCapture::Impl {
    ma_context context;
    ma_device device;
    bool context_initialized = false;
    bool device_initialized = false;
    std::atomic<bool> recording{false};

    std::vector<float> buffer;
    std::mutex buffer_mutex;
    std::vector<float> resample_pending;
    // Native sample position in 1/16000ths; integer phase avoids drift at
    // 44.1 kHz and makes output independent of live drain boundaries.
    std::uint64_t resample_position = 0;

    std::atomic<float> audio_level{0.0f};
    ma_uint32 capture_sample_rate = 16000;

    std::string selected_device_id;
    std::string last_error;
    std::string backend_log;

    static void on_audio_log(void* user_data, ma_uint32 level,
                             const char* message) {
        (void)level;
        auto* self = static_cast<Impl*>(user_data);
        if (self && message) self->backend_log = message;
    }

    void set_error(const char* stage, ma_result result) {
        last_error = stage;
        last_error += ": ";
        last_error += ma_result_description(result);
        last_error += " (";
        last_error += std::to_string(static_cast<int>(result));
        last_error += ")";
    }

    bool initialize_device() {
        if (device_initialized) return true;

        ma_device_config config = ma_device_config_init(ma_device_type_capture);
        config.capture.format = ma_format_f32;
        config.capture.channels = 1;
        // Let WASAPI use the endpoint's native shared-mode rate.  A number
        // of USB interfaces (including Scarlett devices) reject a 16 kHz
        // client stream during IAudioClient::Initialize even though the
        // device can be converted perfectly well in software.
        config.sampleRate = 0;
        config.dataCallback = Impl::data_callback;
        config.pUserData = this;

        // Enumerate on every lazy initialization.  A context can initialize
        // successfully even when Windows has no capture endpoint, in which
        // case ma_device_init() often reports the rather opaque
        // MA_INVALID_ARGS.  Turn that into an actionable error before trying
        // to open a device.
        ma_device_id* target_device_id = nullptr;
        ma_device_info* capture_infos = nullptr;
        ma_uint32 capture_count = 0;
        ma_result enumerate_result = ma_context_get_devices(
            &context, nullptr, nullptr, &capture_infos, &capture_count);
        if (enumerate_result != MA_SUCCESS) {
            set_error("enumerating capture devices", enumerate_result);
            return false;
        }
        if (capture_count == 0) {
            last_error = "no microphone input device is available";
            return false;
        }

        if (!selected_device_id.empty()) {
            try {
                int idx = std::stoi(selected_device_id);
                if (idx >= 0 && static_cast<ma_uint32>(idx) < capture_count) {
                    target_device_id = &capture_infos[idx].id;
                }
            } catch (...) {
                target_device_id = nullptr;
            }
        }

        if (target_device_id) {
            config.capture.pDeviceID = target_device_id;
        }

        // Shared mode is the least surprising Windows path and avoids a
        // stale/exclusive device selection taking the entire recorder down.
        config.capture.shareMode = ma_share_mode_shared;
        auto try_init = [&](ma_uint32 channels, ma_uint32 sample_rate,
                            ma_device_id* device_id) {
            ma_device_config attempt = config;
            attempt.capture.channels = channels;
            attempt.sampleRate = sample_rate;
            attempt.capture.pDeviceID = device_id;
            return ma_device_init(&context, &attempt, &device);
        };

        // USB interfaces such as Scarlett 2i2 expose a stereo capture
        // endpoint.  Requesting a mono WASAPI stream can succeed but select
        // an empty/non-speaking channel.  Open stereo first and down-mix in
        // the callback; fall back to mono for ordinary laptop microphones.
        ma_result result = try_init(2, 0, target_device_id);
        if (result != MA_SUCCESS && target_device_id != nullptr) {
            // A saved device id may refer to a disconnected USB/Bluetooth
            // microphone.  Retry once against the current Windows default.
            result = try_init(2, 0, nullptr);
        }
        if (result != MA_SUCCESS) {
            // Ordinary mono endpoints may reject a two-channel request.
            result = try_init(1, 0, target_device_id);
            if (result != MA_SUCCESS && target_device_id != nullptr) {
                result = try_init(1, 0, nullptr);
            }
        }
        if (result != MA_SUCCESS) {
            // As a final fallback let miniaudio create a fresh context on the
            // calling thread.  This handles Windows COM/backend state left
            // behind by another audio client in the long-lived app context.
            ma_device_config fallback = config;
            fallback.sampleRate = 0;
            fallback.capture.channels = 2;
            fallback.capture.pDeviceID = nullptr;
            result = ma_device_init(nullptr, &fallback, &device);
            if (result != MA_SUCCESS) {
                fallback.capture.channels = 1;
                result = ma_device_init(nullptr, &fallback, &device);
            }
        }
        if (result != MA_SUCCESS) {
            set_error("initializing capture device", result);
            last_error += "; available inputs=" + std::to_string(capture_count);
            if (capture_infos != nullptr && capture_count > 0) {
                last_error += " [";
                for (ma_uint32 i = 0; i < capture_count; ++i) {
                    if (i != 0) last_error += ", ";
                    last_error += capture_infos[i].name;
                }
                last_error += "]";
            }
            if (!backend_log.empty()) {
                last_error += "; backend=";
                last_error += backend_log;
            }
            return false;
        }
        device_initialized = true;
        capture_sample_rate = device.sampleRate != 0 ? device.sampleRate : 16000;
        last_error.clear();
        return true;
    }

    static void data_callback(ma_device* device, void* output, const void* input,
                              ma_uint32 frame_count) {
        (void)output;
        auto* self = static_cast<Impl*>(device->pUserData);
        if (!self->recording.load()) return;

        const float* samples = static_cast<const float*>(input);
        if (!samples) return;

        const ma_uint32 channels = (std::max)(1u, device->capture.channels);

        // Calculate RMS level
        float sum_sq = 0.0f;
        for (ma_uint32 i = 0; i < frame_count; ++i) {
            float mono = 0.0f;
            for (ma_uint32 channel = 0; channel < channels; ++channel) {
                mono += samples[i * channels + channel];
            }
            mono /= static_cast<float>(channels);
            sum_sq += mono * mono;
        }
        float rms = std::sqrt(sum_sq / static_cast<float>(frame_count));
        // Smooth the level with exponential moving average
        float prev = self->audio_level.load();
        float alpha = 0.3f;
        self->audio_level.store(prev + alpha * (rms - prev));

        // Accumulate into buffer
        {
            std::lock_guard<std::mutex> lock(self->buffer_mutex);
            for (ma_uint32 i = 0; i < frame_count; ++i) {
                float mono = 0.0f;
                for (ma_uint32 channel = 0; channel < channels; ++channel) {
                    mono += samples[i * channels + channel];
                }
                self->buffer.push_back(mono / static_cast<float>(channels));
            }
        }

    }
};

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}

AudioCapture::~AudioCapture() {
    shutdown();
}

bool AudioCapture::initialize() {
    if (impl_->context_initialized) return true;

    ma_context_config ctx_config = ma_context_config_init();
    ma_result result = ma_context_init(nullptr, 0, &ctx_config, &impl_->context);
    if (result != MA_SUCCESS) {
        impl_->set_error("initializing audio context", result);
        return false;
    }
    impl_->context_initialized = true;
    ma_log_register_callback(
        ma_context_get_log(&impl_->context),
        ma_log_callback_init(Impl::on_audio_log, impl_.get()));
    impl_->last_error.clear();
    impl_->backend_log.clear();
    return true;
}

void AudioCapture::shutdown() {
    stop_recording();

    if (impl_->device_initialized) {
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;
    }

    if (impl_->context_initialized) {
        ma_context_uninit(&impl_->context);
        impl_->context_initialized = false;
    }
}

bool AudioCapture::select_device(const std::string& device_id) {
    if (impl_->recording.load()) return false;

    // Tear down existing device if any
    if (impl_->device_initialized) {
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;
    }

    impl_->selected_device_id = device_id;
    return true;
}

bool AudioCapture::prepare_device() {
    if (!impl_->context_initialized) {
        impl_->last_error = "audio context is not initialized";
        return false;
    }
    if (impl_->recording.load()) return true;
    return impl_->initialize_device();
}

bool AudioCapture::start_recording() {
    if (!impl_->context_initialized) {
        impl_->last_error = "audio context is not initialized";
        return false;
    }
    if (impl_->recording.load()) return true;

    if (!impl_->initialize_device()) {
        return false;
    }

    // Clear the buffer before starting
    {
        std::lock_guard<std::mutex> lock(impl_->buffer_mutex);
        impl_->buffer.clear();
        impl_->resample_pending.clear();
        impl_->resample_position = 0;
        impl_->buffer.reserve(static_cast<size_t>(impl_->capture_sample_rate) * 180);
    }
    impl_->audio_level.store(0.0f);

    ma_result result = ma_device_start(&impl_->device);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;

        // The default endpoint can disappear between startup and the first
        // hotkey (USB power management and Bluetooth routing both do this).
        // Re-enumerate once instead of surfacing a transient mic error.
        const bool reopened = impl_->initialize_device();
        if (reopened) result = ma_device_start(&impl_->device);
        if (!reopened) {
            return false;
        }
        if (result != MA_SUCCESS) {
            impl_->set_error("starting capture device", result);
            if (impl_->device_initialized) {
                ma_device_uninit(&impl_->device);
                impl_->device_initialized = false;
            }
            return false;
        }
    }

    impl_->recording.store(true);
    impl_->last_error.clear();
    return true;
}

void AudioCapture::stop_recording() {
    if (!impl_->recording.load()) return;

    impl_->recording.store(false);

    if (impl_->device_initialized) {
        ma_device_stop(&impl_->device);
    }
}

bool AudioCapture::is_recording() const {
    return impl_->recording.load();
}

std::string AudioCapture::last_error() const {
    return impl_->last_error;
}

std::vector<float> AudioCapture::get_audio_buffer(bool final) {
    std::lock_guard<std::mutex> lock(impl_->buffer_mutex);
    std::vector<float> data;
    data.swap(impl_->buffer);
    if (impl_->capture_sample_rate == 16000) return data;

    // Pianissimo consumes 16 kHz mono. Carry the interpolation position and
    // boundary sample across drains so preview cadence cannot drop samples.
    auto& pending = impl_->resample_pending;
    pending.insert(pending.end(), data.begin(), data.end());
    std::vector<float> resampled;
    resampled.reserve(pending.size() * 16000 / impl_->capture_sample_rate + 1);
    while (impl_->resample_position < static_cast<std::uint64_t>(pending.size()) * 16000) {
        const size_t left = static_cast<size_t>(impl_->resample_position / 16000);
        if (!final && left + 1 >= pending.size()) break;
        const size_t right = (std::min)(left + 1, pending.size() - 1);
        const float fraction = static_cast<float>(impl_->resample_position % 16000) / 16000.0f;
        resampled.push_back(pending[left] + (pending[right] - pending[left]) * fraction);
        impl_->resample_position += impl_->capture_sample_rate;
    }
    const size_t consumed = (std::min)(pending.size(),
        static_cast<size_t>(impl_->resample_position / 16000));
    pending.erase(pending.begin(), pending.begin() + consumed);
    impl_->resample_position -= static_cast<std::uint64_t>(consumed) * 16000;
    if (final) {
        pending.clear();
        impl_->resample_position = 0;
    }
    return resampled;
}

float AudioCapture::get_audio_level() const {
    float level = impl_->audio_level.load();
    // Clamp to 0.0 - 1.0
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    return level;
}

} // namespace vtt
