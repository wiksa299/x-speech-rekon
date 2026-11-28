#pragma once

#include "core.hpp"

#include <atomic>

namespace xsl {

// Keeps one verified adaptation while the corresponding model is selected.
class FluidWeights {
public:
    FluidWeights() = default;
    ~FluidWeights() { reset(); }
    FluidWeights(const FluidWeights&) = delete;
    FluidWeights& operator=(const FluidWeights&) = delete;

    bool prepare(const std::string& id, const fs::path& original, std::string_view digest,
                 const std::atomic<bool>& stop);
    const fs::path& path() const { return path_; }
    void reset();

private:
    fs::path folder_, path_;
};

} // namespace xsl
