#pragma once

#include <cstdint>

#if defined(_WIN32) && defined(XSR_MOONSHINE_BRIDGE)
#define XSR_MOONSHINE_API __declspec(dllexport)
#elif defined(_WIN32)
#define XSR_MOONSHINE_API __declspec(dllimport)
#else
#define XSR_MOONSHINE_API
#endif

namespace xsr {
enum class MoonshineResult { ok, failed, truncated };
struct MoonshineTranscriber;
#ifdef _WIN32
using MoonshinePathChar = wchar_t;
#else
using MoonshinePathChar = char;
#endif

extern "C" {
XSR_MOONSHINE_API MoonshineTranscriber* xsr_moonshine_load(const MoonshinePathChar* path, bool medium);
XSR_MOONSHINE_API void xsr_moonshine_free(MoonshineTranscriber* transcriber);
XSR_MOONSHINE_API bool xsr_moonshine_terms(MoonshineTranscriber* transcriber, const char* terms);
XSR_MOONSHINE_API MoonshineResult xsr_moonshine_decode(MoonshineTranscriber* transcriber, float* samples,
    uint64_t count, uint32_t rate, char* output, uint64_t capacity);
}
}
