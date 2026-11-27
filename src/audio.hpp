#pragma once

#include "core.hpp"
#include <array>
#include <atomic>
#include <thread>

namespace xsr {

Json microphone_list();
std::vector<float> read_wav(const fs::path& path);
bool contains_audio(const float* samples, size_t count);

struct Recording {
    explicit Recording(bool microphone_test = false)
        : samples(microphone_test ? microphone_test_seconds * sample_rate : max_samples), microphone_test(microphone_test) {}
    std::vector<float> samples;
    const bool microphone_test;
    std::atomic<size_t> count{0};
    std::atomic<float> level{0};
    std::atomic<uint32_t> error{XSR_OK};
    std::atomic<bool> closed{false};
    std::atomic<bool> disconnected{false};
};

Json microphone_measurement(const float* samples, size_t count);
bool play_microphone_take(const std::vector<float>& samples, uint64_t session, const std::atomic<uint64_t>& playing);

struct CaptureMonitoring {
    const char* mode = nullptr;
    bool fallback = false;
    uint32_t stream_rate = 0;
    uint32_t output_rate = 0;
    uint32_t output_period_frames = 0;
};

void record(Recording& target, const std::string& microphone, const fs::path& injection, float monitor_gain,
            bool low_latency_monitoring,
            const std::atomic<bool>& released, const std::atomic<bool>& cancelled,
            const std::function<void(float, bool, size_t, const std::string&, const CaptureMonitoring&)>& report);

} // namespace xsr
