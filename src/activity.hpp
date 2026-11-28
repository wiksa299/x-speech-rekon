#pragma once

#include "core.hpp"

namespace xsl {

enum class ActivityPhase { awaiting_audio, capturing, released_before_audio, transcribing };

struct ActivityProgress {
    Clock::time_point requested_at{};
    std::optional<Clock::time_point> processing_since;
    std::optional<uint32_t> capture_delay_ms;
    bool microphone = false;

    ActivityPhase phase() const {
        if (!microphone) return ActivityPhase::transcribing;
        if (processing_since)
            return capture_delay_ms ? ActivityPhase::transcribing : ActivityPhase::released_before_audio;
        return capture_delay_ms ? ActivityPhase::capturing : ActivityPhase::awaiting_audio;
    }

    void confirm_audio(Clock::time_point now) {
        if (phase() == ActivityPhase::awaiting_audio)
            capture_delay_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - requested_at).count());
    }
};

} // namespace xsl
