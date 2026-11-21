#pragma once

#include <atomic>
#include <string>

namespace xsr {

struct FileSelection {
    std::string path;
    bool failed = false;
};

FileSelection browse_path(bool folder, const std::atomic<bool>& stopping);
bool copy_diagnostic_text(const std::string& text);

} // namespace xsr
