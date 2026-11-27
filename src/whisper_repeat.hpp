#pragma once

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xsr {

inline std::vector<std::string> whisper_words(std::string_view text, std::vector<std::size_t>* offsets = nullptr) {
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    std::vector<std::string> words;
    bool in_word = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const bool decimal = c == '.' && i > 0 && i + 1 < text.size() &&
            is_digit(text[i - 1]) && is_digit(text[i + 1]);
        if (!decimal && std::string_view(" \t\r\n.,!?;:").find(c) != std::string_view::npos) {
            in_word = false;
            continue;
        }
        if (!in_word) {
            words.emplace_back();
            if (offsets) offsets->push_back(i);
        }
        words.back() += c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        in_word = true;
    }
    return words;
}

inline bool whisper_number_word(const std::string& word) {
    constexpr std::string_view spoken[] = {"zero", "oh", "one", "two", "three", "tree", "four", "fower",
        "five", "fife", "six", "seven", "eight", "nine", "niner", "ten", "hundred", "thousand", "point",
        "decimal", "minus", "plus", "double", "triple"};
    return word.find_first_not_of("0123456789+-.") == std::string::npos ||
        std::find(std::begin(spoken), std::end(spoken), std::string_view(word)) != std::end(spoken);
}

inline std::optional<std::size_t> whisper_repeated_prefix_end(std::string_view text) {
    std::vector<std::size_t> offsets;
    const auto words = whisper_words(text, &offsets);
    for (std::size_t width = 1; width <= words.size() / 2; ++width) {
        if (std::all_of(words.begin(), words.begin() + width, whisper_number_word)) continue;
        std::size_t i = width;
        // Permit an incomplete last copy when the decoder stops mid-loop.
        while (i < words.size() && words[i] == words[i % width]) ++i;
        if (i == words.size()) return text.substr(0, offsets[width]).find_last_not_of(" \t\r\n,;:") + 1;
    }
    return std::nullopt;
}

inline std::optional<std::vector<std::string>> whisper_without_repeated_tail(std::string_view text) {
    auto words = whisper_words(text);
    auto kept = words.size();
    for (std::size_t width = 1; width <= words.size() / 2; ++width) {
        const auto suffix = words.end() - width;
        if (std::all_of(suffix, words.end(), whisper_number_word)) continue;
        std::size_t begin = words.size() - width;
        while (begin >= width && std::equal(words.begin() + begin - width, words.begin() + begin, suffix))
            begin -= width;
        kept = std::min(kept, begin + width);
    }
    if (kept == words.size()) return std::nullopt;
    words.resize(kept);
    return words;
}

} // namespace xsr
