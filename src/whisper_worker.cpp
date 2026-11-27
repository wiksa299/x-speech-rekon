#include "core.hpp"
#include "whisper_repeat.hpp"

#include <ggml-backend.h>
#include <grammar-parser.h>
#include <whisper.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using namespace xsr;
using WhisperModel = std::unique_ptr<whisper_context, decltype(&whisper_free)>;

int find_gpu(const std::string& requested, const std::string& description) {
    if (requested.rfind("vulkan/", 0) != 0) return -1;
    const auto id = requested.substr(7);
    int ordinal = 0;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        const auto dev = ggml_backend_dev_get(i);
        ggml_backend_dev_props props{};
        ggml_backend_dev_get_props(dev, &props);
        if (props.type != GGML_BACKEND_DEVICE_TYPE_GPU && props.type != GGML_BACKEND_DEVICE_TYPE_IGPU) continue;
        const auto reg = ggml_backend_dev_backend_reg(dev);
        if (std::string_view(ggml_backend_reg_name(reg)) == "Vulkan" &&
            ((props.device_id && id == props.device_id) ||
             (props.name && props.description && id == props.name && description == props.description))) return ordinal;
        ++ordinal;
    }
    return -1;
}

WhisperModel open_model(const fs::path& path, bool& gpu, int ordinal) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {nullptr, whisper_free};
    whisper_model_loader loader{
        &input,
        [](void* stream, void* data, size_t size) {
            auto& file = *static_cast<std::ifstream*>(stream);
            file.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
            return static_cast<size_t>(file.gcount());
        },
        [](void* stream) { return !*static_cast<std::ifstream*>(stream); },
        [](void*) {}
    };
    auto params = whisper_context_default_params();
    params.use_gpu = ordinal >= 0;
    params.gpu_device = std::max(0, ordinal);
    bool gpu_init_failed = false;
    whisper_log_set(+[](ggml_log_level level, const char* message, void* data) {
        if (level == GGML_LOG_LEVEL_ERROR && std::string_view(message).find("failed to initialize") != std::string_view::npos)
            *static_cast<bool*>(data) = true;
        std::cerr << message;
    }, &gpu_init_failed);
    WhisperModel model{whisper_init_with_params(&loader, params), whisper_free};
    whisper_log_set(nullptr, nullptr);
    gpu = params.use_gpu && !gpu_init_failed && model != nullptr;
    if (params.use_gpu && !gpu) model.reset();
    return model;
}

bool append_pcm(std::vector<float>& samples, const std::string& pcm) {
    static_assert(sizeof(float) == 4);
    constexpr auto hex_per_sample = sizeof(float) * 2;
    const auto count = pcm.size() / hex_per_sample;
    if (pcm.size() % hex_per_sample || count > whisper_chunk_samples || samples.size() + count > max_samples) return false;
    const auto offset = samples.size();
    samples.resize(offset + count);
    auto bytes = reinterpret_cast<unsigned char*>(samples.data() + offset);
    for (size_t i = 0; i < count * sizeof(float); ++i) {
        const auto start = pcm.data() + 2 * i;
        const auto decoded = std::from_chars(start, start + 2, bytes[i], 16);
        if (decoded.ec != std::errc{} || decoded.ptr != start + 2) return false;
    }
    return std::all_of(samples.begin() + offset, samples.end(), [](float value) { return std::isfinite(value); });
}

std::string grammar_literal(const std::string& term) {
    std::string literal;
    const auto first = term.front();
    size_t start = 0;
    if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z')) {
        const auto lower = static_cast<char>(first | 0x20);
        literal = std::string("[") + lower + static_cast<char>(lower & ~0x20) + "]";
        start = 1;
    }
    literal += '"';
    for (size_t i = start; i < term.size(); ++i) {
        if (term[i] == '"' || term[i] == '\\') literal += '\\';
        literal += term[i];
    }
    return literal + '"';
}

Json decode(whisper_context* model, const std::vector<float>& samples, const std::vector<std::string>& terms) {
    auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.language = "en";
    params.n_threads = static_cast<int>(std::min(6u, std::max(2u, std::thread::hardware_concurrency()) / 2));
    params.no_context = true;
    params.no_timestamps = true;
    params.single_segment = true;
    params.print_progress = params.print_realtime = params.print_timestamps = params.print_special = false;
    params.temperature_inc = 0.f;
    params.audio_ctx = std::min(whisper_n_audio_ctx(model),
        std::max(256, static_cast<int>((samples.size() + 319) / 320) + 32));

    std::string prompt, alternatives;
    std::array<whisper_token, 223> tokens{};
    int token_count = 0;
    for (const auto& term : terms) {
        if (term.empty() || !valid_utf8(term) || std::any_of(term.begin(), term.end(), [](unsigned char c) {
            return c < 0x20 || c == 0x7f;
        })) continue;
        auto next = prompt + (prompt.empty() ? "" : " ") + term;
        if (term.back() != '.' && term.back() != '?' && term.back() != '!') next += '.';
        auto trial = tokens;
        const auto count = whisper_tokenize(model, next.c_str(), trial.data(), static_cast<int>(trial.size()));
        if (count < 0) break;
        prompt = std::move(next);
        tokens = trial;
        token_count = count;
        if (!alternatives.empty()) alternatives += " | ";
        alternatives += grammar_literal(term);
    }

    grammar_parser::parse_state grammar;
    std::vector<const whisper_grammar_element*> rules;
    if (!alternatives.empty()) {
        // Allow a phrase to be followed by a value or other words. A subscriber
        // can supply partial phrases, and the existing parser validates commands.
        const auto source = std::string(
            "root ::= [ ]* ([Pp]\"lease\" [ ,]+)? phrase ([ ]+ [^\\r\\n]+)? [.?!]? [ ]*\nphrase ::= ") + alternatives + '\n';
        grammar = grammar_parser::parse(source.c_str());
        if (grammar.rules.empty()) return {{"status", XSR_ENGINE_ERROR}};
        rules = grammar.c_rules();
        params.grammar_rules = rules.data();
        params.n_grammar_rules = rules.size();
        params.i_start_rule = grammar.symbol_ids.at("root");
        params.grammar_penalty = 2.f;
        params.prompt_tokens = tokens.data();
        params.prompt_n_tokens = token_count;
    }

    const auto start = Clock::now();
    const auto run = [&](whisper_full_params options) {
        std::string text;
        if (whisper_full(model, options, samples.data(), static_cast<int>(samples.size())) != 0)
            return std::optional<std::string>{};
        for (int i = 0; i < whisper_full_n_segments(model); ++i) text += whisper_full_get_segment_text(model, i);
        return std::optional<std::string>{std::move(text)};
    };
    auto decoded = run(params);
    if (!decoded)
        return {{"status", XSR_ENGINE_ERROR}};
    auto& text = *decoded;
    if (text.find_first_not_of(" \r\t\n") == std::string::npos) return {{"status", XSR_NO_SPEECH}};
    if (text.size() > max_result_size) return {{"status", XSR_OUTPUT_TRUNCATED}};
    if (!valid_utf8(text)) return {{"status", XSR_ENGINE_ERROR}};
    uint32_t vocabulary = terms.empty() ? XSR_VOCAB_NONE : rules.empty() ? XSR_VOCAB_UNSUPPORTED : XSR_VOCAB_APPLIED;
    const auto prefix_end = whisper_repeated_prefix_end(text);
    const auto expected = prefix_end ? std::optional{whisper_words(std::string_view(text).substr(0, *prefix_end))} :
        whisper_without_repeated_tail(text);
    if (expected) {
        params.grammar_rules = nullptr;
        params.n_grammar_rules = 0;
        params.prompt_tokens = nullptr;
        params.prompt_n_tokens = 0;
        if (vocabulary != XSR_VOCAB_APPLIED) {
            params.strategy = WHISPER_SAMPLING_BEAM_SEARCH;
            params.beam_search.beam_size = 3;
        }
        auto retry = run(params);
        if (retry && retry->size() <= max_result_size && valid_utf8(*retry) && whisper_words(*retry) == *expected) {
            text = std::move(*retry);
            if (vocabulary == XSR_VOCAB_APPLIED) vocabulary = XSR_VOCAB_NONE;
        } else if (prefix_end) text.resize(*prefix_end);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    return {{"status", XSR_OK}, {"text", std::move(text)}, {"language", "en"},
            {"vocabulary", vocabulary},
            {"capture_ms", samples.size() * 1000 / sample_rate}, {"compute_ms", elapsed.count()}};
}

void send(const Json& message) {
    std::cout << wire_frame(message) << std::flush;
}

int run(const fs::path& directory) {
    ggml_backend_load_all_from_path(directory.u8string().c_str());
    if (!ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU)) return 1;
    WhisperModel model{nullptr, whisper_free};
    std::vector<float> samples;
    Json message;
    while (read_frame(std::cin, message)) {
        const auto type = message.at("type").get<std::string>();
        if (type == "shutdown") break;
        if (type == "load") {
            const auto path = fs::u8path(message.at("path").get<std::string>());
            const auto requested = message.value("compute", "cpu");
            const auto ordinal = find_gpu(requested, message.value("description", ""));
            bool gpu = false;
            model.reset();
            model = open_model(path, gpu, ordinal);
            if (!model && ordinal >= 0) model = open_model(path, gpu, -1);
            send({{"type", "loaded"}, {"ok", model != nullptr}, {"gpu", gpu}});
        } else if (type == "audio" && model) {
            if (!append_pcm(samples, message.at("pcm").get_ref<const std::string&>())) return 1;
        } else if (type == "decode" && model && !samples.empty()) {
            auto result = decode(model.get(), samples, message.at("terms").get<std::vector<std::string>>());
            samples.clear();
            result["type"] = "result";
            send(result);
        } else {
            return 1;
        }
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 1) return 1;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        return run(xsr::executable_path(argv[0]).parent_path());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
