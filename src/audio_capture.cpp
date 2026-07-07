#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "audio_capture.h"
#include <cmath>
#include <cstring>

namespace vtt {

struct AudioCapture::Impl {
    ma_context context;
    ma_device device;
    bool context_initialized = false;
    bool device_initialized = false;
    std::atomic<bool> recording{false};

    std::vector<float> buffer;
    std::mutex buffer_mutex;

    AudioCallback callback;
    std::mutex callback_mutex;
    std::atomic<bool> has_callback{false};

    std::atomic<float> audio_level{0.0f};

    std::string selected_device_id;

    bool initialize_device() {
        if (device_initialized) return true;

        ma_device_config config = ma_device_config_init(ma_device_type_capture);
        config.capture.format = ma_format_f32;
        config.capture.channels = 1;
        config.sampleRate = 16000;
        config.dataCallback = Impl::data_callback;
        config.pUserData = this;

        // If a specific device was selected, look it up.
        ma_device_id* target_device_id = nullptr;
        ma_device_info* capture_infos = nullptr;
        ma_uint32 capture_count = 0;

        if (!selected_device_id.empty()) {
            ma_result res = ma_context_get_devices(&context, nullptr, nullptr,
                                                    &capture_infos, &capture_count);
            if (res == MA_SUCCESS) {
                try {
                    int idx = std::stoi(selected_device_id);
                    if (idx >= 0 && static_cast<ma_uint32>(idx) < capture_count) {
                        target_device_id = &capture_infos[idx].id;
                    }
                } catch (...) {
                    target_device_id = nullptr;
                }
            }
        }

        if (target_device_id) {
            config.capture.pDeviceID = target_device_id;
        }

        ma_result result = ma_device_init(&context, &config, &device);
        if (result != MA_SUCCESS) {
            return false;
        }
        device_initialized = true;
        return true;
    }

    static void data_callback(ma_device* device, void* output, const void* input,
                              ma_uint32 frame_count) {
        (void)output;
        auto* self = static_cast<Impl*>(device->pUserData);
        if (!self->recording.load()) return;

        const float* samples = static_cast<const float*>(input);
        if (!samples) return;

        // Calculate RMS level
        float sum_sq = 0.0f;
        for (ma_uint32 i = 0; i < frame_count; ++i) {
            sum_sq += samples[i] * samples[i];
        }
        float rms = std::sqrt(sum_sq / static_cast<float>(frame_count));
        // Smooth the level with exponential moving average
        float prev = self->audio_level.load();
        float alpha = 0.3f;
        self->audio_level.store(prev + alpha * (rms - prev));

        // Accumulate into buffer
        {
            std::lock_guard<std::mutex> lock(self->buffer_mutex);
            self->buffer.insert(self->buffer.end(), samples, samples + frame_count);
        }

        // Fire callback with the chunk
        if (self->has_callback.load()) {
            std::lock_guard<std::mutex> lock(self->callback_mutex);
            if (self->callback) {
                std::vector<float> chunk(samples, samples + frame_count);
                self->callback(chunk);
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
        return false;
    }
    impl_->context_initialized = true;
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

std::vector<AudioDevice> AudioCapture::get_devices() const {
    std::vector<AudioDevice> devices;
    if (!impl_->context_initialized) return devices;

    ma_device_info* capture_infos;
    ma_uint32 capture_count;
    ma_result result = ma_context_get_devices(&impl_->context, nullptr, nullptr,
                                               &capture_infos, &capture_count);
    if (result != MA_SUCCESS) return devices;

    for (ma_uint32 i = 0; i < capture_count; ++i) {
        AudioDevice dev;
        dev.name = capture_infos[i].name;
        // Use the name as the ID since miniaudio device IDs are opaque structs.
        // We store the index for lookup during select_device.
        dev.id = std::to_string(i);
        dev.is_default = capture_infos[i].isDefault != 0;
        devices.push_back(std::move(dev));
    }

    return devices;
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
    if (!impl_->context_initialized) return false;
    if (impl_->recording.load()) return true;
    return impl_->initialize_device();
}

bool AudioCapture::start_recording() {
    if (!impl_->context_initialized) return false;
    if (impl_->recording.load()) return true;

    if (!impl_->initialize_device()) {
        return false;
    }

    // Clear the buffer before starting
    {
        std::lock_guard<std::mutex> lock(impl_->buffer_mutex);
        impl_->buffer.clear();
        impl_->buffer.reserve(16000 * 180);
    }
    impl_->audio_level.store(0.0f);

    ma_result result = ma_device_start(&impl_->device);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;
        return false;
    }

    impl_->recording.store(true);
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

std::vector<float> AudioCapture::get_audio_buffer() {
    std::lock_guard<std::mutex> lock(impl_->buffer_mutex);
    std::vector<float> data;
    data.swap(impl_->buffer);
    return data;
}

void AudioCapture::set_audio_callback(AudioCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->callback_mutex);
    impl_->callback = std::move(callback);
    impl_->has_callback.store(static_cast<bool>(impl_->callback));
}

float AudioCapture::get_audio_level() const {
    float level = impl_->audio_level.load();
    // Clamp to 0.0 - 1.0
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    return level;
}

} // namespace vtt
