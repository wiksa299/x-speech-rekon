#include "moonshine.hpp"

#include <moonshine-c-api.h>
#include <cstdio>
#include <cstring>

static_assert(MOONSHINE_HEADER_VERSION == 30000, "Use the Moonshine Voice v0.1.6 SDK");
namespace xsr {
int32_t xsr_moonshine_load(const char* path, bool medium) {
    const auto architecture = medium ? MOONSHINE_MODEL_ARCH_MEDIUM_STREAMING : MOONSHINE_MODEL_ARCH_SMALL_STREAMING;
    const auto handle = moonshine_load_transcriber_from_files(path, architecture, nullptr, 0, MOONSHINE_HEADER_VERSION);
    if (handle < 0) std::fprintf(stderr, "Moonshine: %s\n", moonshine_error_to_string(handle));
    return handle;
}

void xsr_moonshine_free(int32_t handle) {
    moonshine_free_transcriber(handle);
}

bool xsr_moonshine_terms(int32_t handle, const char* terms) {
    return moonshine_transcriber_set_keyterms(handle, terms) == MOONSHINE_ERROR_NONE;
}

MoonshineResult xsr_moonshine_decode(int32_t handle, float* samples, uint64_t count,
                                   uint32_t rate, char* output, uint64_t capacity) {
    transcript_t* transcript = nullptr;
    if (moonshine_transcribe_without_streaming(handle, samples, count, rate, 0, &transcript) != MOONSHINE_ERROR_NONE ||
        !transcript) return MoonshineResult::failed;

    uint64_t written = 0;
    for (uint64_t i = 0; i < transcript->line_count; ++i) {
        const auto* line = transcript->lines[i].text;
        if (!line || !*line) continue;
        const auto length = std::strlen(line);
        if (length + (written ? 1u : 0u) >= capacity - written) return MoonshineResult::truncated;
        if (written) output[written++] = ' ';
        std::memcpy(output + written, line, length);
        written += length;
    }
    output[written] = '\0';
    return MoonshineResult::ok;
}
}
