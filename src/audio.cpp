#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace xsl {
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
    std::atomic<float> interval_peak{0};
    ma_resampler resampler{};
    bool resampling = false;
    std::array<float, 1024> resampled;

    ~CaptureState() { if (resampling) ma_resampler_uninit(&resampler, nullptr); }

    bool set_rate(ma_uint32 rate) {
        if (resampling) ma_resampler_uninit(&resampler, nullptr);
        resampling = false;
        if (rate == sample_rate) return true;
        const auto config = ma_resampler_config_init(ma_format_f32, 1, rate, sample_rate, ma_resample_algorithm_linear);
        resampling = ma_resampler_init(&config, nullptr, &resampler) == MA_SUCCESS;
        return resampling;
    }

    void append(const float* samples, size_t frames) {
        if (!samples || released || cancelled) return;
        const auto offset = target.count.load(std::memory_order_relaxed);
        const auto available = std::min(frames, target.samples.size() - offset);
        if (!available) return;
        std::memcpy(target.samples.data() + offset, samples, available * sizeof(float));
        float peak = 0;
        for (size_t i = 0; i < available; ++i) peak = std::max(peak, std::abs(samples[i]));
        target.level = std::min(1.0f, peak);
        if (target.microphone_test) {
            auto last = interval_peak.load(std::memory_order_relaxed);
            while (last < peak && !interval_peak.compare_exchange_weak(last, peak, std::memory_order_relaxed)) {}
        }
        target.count.store(offset + available, std::memory_order_release);
    }

    void collect(const float* input, ma_uint64 frames) {
        if (!resampling) {
            append(input, static_cast<size_t>(frames));
            return;
        }
        if (!input || released || cancelled) return;
        while (frames && target.count.load(std::memory_order_relaxed) < target.samples.size() && !released && !cancelled) {
            ma_uint64 consumed = frames, produced = resampled.size();
            if (ma_resampler_process_pcm_frames(&resampler, input, &consumed, resampled.data(), &produced) != MA_SUCCESS ||
                (!consumed && !produced)) {
                target.disconnected = true;
                return;
            }
            append(resampled.data(), static_cast<size_t>(produced));
            input += consumed;
            frames -= consumed;
        }
    }
};

void capture_callback(ma_device* device, void* output, const void* samples, ma_uint32 count) {
    auto& state = *static_cast<CaptureState*>(device->pUserData);
    const auto* input = static_cast<const float*>(samples);
    state.collect(input, count);
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

struct TakePlayback {
    const std::vector<float>& samples;
    uint64_t session;
    const std::atomic<uint64_t>& playing;
    size_t offset = 0;
    std::atomic<bool> done{false};
    std::atomic<bool> stopped{false};
};

void play_take_callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    auto& state = *static_cast<TakePlayback*>(device->pUserData);
    auto* pcm = static_cast<float*>(output);
    std::fill_n(pcm, frames, 0.0f);
    if (state.playing != state.session) return;
    const auto count = std::min(size_t(frames), state.samples.size() - state.offset);
    for (size_t i = 0; i < count; ++i)
        pcm[i] = std::clamp(state.samples[state.offset + i], -1.0f, 1.0f) * 0.5f;
    state.offset += count;
    if (state.offset == state.samples.size()) state.done = true;
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
            result.push_back({{"id", microphone_id(microphones[i])}, {"name", microphones[i].name},
                              {"default", microphones[i].isDefault != 0}});
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

Json microphone_measurement(const float* samples, size_t count) {
    constexpr size_t window = sample_rate / 50;
    constexpr size_t quiet_windows = 2 * sample_rate / window;
    const auto dbfs = [](double amplitude) { return 20 * std::log10(std::max(amplitude, 0.000001)); };
    std::vector<double> quiet;
    quiet.reserve(quiet_windows);
    float peak = 0;
    unsigned clipped = 0, voice_windows = 0;
    size_t voice_samples = 0;
    double voice_energy = 0, voice_gate = 0.000001;
    for (size_t offset = 0; offset < count; offset += window) {
        const auto size = std::min(window, count - offset);
        float window_peak = 0;
        double energy = 0;
        for (size_t i = 0; i < size; ++i) {
            const float sample = samples[offset + i];
            if (!std::isfinite(sample)) continue;
            window_peak = std::max(window_peak, std::abs(sample));
            energy += double(sample) * sample;
        }
        peak = std::max(peak, window_peak);
        if (window_peak >= 0.999f) ++clipped;
        const double mean_energy = energy / size;
        if (quiet.size() < quiet_windows) {
            quiet.push_back(mean_energy);
            if (quiet.size() == quiet_windows) {
                auto median = quiet.begin() + quiet.size() / 2;
                std::nth_element(quiet.begin(), median, quiet.end());
                voice_gate = std::max(voice_gate, *median * 10);
            }
        } else if (mean_energy >= voice_gate) {
            ++voice_windows;
            voice_samples += size;
            voice_energy += energy;
        }
    }
    const bool has_voice = voice_windows >= 5;
    return {{"peak_dbfs", dbfs(std::min(peak, 1.0f))}, {"clipped", clipped},
            {"rms_dbfs", has_voice ? Json(dbfs(std::sqrt(voice_energy / voice_samples))) : Json(nullptr)},
            {"capture_ms", count * 1000 / sample_rate}};
}

bool play_microphone_take(const std::vector<float>& samples, uint64_t session, const std::atomic<uint64_t>& playing) {
    if (playing != session) return true;
    AudioContext context;
    if (!context.ready) return false;
    TakePlayback state{samples, session, playing};
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 1;
    config.sampleRate = sample_rate;
    config.periodSizeInMilliseconds = 10;
    config.dataCallback = play_take_callback;
    config.notificationCallback = [](const ma_device_notification* event) {
        if (event->type == ma_device_notification_type_stopped)
            static_cast<TakePlayback*>(event->pDevice->pUserData)->stopped = true;
    };
    config.pUserData = &state;
    ma_device device{};
    if (ma_device_init(&context.value, &config, &device) != MA_SUCCESS) return false;
    const bool started = playing == session && ma_device_start(&device) == MA_SUCCESS;
    if (started) {
        const auto deadline = Clock::now() + std::chrono::seconds(microphone_test_seconds + 2);
        while (!state.done && !state.stopped && playing == session && Clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        if (state.done)
            for (int i = 0; i < 20 && !state.stopped && playing == session; ++i) std::this_thread::sleep_for(5ms);
    }
    const bool ok = playing != session || (started && state.done && !state.stopped);
    ma_device_uninit(&device);
    return ok;
}

void record(Recording& target, const std::string& microphone, const fs::path& injection, float monitor_gain,
            bool low_latency_monitoring,
            const std::atomic<bool>& released, const std::atomic<bool>& cancelled,
            const std::function<void(float, bool, size_t, const std::string&, const CaptureMonitoring&)>& report) {
    struct CloseFlag { Recording& value; ~CloseFlag() { value.closed = true; } } close{target};
    CaptureState state{target, released, cancelled, monitor_gain};
    if (released || cancelled) return;
    if (!injection.empty()) {
        const auto input = read_wav(injection);
        if (input.empty()) { target.error = XSL_BAD_REQUEST; return; }
        state.append(input.data(), input.size());
        if (target.count) report(target.level, true, target.count, {}, {});
        if (target.microphone_test) return;
        while (!released && !cancelled) std::this_thread::sleep_for(5ms);
        return;
    }
    AudioContext context;
    if (!context.ready) { target.error = XSL_MIC_UNAVAILABLE; return; }
    ma_device_info* devices = nullptr;
    ma_uint32 count = 0;
    std::optional<ma_device_id> chosen;
    if (!microphone.empty()) {
        if (ma_context_get_devices(&context.value, nullptr, nullptr, &devices, &count) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count; ++i) {
                if (microphone_id(devices[i]) == microphone) { chosen = devices[i].id; break; }
            }
        }
        if (!chosen) { target.error = XSL_MIC_UNAVAILABLE; return; }
    }

    ma_device device{};
    const auto start_device = [&](ma_device_type type, ma_uint32 rate, bool low_latency) {
        target.disconnected = false;
        auto config = ma_device_config_init(type);
        config.capture.pDeviceID = chosen ? &*chosen : nullptr;
        config.capture.format = ma_format_f32;
        config.capture.channels = 1;
        config.playback.format = ma_format_f32;
        config.playback.channels = 1;
        config.sampleRate = rate;
        config.periodSizeInMilliseconds = low_latency ? 5 : 10;
        if (low_latency) {
            config.periods = 2;
            config.performanceProfile = ma_performance_profile_low_latency;
            config.noFixedSizedCallback = MA_TRUE;
            config.wasapi.usage = ma_wasapi_usage_pro_audio;
        }
        config.dataCallback = capture_callback;
        config.notificationCallback = notify_callback;
        config.pUserData = &state;
        if (ma_device_init(&context.value, &config, &device) != MA_SUCCESS) return false;
        if (!released && !cancelled && state.set_rate(device.sampleRate) && ma_device_start(&device) == MA_SUCCESS) return true;
        ma_device_uninit(&device);
        return false;
    };
    bool low_latency_active = false;
    if (monitor_gain > 0 && low_latency_monitoring) {
        ma_device_info output{};
        if (ma_context_get_device_info(&context.value, ma_device_type_playback, nullptr, &output) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < output.nativeDataFormatCount; ++i) {
                const auto& format = output.nativeDataFormats[i];
                if (format.sampleRate && !(format.flags & MA_DATA_FORMAT_FLAG_EXCLUSIVE_MODE)) {
                    low_latency_active = start_device(ma_device_type_duplex, format.sampleRate, true);
                    break;
                }
            }
        }
    }
    const bool duplex = low_latency_active || (monitor_gain > 0 && !released && !cancelled &&
        start_device(ma_device_type_duplex, sample_rate, false));
    bool recording = duplex;
    if (!recording && !released && !cancelled) {
        recording = start_device(ma_device_type_capture, sample_rate, false);
    }
    if (!recording) {
        if (!released && !cancelled) target.error = XSL_MIC_UNAVAILABLE;
        return;
    }

    CaptureMonitoring monitoring;
    if (monitor_gain > 0) {
        monitoring = {low_latency_active ? "low_latency" : duplex ? "standard" : "capture_only",
                      low_latency_monitoring ? !low_latency_active : !duplex,
                      device.sampleRate, device.playback.internalSampleRate, device.playback.internalPeriodSizeInFrames};
    }
    bool started = false;
    const std::string name = target.microphone_test ? device.capture.name : "";
    auto next_level = Clock::now();
    while (!released && !cancelled && !target.disconnected) {
        const auto now = Clock::now();
        const auto count = target.count.load(std::memory_order_acquire);
        if (count && (!started || now >= next_level)) {
            const auto level = target.microphone_test ? state.interval_peak.exchange(0, std::memory_order_relaxed) :
                target.level.load(std::memory_order_relaxed);
            report(level, !started, count, name, monitoring);
            started = true;
            next_level = now + 100ms;
        }
        if (target.microphone_test && count == target.samples.size()) break;
        std::this_thread::sleep_for(5ms);
    }
    const bool disconnected = target.disconnected;
    ma_device_uninit(&device);
    if (disconnected) target.error = XSL_MIC_UNAVAILABLE;
}

} // namespace xsl
