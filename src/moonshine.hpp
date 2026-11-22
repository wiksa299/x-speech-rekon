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

extern "C" {
XSR_MOONSHINE_API int32_t xsr_moonshine_load(const char* path, bool medium);
XSR_MOONSHINE_API void xsr_moonshine_free(int32_t handle);
XSR_MOONSHINE_API bool xsr_moonshine_terms(int32_t handle, const char* terms);
XSR_MOONSHINE_API MoonshineResult xsr_moonshine_decode(int32_t handle, float* samples,
    uint64_t count, uint32_t rate, char* output, uint64_t capacity);
}
}
