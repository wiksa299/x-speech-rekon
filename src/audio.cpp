#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace xsr {
namespace {

class AudioContext {
public:
    AudioContext() {
#ifdef _WIN32
        ma_backend backends[] = {ma_backend_wasapi};
#else
        ma_backend backends[] = {ma_backend_pulseaudio, ma_backend_alsa};
#endif
        ready = ma_context_init(backends, sizeof(backends) / sizeof(*backends), nullptr, &value) == MA_SUCCESS;
    }
    ~AudioContext() { if (ready) ma_context_uninit(&value); }
    ma_context value{};
    bool ready = false;
};

std::string microphone_id(const ma_device_info& device) {
    const auto* data = reinterpret_cast<const unsigned char*>(&device.id);
    static constexpr char hex[] = "0123456789abcdef";
    std::string id;
    for (size_t i = 0; i < sizeof(ma_device_id); ++i) {
        id.push_back(hex[data[i] >> 4]);
        id.push_back(hex[data[i] & 15]);
    }
    return id;
}

struct CaptureState {
    Recording& target;
    const std::atomic<bool>& released;
    const std::atomic<bool>& cancelled;
    float monitor_gain;

    void append(const float* samples, size_t frames) {
        if (!samples || released || cancelled) return;
        const auto offset = target.count.load(std::memory_order_relaxed);
        const auto available = std::min(frames, target.samples.size() - offset);
        if (!available) return;
        std::memcpy(target.samples.data() + offset, samples, available * sizeof(float));
        float peak = 0;
        for (size_t i = 0; i < available; ++i) peak = std::max(peak, std::abs(samples[i]));
        target.level = std::min(1.0f, peak);
        target.count.store(offset + available, std::memory_order_release);
    }
};

void capture_callback(ma_device* device, void* output, const void* samples, ma_uint32 count) {
    auto& state = *static_cast<CaptureState*>(device->pUserData);
    const auto* input = static_cast<const float*>(samples);
    state.append(input, count);
    if (!output) return;
    auto* playback = static_cast<float*>(output);
    if (!input || state.released || state.cancelled) {
        std::fill_n(playback, count, 0.0f);
        return;
    }
    for (ma_uint32 i = 0; i < count; ++i)
        playback[i] = std::clamp(input[i], -1.0f, 1.0f) * state.monitor_gain;
}

void notify_callback(const ma_device_notification* notification) {
    if (notification->type == ma_device_notification_type_stopped) {
        auto& state = *static_cast<CaptureState*>(notification->pDevice->pUserData);
        if (!state.released && !state.cancelled) state.target.disconnected = true;
    }
}

} // namespace

Json microphone_list() {
    Json result = Json::array();
    AudioContext context;
    ma_device_info* microphones = nullptr;
    ma_uint32 count = 0;
    if (context.ready && ma_context_get_devices(&context.value, nullptr, nullptr,
                                                 &microphones, &count) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count && i < 40; ++i) {
            result.push_back({{"id", microphone_id(microphones[i])}, {"name", microphones[i].name}});
        }
    }
    return result;
}

std::vector<float> read_wav(const fs::path& path) {
    ma_decoder decoder{};
    auto config = ma_decoder_config_init(ma_format_f32, 1, sample_rate);
#ifdef _WIN32
    const auto opened = ma_decoder_init_file_w(path.c_str(), &config, &decoder);
#else
    const auto opened = ma_decoder_init_file(path.c_str(), &config, &decoder);
#endif
    if (opened != MA_SUCCESS) return {};
    std::vector<float> result(max_samples + 1);
    ma_uint64 count = 0;
    const auto status = ma_decoder_read_pcm_frames(&decoder, result.data(), result.size(), &count);
    ma_decoder_uninit(&decoder);
    if ((status != MA_SUCCESS && status != MA_AT_END) || count > max_samples) return {};
    result.resize(size_t(count));
    return result;
}

bool contains_audio(const float* samples, size_t count) {
    constexpr size_t frame_size = 320;
    constexpr double minimum_energy = 0.00001;
    unsigned audible = 0;
    for (size_t offset = 0; offset < count; offset += frame_size) {
        const auto end = std::min(count, offset + frame_size);
        double energy = 0.0;
        for (size_t i = offset; i < end; ++i) energy += double(samples[i]) * samples[i];
        if (energy / (end - offset) > minimum_energy && ++audible == 3) return true;
    }
    return false;
}

void record(Recording& target, const std::string& microphone, const fs::path& injection, float monitor_gain,
            const std::atomic<bool>& released, const std::atomic<bool>& cancelled,
            const std::function<void(float, bool)>& report) {
    struct CloseFlag { Recording& value; ~CloseFlag() { value.closed = true; } } close{target};
    CaptureState state{target, released, cancelled, monitor_gain};
    if (released || cancelled) return;
    if (!injection.empty()) {
        const auto input = read_wav(injection);
        if (input.empty()) { target.error = XSR_BAD_REQUEST; return; }
        state.append(input.data(), input.size());
        if (target.count) report(target.level, true);
        while (!released && !cancelled) std::this_thread::sleep_for(5ms);
        return;
    }
    AudioContext context;
    if (!context.ready) { target.error = XSR_MIC_UNAVAILABLE; return; }
    ma_device_info* devices = nullptr;
    ma_uint32 count = 0;
    ma_device_id* chosen = nullptr;
    if (!microphone.empty()) {
        if (ma_context_get_devices(&context.value, nullptr, nullptr, &devices, &count) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count; ++i) {
                if (microphone_id(devices[i]) == microphone) { chosen = &devices[i].id; break; }
            }
        }
        if (!chosen) { target.error = XSR_MIC_UNAVAILABLE; return; }
    }

    ma_device device{};
    const auto start_device = [&](ma_device_type type) {
        auto config = ma_device_config_init(type);
        config.capture.pDeviceID = chosen;
        config.capture.format = ma_format_f32;
        config.capture.channels = 1;
        config.playback.format = ma_format_f32;
        config.playback.channels = 1;
        config.sampleRate = sample_rate;
        config.periodSizeInMilliseconds = 10;
        config.dataCallback = capture_callback;
        config.notificationCallback = notify_callback;
        config.pUserData = &state;
        if (ma_device_init(&context.value, &config, &device) != MA_SUCCESS) return false;
        if (!released && !cancelled && ma_device_start(&device) == MA_SUCCESS) return true;
        ma_device_uninit(&device);
        return false;
    };
    bool recording = monitor_gain > 0 && start_device(ma_device_type_duplex);
    if (!recording && !released && !cancelled) {
        target.disconnected = false;
        recording = start_device(ma_device_type_capture);
    }
    if (!recording) {
        if (!released && !cancelled) target.error = XSR_MIC_UNAVAILABLE;
        return;
    }

    bool started = false;
    auto next_level = Clock::now();
    while (!released && !cancelled && !target.disconnected) {
        const auto now = Clock::now();
        if (target.count && (!started || now >= next_level)) {
            report(target.level, !started);
            started = true;
            next_level = now + 100ms;
        }
        std::this_thread::sleep_for(5ms);
    }
    ma_device_uninit(&device);
    if (target.disconnected) target.error = XSR_MIC_UNAVAILABLE;
}

} // namespace xsr
