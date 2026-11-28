#pragma once

#include "core.hpp"
#include <atomic>

namespace xsl {


Json scan_models(const fs::path& folder, const Json& catalog, const std::atomic<bool>& stop, bool require_directory);
Json identify_model(const fs::path& path, const Json& catalog, const std::atomic<bool>& stop);
bool matches_digest(const fs::path& path, const std::string& expected, const std::atomic<bool>& stop);
std::string download_model(const fs::path& folder, const Json& entry,
                           const std::atomic<bool>& stop, const std::function<void(uint64_t)>& progress);

} // namespace xsl
