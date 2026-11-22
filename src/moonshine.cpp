#include "moonshine.hpp"

#include <moonshine-c-api.h>
#include <cstdio>
#include <cstring>

#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <array>
#include <filesystem>
#endif

static_assert(MOONSHINE_HEADER_VERSION == 30000, "Use the Moonshine Voice v0.1.6 SDK");
namespace xsr {
#ifdef _WIN32
namespace {

constexpr size_t asset_count = 8;

struct MappedAsset {
    const uint8_t* data = nullptr;
    uint64_t size = 0;

    ~MappedAsset() { if (data) UnmapViewOfFile(data); }

    bool open(const std::filesystem::path& path) {
        const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER bytes{};
        const auto mapping = GetFileSizeEx(file, &bytes) && bytes.QuadPart > 0 ?
            CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
        CloseHandle(file);
        if (!mapping) return false;
        data = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        CloseHandle(mapping);
        size = uint64_t(bytes.QuadPart);
        return data != nullptr;
    }
};

} // namespace
#endif

struct MoonshineTranscriber {
    int32_t handle = -1;
#ifdef _WIN32
    std::array<MappedAsset, asset_count> assets;
#endif

    ~MoonshineTranscriber() { if (handle >= 0) moonshine_free_transcriber(handle); }
};

MoonshineTranscriber* xsr_moonshine_load(const MoonshinePathChar* path, bool medium) {
    const auto architecture = medium ? MOONSHINE_MODEL_ARCH_MEDIUM_STREAMING : MOONSHINE_MODEL_ARCH_SMALL_STREAMING;
    auto transcriber = std::make_unique<MoonshineTranscriber>();
#ifdef _WIN32
    // The SDK's path-based loader uses narrow filesystem paths for some assets.
    std::array<const char*, asset_count> names{"frontend.model.ort", "frontend.weights.ort", "encoder.ort",
        "adapter.ort", "cross_kv.ort", "decoder_kv.ort", "streaming_config.json", "tokenizer.bin"};
    std::array<const uint8_t*, asset_count> data{};
    std::array<uint64_t, asset_count> sizes{};
    const std::filesystem::path directory(path);
    for (size_t i = 0; i < names.size(); ++i) {
        auto& asset = transcriber->assets[i];
        if (!asset.open(directory / names[i])) return nullptr;
        data[i] = asset.data;
        sizes[i] = asset.size;
    }
    transcriber->handle = moonshine_load_transcriber_from_memory_files(
        names.data(), data.data(), sizes.data(), names.size(), architecture, nullptr, 0, MOONSHINE_HEADER_VERSION);
#else
    transcriber->handle = moonshine_load_transcriber_from_files(path, architecture, nullptr, 0, MOONSHINE_HEADER_VERSION);
#endif
    if (transcriber->handle < 0) {
        std::fprintf(stderr, "Moonshine: %s\n", moonshine_error_to_string(transcriber->handle));
        return nullptr;
    }
    return transcriber.release();
}

void xsr_moonshine_free(MoonshineTranscriber* transcriber) { delete transcriber; }

bool xsr_moonshine_terms(MoonshineTranscriber* transcriber, const char* terms) {
    return moonshine_transcriber_set_keyterms(transcriber->handle, terms) == MOONSHINE_ERROR_NONE;
}

MoonshineResult xsr_moonshine_decode(MoonshineTranscriber* transcriber, float* samples, uint64_t count,
                                   uint32_t rate, char* output, uint64_t capacity) {
    transcript_t* transcript = nullptr;
    if (moonshine_transcribe_without_streaming(transcriber->handle, samples, count, rate, 0, &transcript) != MOONSHINE_ERROR_NONE ||
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
