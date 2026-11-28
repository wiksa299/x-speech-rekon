#pragma once

#include <atomic>
#include <optional>
#include <string>

namespace xsl {

struct FileSelection {
    std::string path;
    bool failed = false;
};

FileSelection browse_path(bool folder, const std::atomic<bool>& stopping);
bool copy_clipboard_text(const std::string& text);
std::optional<std::string> paste_clipboard_text();

} // namespace xsl
