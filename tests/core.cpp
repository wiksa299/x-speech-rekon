#include "core.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace xsr;

static void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

int main() {
    try {
        auto now = Clock::time_point{};
        std::vector<Json> requests;
        std::vector<Delivery> deliveries;
        std::string blocked;
        Broker broker([&](const Json& value) {
                          requests.push_back(value);
                          return value.at("type") != blocked;
                      },
                      [&](const Delivery& value) { deliveries.push_back(value); });
        const auto request = [&](int sender, uint32_t op, uint64_t session = 0,
                                 const XsrString* terms = nullptr, uint32_t n = 0) {
            XsrRequest command{};
            command.version = XSR_ABI_VERSION;
            command.struct_size = sizeof(command);
            command.operation = op;
            command.session = session;
            command.terms = terms;
            command.term_count = n;
            broker.request(sender, command, now);
            return command;
        };
        const auto confirm_configuration = [&](uint32_t state = XSR_MODEL_UNLOADED) {
            require(requests.back().at("type") == "configure", "configuration sent");
            broker.accept({{"type", "state"}, {"generation", requests.back().at("generation")}, {"state", state}}, now);
        };
        require(request(1, XSR_PTT_BEGIN).status == XSR_NOT_SUBSCRIBED, "no subscription");
        require(request(1, XSR_SUBSCRIBE).status == XSR_OK, "first subscription");
        require(request(2, XSR_SUBSCRIBE).status == XSR_OK, "second subscription");
        require(request(1, XSR_PTT_BEGIN).status == XSR_MODEL_UNAVAILABLE, "no default model");
        require(!broker.service_ready(), "service unavailable before handshake");

        Settings settings;
        settings.profile = {"parakeet-tdt-0.6b-v3", "Q8_0", "revision", "/fixture"};
        broker.set_settings(settings, now);
        broker.engine_online(now);
        require(request(1, XSR_PTT_BEGIN).status == XSR_ENGINE_RESTARTING, "wait for configuration acknowledgement");
        confirm_configuration();
        require(broker.service_ready(), "engine online");
        XsrString initial{"flight level", 12};
        XsrString replace{"ready", 5};
        XsrString invalid{"x", XSR_MAX_TERM_BYTES + 1};
        require(request(1, XSR_SET_VOCABULARY, 0, &initial, 1).status == XSR_OK, "terms");
        require(request(2, XSR_SET_VOCABULARY, 0, &replace, 1).status == XSR_OK, "second terms");
        require(request(1, XSR_SET_VOCABULARY, 0, &invalid, 1).status == XSR_VOCABULARY_LIMIT, "term limit");

        const auto one = request(1, XSR_PTT_BEGIN);
        require(one.status == XSR_OK && one.session > 0, "begin one");
        require(requests.back().at("terms") == Json::array({"flight level"}), "snapshot");
        require(request(1, XSR_SET_VOCABULARY, 0, &replace, 1).status == XSR_OK, "replace in flight");
        require(request(2, XSR_PTT_BEGIN).status == XSR_BUSY, "busy");
        require(request(2, XSR_PTT_END, one.session).status == XSR_STALE_SESSION, "owner only");
        broker.accept({{"type", "capture"}, {"session", one.session}}, now);
        require(request(1, XSR_PTT_END, one.session).status == XSR_OK, "release");
        require(request(2, XSR_PTT_BEGIN).status == XSR_BUSY, "busy until terminal");
        broker.accept({{"type", "result"}, {"session", one.session}, {"status", XSR_OK},
                       {"text", "FL250."}, {"vocabulary", XSR_VOCAB_APPLIED}}, now);
        require(deliveries.size() == 2 && deliveries.back().client == 1 &&
                deliveries.back().result.at("quant") == "Q8_0", "targeted delivery");
        const auto two = request(1, XSR_PTT_BEGIN);
        require(requests.back().at("terms") == Json::array({"ready"}), "new vocabulary");
        now += 3s;
        broker.tick(now);
        require(deliveries.back().status == XSR_TIMEOUT &&
                deliveries.back().stage == XSR_STAGE_CAPTURE_START, "capture timeout");
        broker.accept({{"type", "result"}, {"session", two.session}, {"status", XSR_CANCELLED}}, now);
        require(deliveries.size() == 3, "ignore late result");

        const auto three = request(2, XSR_PTT_BEGIN);
        broker.accept({{"type", "capture"}, {"session", three.session}}, now);
        now += 30s;
        broker.tick(now);
        require(deliveries.back().stage == XSR_STAGE_CAPTURE && deliveries.back().client == 2, "hold timeout");
        require(broker.restart_due(now + 1s) && !broker.service_ready(), "draining service requests restart");
        broker.engine_lost(now);
        require(deliveries.size() == 5, "one terminal per session");
        broker.engine_online(now);
        confirm_configuration();
        const auto early = request(2, XSR_PTT_BEGIN);
        require(request(2, XSR_PTT_END, early.session).status == XSR_OK, "early end");
        broker.accept({{"type", "result"}, {"session", early.session}, {"status", XSR_NO_AUDIO}}, now);
        require(deliveries.back().status == XSR_NO_AUDIO, "no audio");
        require(request(2, XSR_PTT_BEGIN).status == XSR_OK, "restarted");
        broker.engine_lost(now);
        require(deliveries.back().status == XSR_ENGINE_LOST, "engine loss");
        broker.engine_online(now);
        const auto generation = requests.back().at("generation");
        broker.accept({{"type", "state"}, {"generation", generation}, {"state", XSR_MODEL_READY}}, now);
        const auto warm = request(1, XSR_PTT_BEGIN);
        broker.accept({{"type", "capture"}, {"session", warm.session}}, now);
        require(request(1, XSR_PTT_END, warm.session).status == XSR_OK, "warm end");
        now += 10s;
        broker.tick(now);
        require(deliveries.back().status == XSR_TIMEOUT &&
                deliveries.back().stage == XSR_STAGE_TRANSCRIPTION, "warm decode deadline");
        broker.accept({{"type", "result"}, {"session", warm.session}, {"status", XSR_CANCELLED}}, now);
        broker.accept({{"type", "state"}, {"generation", generation}, {"state", XSR_MODEL_UNLOADED}}, now);
        const auto cold = request(1, XSR_PTT_BEGIN);
        broker.accept({{"type", "capture"}, {"session", cold.session}}, now);
        require(request(1, XSR_PTT_END, cold.session).status == XSR_OK, "cold end");
        now += 10s;
        broker.tick(now);
        require(broker.busy(), "cold uses longer deadline");
        now += 10s;
        broker.tick(now);
        require(deliveries.back().status == XSR_TIMEOUT, "cold decode deadline");
        broker.accept({{"type", "result"}, {"session", cold.session}, {"status", XSR_CANCELLED}}, now);

        Settings replacement = settings;
        replacement.profile.path = "/bad-model";
        broker.set_settings(replacement, now);
        confirm_configuration();
        const auto failed = request(1, XSR_PTT_BEGIN);
        require(failed.status == XSR_OK, "new model attempted at first use");
        broker.accept({{"type", "result"}, {"session", failed.session}, {"status", XSR_MODEL_UNAVAILABLE}}, now);
        require(broker.settings().profile.path == settings.profile.path, "rollback to previous model");

        confirm_configuration();
        const auto lost_end = request(1, XSR_PTT_BEGIN);
        require(lost_end.status == XSR_OK, "release test begins");
        broker.accept({{"type", "capture"}, {"session", lost_end.session}}, now);
        blocked = "end";
        require(request(1, XSR_PTT_END, lost_end.session).status == XSR_ENGINE_RESTARTING, "rejected release");
        require(!broker.busy() && !broker.service_ready() && broker.restart_due(now + 1s) &&
                deliveries.back().status == XSR_ENGINE_RESTARTING && requests.back().at("type") == "cancel",
                "rejected release aborts and drains immediately");
        const auto count = deliveries.size();
        broker.accept({{"type", "result"}, {"session", cold.session}, {"status", XSR_CANCELLED}}, now);
        require(!broker.service_ready(), "older result does not end drain");
        broker.accept({{"type", "result"}, {"session", lost_end.session}, {"status", XSR_CANCELLED}}, now);
        require(broker.service_ready() && deliveries.size() == count, "single terminal for aborted session");

        blocked = "configure";
        broker.set_settings(replacement, now);
        require(request(1, XSR_PTT_BEGIN).status == XSR_ENGINE_RESTARTING, "rejected configuration blocks new PTT");
        const auto rejected_generation = requests.back().at("generation");
        blocked.clear();
        broker.tick(now + 100ms);
        require(requests.back().at("generation") == rejected_generation, "retry the desired configuration");
        confirm_configuration();
        require(broker.service_ready(), "service resumes after confirmation");
        broker.accept({{"type", "inference_rejected"}, {"operation", "configure"}, {"generation", rejected_generation}}, now);
        require(!broker.service_ready(), "engine-level refusal suspends PTT");
        broker.tick(now + 100ms);
        confirm_configuration();

        Broker reopened([&](const Json& value) { requests.push_back(value); return true; }, [](const Delivery&) {});
        reopened.restore_last_good(broker.last_good());
        reopened.set_settings(replacement, now);
        reopened.engine_online(now);
        reopened.accept({{"type", "state"}, {"generation", requests.back().at("generation")}, {"state", XSR_MODEL_MISSING}}, now);
        require(reopened.settings().profile.path == settings.profile.path, "last good model restored across providers");
        broker.shutdown(now);
        require(broker.subscribers().empty() && deliveries.back().kind == XSR_SERVICE_STOPPED, "shutdown");

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
