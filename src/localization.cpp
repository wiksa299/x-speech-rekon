#include "localization.hpp"
#include "english_catalog.hpp"
#include "xspeech_rekon.h"

#include <XPLMPlugin.h>
#include <XPLMUtilities.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace xsr {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

Json read_object(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    auto data = Json::parse(input, nullptr, false);
    return data.is_object() ? std::move(data) : Json{};
}

bool usable_text(const Json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text.find('\0') == text.npos && text.find("##") == text.npos;
}

// Catalog format strings currently take unsigned integers, in order.
int format_arguments(const std::string& text) {
    int count = 0;
    for (size_t pos = 0; (pos = text.find('%', pos)) != text.npos; ++pos) {
        if (++pos == text.size()) return -1;
        if (text[pos] == 'u') ++count;
        else if (text[pos] != '%') return -1;
    }
    return count;
}

const char* simulator_language() {
    switch (XPLMGetLanguage()) {
    case xplm_Language_French: return "fr";
    case xplm_Language_Spanish: return "es";
    case xplm_Language_German: return "de";
    case xplm_Language_Italian: return "it";
    case xplm_Language_Korean: return "ko";
    case xplm_Language_Russian: return "ru";
    case xplm_Language_Greek: return "el";
    case xplm_Language_Japanese: return "ja";
    case xplm_Language_Chinese: return "zh";
    case xplm_Language_Ukrainian: return "uk";
    default: return "en";
    }
}
}

void InterfaceLocale::initialize(fs::path directory, const std::string& preference) {
    directory_ = std::move(directory);
    languages_ = {{"en", "English"}};
    auto registry = read_object(directory_ / "catalog.json");
    for (const auto& [id, name] : registry.items()) {
        if (id.empty() || id.size() > 32 || id == "auto" ||
            id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != id.npos ||
            !usable_text(name)) continue;
        languages_[id] = name.get<std::string>();
    }
    active_.clear();
    select(preference);
}

bool InterfaceLocale::select(const std::string& preference) {
    preference_ = languages_.count(preference) ? preference : "auto";
    return refresh();
}

bool InterfaceLocale::refresh() {
    const char* requested = preference_ == "auto" ? simulator_language() : preference_.c_str();
    const char* code = languages_.count(requested) ? requested : "en";
    if (active_ == code) return false;
    static const auto english = Json::parse(built_in_english_catalog);
    auto values = english;
    const auto overlay = [&](const char* language) {
        const auto data = read_object(directory_ / (std::string(language) + ".json"));
        for (const auto& [key, candidate] : data.items()) {
            auto base = english.find(key);
            if (base != english.end() && usable_text(candidate) &&
                format_arguments(candidate.get_ref<const std::string&>()) ==
                format_arguments(base->get_ref<const std::string&>()))
                values[key] = candidate;
        }
    };
    overlay("en");
    if (std::strcmp(code, "en") != 0) overlay(code);
    for (auto& [key, value] : values.items()) {
        auto& entry = entries_[key];
        entry.text = value.get<std::string>();
        entry.label = entry.text + "###" + key;
    }
    active_ = code;
    return true;
}

const char* InterfaceLocale::lookup(const char* key, bool widget) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return key;
    return (widget ? it->second.label : it->second.text).c_str();
}

InterfaceLocale& interface_locale() { static InterfaceLocale locale; return locale; }
const char* tr(const char* key) { return interface_locale().lookup(key, false); }
const char* ui_label(const char* key) { return interface_locale().lookup(key, true); }

fs::path preferences_path() {
    char path[1024]{};
    XPLMGetPrefsPath(path);
    return fs::u8path(path).parent_path() / "x-speech-rekon.json";
}

fs::path interface_directory(bool diagnostic) {
    auto id = diagnostic ? XPLMFindPluginBySignature(XSR_PROVIDER_SIGNATURE) : XPLMGetMyID();
    char path[1024]{};
    XPLMGetPluginInfo(id == XPLM_NO_PLUGIN_ID ? XPLMGetMyID() : id, nullptr, path, nullptr, nullptr);
    auto root = fs::u8path(path).parent_path().parent_path();
    if (id == XPLM_NO_PLUGIN_ID) root = root.parent_path() / "X-Speech-Rekon";
    return root / "locales";
}

void initialize_interface(bool diagnostic) {
    auto prefs = read_object(preferences_path());
    auto it = prefs.find("interface_language");
    interface_locale().initialize(interface_directory(diagnostic),
        it != prefs.end() && it->is_string() ? it->get<std::string>() : "auto");
}

void copy_plugin_description(char* destination, const char* key) {
    const char* value = tr(key);
    size_t size = std::min<size_t>(255, std::strlen(value));
    while (size && (static_cast<unsigned char>(value[size]) & 0xc0) == 0x80) --size;
    std::memcpy(destination, value, size);
    destination[size] = '\0';
}
} // namespace xsr
