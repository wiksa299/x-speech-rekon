// Minimal protocol peer for testing locale changes during PTT without audio hardware.
#include "core.hpp"

#include <iostream>

int locale_test_engine() {
    using namespace xsl;
    Json profile = {{"path", "fixture.gguf"}, {"id", parakeet_v2_id}, {"quant", "Q8_0"},
                    {"revision", "fixture"}, {"sha256", std::string(64, 'a')}, {"state", "candidate"}};
    auto emit = [](const Json& event) { std::cout << wire_frame(event) << std::flush; };
    emit({{"type", "hello"}, {"protocol", 1}, {"version", "fixture"}, {"devices", Json::array()}});
    Json cmd;
    std::string language;
    while (read_frame(std::cin, cmd)) {
        const auto type = cmd.at("type").get<std::string>();
        if (type == "configure")
            emit({{"type", "state"}, {"generation", cmd.at("generation")},
                  {"state", cmd.at("path") == "" ? XSL_MODEL_MISSING : XSL_MODEL_READY}});
        else if (type == "scan")
            emit({{"type", type}, {"ticket", cmd.at("ticket")}, {"status", "ok"}, {"items", Json::array({profile})}});
        else if (type == "identify")
            emit({{"type", type}, {"ticket", cmd.at("ticket")}, {"model", profile}});
        else if (type == "microphones")
            emit({{"type", type}, {"items", Json::array()}});
        else if (type == "begin") {
            language = cmd.at("language").get<std::string>();
            emit({{"type", "capture"}, {"session", cmd.at("session")}});
        } else if (type == "end")
            emit({{"type", "result"}, {"status", XSL_OK}, {"session", cmd.at("session")},
                  {"text", "Heading select."}, {"language", language}, {"vocabulary", XSL_VOCAB_NONE},
                  {"capture_ms", 500}, {"compute_ms", 10}});
        else if (type == "shutdown") break;
    }
    return 0;
}
