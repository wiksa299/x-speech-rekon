#include "core.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace xvr;

static void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

int main() {
    try {
        auto now = Clock::time_point{};
        std::vector<Json> requests;
        std::vector<Delivery> deliveries;
        Broker broker([&](const Json& value) { requests.push_back(value); return true; },
                      [&](const Delivery& value) { deliveries.push_back(value); });
        const auto request = [&](int sender, uint32_t op, uint64_t session = 0,
                                 const XvrString* terms = nullptr, uint32_t n = 0) {
            XvrRequest command{};
            command.version = XVR_ABI_VERSION;
            command.struct_size = sizeof(command);
            command.operation = op;
            command.session = session;
            command.terms = terms;
            command.term_count = n;
            broker.request(sender, command, now);
            return command;
        };
        require(request(1, XVR_PTT_BEGIN).status == XVR_NOT_SUBSCRIBED, "no subscription");
        require(request(1, XVR_SUBSCRIBE).status == XVR_OK, "first subscription");
        require(request(2, XVR_SUBSCRIBE).status == XVR_OK, "second subscription");
        require(request(1, XVR_PTT_BEGIN).status == XVR_MODEL_UNAVAILABLE, "no default model");

        Settings settings;
        settings.profile = {"parakeet-tdt-0.6b-v3", "Q8_0", "revision", "/fixture"};
        broker.set_settings(settings, now);
        broker.engine_online(now);
        XvrString initial{"flight level", 12};
        XvrString replace{"ready", 5};
        XvrString invalid{"x", XVR_MAX_TERM_BYTES + 1};
        require(request(1, XVR_SET_VOCABULARY, 0, &initial, 1).status == XVR_OK, "terms");
        require(request(2, XVR_SET_VOCABULARY, 0, &replace, 1).status == XVR_OK, "second terms");
        require(request(1, XVR_SET_VOCABULARY, 0, &invalid, 1).status == XVR_VOCABULARY_LIMIT, "term limit");

        const auto one = request(1, XVR_PTT_BEGIN);
        require(one.status == XVR_OK && one.session > 0, "begin one");
        require(requests.back().at("terms") == Json::array({"flight level"}), "snapshot");
        require(request(1, XVR_SET_VOCABULARY, 0, &replace, 1).status == XVR_OK, "replace in flight");
        require(request(2, XVR_PTT_BEGIN).status == XVR_BUSY, "busy");
        require(request(2, XVR_PTT_END, one.session).status == XVR_STALE_SESSION, "owner only");
        broker.accept({{"type", "capture"}, {"session", one.session}}, now);
        require(request(1, XVR_PTT_END, one.session).status == XVR_OK, "release");
        require(request(2, XVR_PTT_BEGIN).status == XVR_BUSY, "busy until terminal");
        broker.accept({{"type", "result"}, {"session", one.session}, {"status", XVR_OK},
                       {"text", "FL250."}, {"vocabulary", XVR_VOCAB_APPLIED}}, now);
        require(deliveries.size() == 2 && deliveries.back().client == 1 &&
                deliveries.back().result.at("quant") == "Q8_0", "targeted delivery");
        const auto two = request(1, XVR_PTT_BEGIN);
        require(requests.back().at("terms") == Json::array({"ready"}), "new vocabulary");
        now += 3s;
        broker.tick(now);
        require(deliveries.back().status == XVR_TIMEOUT &&
                deliveries.back().stage == XVR_STAGE_CAPTURE_START, "capture timeout");
        broker.accept({{"type", "result"}, {"session", two.session}, {"status", XVR_CANCELLED}}, now);
        require(deliveries.size() == 3, "ignore late result");

        const auto three = request(2, XVR_PTT_BEGIN);
        broker.accept({{"type", "capture"}, {"session", three.session}}, now);
        now += 30s;
        broker.tick(now);
        require(deliveries.back().stage == XVR_STAGE_CAPTURE && deliveries.back().client == 2, "hold timeout");
        require(broker.restart_due(now + 1s), "hard timeout requests restart");
        broker.engine_lost(now);
        require(deliveries.size() == 5, "one terminal per session");
        broker.engine_online(now);
        const auto early = request(2, XVR_PTT_BEGIN);
        require(request(2, XVR_PTT_END, early.session).status == XVR_OK, "early end");
        broker.accept({{"type", "result"}, {"session", early.session}, {"status", XVR_NO_AUDIO}}, now);
        require(deliveries.back().status == XVR_NO_AUDIO, "no audio");
        require(request(2, XVR_PTT_BEGIN).status == XVR_OK, "restarted");
        broker.engine_lost(now);
        require(deliveries.back().status == XVR_ENGINE_LOST, "engine loss");
        broker.engine_online(now);
        const auto generation = requests.back().at("generation");
        broker.accept({{"type", "state"}, {"generation", generation}, {"state", XVR_MODEL_READY}}, now);
        const auto warm = request(1, XVR_PTT_BEGIN);
        broker.accept({{"type", "capture"}, {"session", warm.session}}, now);
        require(request(1, XVR_PTT_END, warm.session).status == XVR_OK, "warm end");
        now += 10s;
        broker.tick(now);
        require(deliveries.back().status == XVR_TIMEOUT &&
                deliveries.back().stage == XVR_STAGE_TRANSCRIPTION, "warm decode deadline");
        broker.accept({{"type", "result"}, {"session", warm.session}, {"status", XVR_CANCELLED}}, now);
        broker.accept({{"type", "state"}, {"generation", generation}, {"state", XVR_MODEL_UNLOADED}}, now);
        const auto cold = request(1, XVR_PTT_BEGIN);
        broker.accept({{"type", "capture"}, {"session", cold.session}}, now);
        require(request(1, XVR_PTT_END, cold.session).status == XVR_OK, "cold end");
        now += 10s;
        broker.tick(now);
        require(broker.busy(), "cold uses longer deadline");
        now += 10s;
        broker.tick(now);
        require(deliveries.back().status == XVR_TIMEOUT, "cold decode deadline");
        broker.accept({{"type", "result"}, {"session", cold.session}, {"status", XVR_CANCELLED}}, now);

        Settings replacement = settings;
        replacement.profile.path = "/bad-model";
        broker.set_settings(replacement, now);
        const auto failed = request(1, XVR_PTT_BEGIN);
        require(failed.status == XVR_OK, "new model attempted at first use");
        broker.accept({{"type", "result"}, {"session", failed.session}, {"status", XVR_MODEL_UNAVAILABLE}}, now);
        require(broker.settings().profile.path == settings.profile.path, "rollback to previous model");

        broker.shutdown(now);
        require(broker.subscribers().empty() && deliveries.back().kind == XVR_SERVICE_STOPPED, "shutdown");

        require(valid_utf8("Réglages micrófono") && !valid_utf8("\xed\xa0\x80") &&
                !valid_utf8("\xf0\x80\x80\x80"), "UTF-8");
        std::istringstream stream(wire_frame({{"type", "hello"}, {"protocol", 1}}));
        Json message;
        require(read_frame(stream, message) && message.at("protocol") == 1, "IPC frame");
        std::istringstream oversized(std::string(max_frame + 1, 'a') + "\n");
        require(!read_frame(oversized, message), "IPC bound");
        std::cout << "Broker and IPC checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
