#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace xsl {

// Private message exchanged only by the two UI modules on the simulator thread.
inline constexpr int interface_language_message = 0x58534C31;
inline constexpr char diagnostic_signature[] = "wiksa.x-speech-listen.diagnostic";

class InterfaceLocale {
public:
    using Languages = std::map<std::string, std::string, std::less<>>;
    void initialize(std::filesystem::path directory, const std::string& preference);
    bool select(const std::string& preference);
    bool refresh();
    const auto& languages() const { return languages_; }
    const auto& preference() const { return preference_; }
    const char* lookup(const char* key, bool widget) const;
private:
    struct Entry { std::string text, label; };
    std::filesystem::path directory_;
    Languages languages_;
    std::string preference_, active_;
    std::map<std::string, Entry, std::less<>> entries_;
};

InterfaceLocale& interface_locale();
const char* tr(const char* key);
const char* ui_label(const char* key);
std::filesystem::path interface_directory(bool diagnostic);
std::filesystem::path preferences_path();
void initialize_interface(bool diagnostic);
void copy_plugin_description(char* destination, const char* key);

} // namespace xsl
