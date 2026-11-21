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
    std::vector<float> samples = std::vector<float>(max_samples);
    std::atomic<size_t> count{0};
    std::atomic<float> level{0};
    std::atomic<uint32_t> error{XSR_OK};
    std::atomic<bool> closed{false};
    std::atomic<bool> disconnected{false};
};

void record(Recording& target, const std::string& microphone, const fs::path& injection,
            const std::atomic<bool>& released, const std::atomic<bool>& cancelled,
            const std::function<void(float, bool)>& report);

} // namespace xsr
