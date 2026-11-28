#include "core.hpp"
#include "activity.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace xsl;

static void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

int main() {
    try {
        auto now = Clock::time_point{};
        ActivityProgress microphone;
        microphone.requested_at = now;
        microphone.microphone = true;
        require(microphone.phase() == ActivityPhase::awaiting_audio, "PTT acceptance is not audio");
        microphone.confirm_audio(now + 320ms);
        require(microphone.phase() == ActivityPhase::capturing && microphone.capture_delay_ms == 320,
                "first audio confirms capture and records its delay");
        microphone.confirm_audio(now + 500ms);
        microphone.processing_since = now + 650ms;
        require(microphone.phase() == ActivityPhase::transcribing && microphone.capture_delay_ms == 320,
                "release retains the first audio delay");
        ActivityProgress quick_press;
        quick_press.microphone = true;
        quick_press.processing_since = now + 40ms;
        quick_press.confirm_audio(now + 350ms);
        require(quick_press.phase() == ActivityPhase::released_before_audio && !quick_press.capture_delay_ms,
                "late audio does not turn a released PTT into listening");
        ActivityProgress wave;
        wave.processing_since = now;
        require(wave.phase() == ActivityPhase::transcribing, "WAV tests skip microphone startup");
        std::vector<Json> requests;
        std::vector<Delivery> deliveries;
        std::string blocked;
        Broker broker([&](const Json& value) {
                          requests.push_back(value);
                          return value.at("type") != blocked;
                      },
                      [&](const Delivery& value) { deliveries.push_back(value); });
        const auto request = [&](int sender, uint32_t op, uint64_t session = 0,
                                 const XslString* terms = nullptr, uint32_t n = 0) {
            XslRequest command{};
            command.version = XSL_ABI_VERSION;
            command.struct_size = sizeof(command);
            command.operation = op;
            command.session = session;
            command.terms = terms;
            command.term_count = n;
            broker.request(sender, command, now);
            return command;
        };
        const auto confirm_configuration = [&](uint32_t state = XSL_MODEL_UNLOADED) {
            require(requests.back().at("type") == "configure", "configuration sent");
            broker.accept({{"type", "state"}, {"generation", requests.back().at("generation")}, {"state", state}}, now);
        };
        const auto begin_with_gain = [&](int sender, uint32_t percent) {
            XslPttBeginRequest command{};
            command.request.version = XSL_ABI_VERSION;
            command.request.struct_size = sizeof(command);
            command.request.operation = XSL_PTT_BEGIN;
            command.monitor_gain_percent = percent;
            broker.request(sender, command.request, now);
            return command.request;
        };
        require(request(1, XSL_PTT_BEGIN).status == XSL_NOT_SUBSCRIBED, "no subscription");
        require(request(1, XSL_SUBSCRIBE).status == XSL_OK, "first subscription");
        require(request(2, XSL_SUBSCRIBE).status == XSL_OK, "second subscription");
        require(request(1, XSL_PTT_BEGIN).status == XSL_MODEL_UNAVAILABLE, "no default model");
        require(!broker.service_ready(), "service unavailable before handshake");

        broker.engine_online(now);
        confirm_configuration(XSL_MODEL_MISSING);
        uint64_t microphone_session = 0;
        require(broker.begin_microphone_test(now, microphone_session) == XSL_OK &&
                requests.back().value("microphone_test", false), "guided microphone test runs without a model");
        require(request(1, XSL_PTT_BEGIN).status == XSL_BUSY, "guided test owns the only capture slot");
        require(broker.end_local(microphone_session, now) == XSL_STALE_SESSION, "recognition test cannot end guided test");
        broker.accept({{"type", "capture"}, {"session", microphone_session}}, now);
        require(deliveries.back().client == microphone_client, "guided test capture is private");
        const auto sent_during_test = requests.size();
        broker.accept({{"type", "result"}, {"session", microphone_session}, {"status", XSL_OK},
                       {"peak_dbfs", -5.7}}, now);
        require(deliveries.back().client == microphone_client && deliveries.back().result.at("peak_dbfs") == -5.7 &&
                !broker.busy() && requests.size() == sent_during_test, "guided test returns levels without changing model state");
        deliveries.clear();

        Settings settings;
        settings.sidetone = true;
        settings.cpu_threads = 3;
        settings.profile = {"parakeet-tdt-0.6b-v3", "Q8_0", "revision", "/fixture"};
        broker.set_settings(settings, now);
        const auto sent_before_pause = requests.size();
        for (auto status : {XSL_ENGINE_DISABLED, XSL_ENGINE_SUSPENDED}) {
            broker.engine_lost(now, status);
            require(request(1, XSL_GET_STATUS).service_status == status &&
                    request(1, XSL_PTT_BEGIN).status == status &&
                    broker.begin_microphone_test(now, microphone_session) == status,
                    "paused engine rejects requests and reports its mode");
        }
        require(requests.size() == sent_before_pause && broker.subscribers().count(1) == 1,
                "pausing sends no capture and preserves subscriptions");
        broker.engine_online(now);
        require(requests.back().at("cpu_threads") == 3, "CPU thread preference reaches the engine on configuration");
        require(request(1, XSL_PTT_BEGIN).status == XSL_ENGINE_RESTARTING, "wait for configuration acknowledgement");
        confirm_configuration();
        require(broker.service_ready(), "engine online");
        XslString initial{"flight level", 12};
        XslString replace{"ready", 5};
        XslString invalid{"x", XSL_MAX_TERM_BYTES + 1};
        require(request(1, XSL_SET_VOCABULARY, 0, &initial, 1).status == XSL_OK, "terms");
        require(request(2, XSL_SET_VOCABULARY, 0, &replace, 1).status == XSL_OK, "second terms");
        require(request(1, XSL_SET_VOCABULARY, 0, &invalid, 1).status == XSL_VOCABULARY_LIMIT, "term limit");

        settings.beam_width = 3;
        settings.vocabulary_weight = 1.5f;
        broker.set_settings(settings, now);

        const auto sent_before_invalid_gain = requests.size();
        require(begin_with_gain(1, 101).status == XSL_BAD_REQUEST &&
                requests.size() == sent_before_invalid_gain, "invalid gain does not start a session");
        const auto one = request(1, XSL_PTT_BEGIN);
        require(one.status == XSL_OK && one.session > 0, "begin one");
        require(requests.back().at("sidetone") == true, "microphone monitoring forwarded to engine");
        require(requests.back().at("low_latency_monitoring") == false, "standard monitoring by default");
        require(requests.back().at("monitor_gain_percent") == XSL_DEFAULT_MONITOR_GAIN_PERCENT, "default monitoring level");
        require(requests.back().at("terms") == Json::array({"flight level"}), "snapshot");
        require(requests.back().at("beam_width") == 3 && requests.back().at("vocabulary_weight") == 1.5f,
                "decode settings snapshot");
        require(request(1, XSL_SET_VOCABULARY, 0, &replace, 1).status == XSL_OK, "replace in flight");
        require(request(2, XSL_PTT_BEGIN).status == XSL_BUSY, "busy");
        require(request(2, XSL_PTT_END, one.session).status == XSL_STALE_SESSION, "owner only");
        broker.accept({{"type", "capture"}, {"session", one.session}}, now);
        require(request(1, XSL_PTT_END, one.session).status == XSL_OK, "release");
        require(request(2, XSL_PTT_BEGIN).status == XSL_BUSY, "busy until terminal");
        broker.accept({{"type", "result"}, {"session", one.session}, {"status", XSL_OK},
                       {"text", "FL250."}, {"candidates", Json::array({"FL250.", "FL 250."})},
                       {"vocabulary", XSL_VOCAB_APPLIED}}, now);
        require(deliveries.size() == 2 && deliveries.back().client == 1 &&
                deliveries.back().result.at("quant") == "Q8_0" &&
                deliveries.back().result.at("candidates") == Json::array({"FL250.", "FL 250."}),
                "targeted delivery of ranked text");
        settings.low_latency_monitoring = true;
        settings.monitor_gain_percent = 27;
        broker.set_settings(settings, now);
        const auto two = begin_with_gain(1, 40);
        require(two.status == XSL_OK && requests.back().at("sidetone") == true &&
                requests.back().at("monitor_gain_percent") == 40 && requests.back().at("low_latency_monitoring") == true,
                "gain and low-latency mode apply to this capture");
        require(requests.back().at("terms") == Json::array({"ready"}), "new vocabulary");
        now += 3s;
        broker.tick(now);
        require(deliveries.back().status == XSL_TIMEOUT &&
                deliveries.back().stage == XSL_STAGE_CAPTURE_START, "capture timeout");
        broker.accept({{"type", "result"}, {"session", two.session}, {"status", XSL_CANCELLED}}, now);
        require(deliveries.size() == 3, "ignore late result");

        settings.vocabulary_enabled = false;
        broker.set_settings(settings, now);
        const auto three = request(2, XSL_PTT_BEGIN);
        require(three.status == XSL_OK && requests.back().at("monitor_gain_percent") == 27,
                "base PTT uses configured gain after another session overrides it");
        require(requests.back().at("terms").empty() && broker.subscribers().at(2).size() == 1,
                "lexical bias switch suppresses terms without changing the registration");
        broker.accept({{"type", "capture"}, {"session", three.session}}, now);
        now += 30s;
        broker.tick(now);
        require(deliveries.back().stage == XSL_STAGE_CAPTURE && deliveries.back().client == 2, "hold timeout");
        require(broker.restart_due(now + 1s) && !broker.service_ready(), "draining service requests restart");
        broker.engine_lost(now);
        require(deliveries.size() == 5, "one terminal per session");
        broker.engine_online(now);
        confirm_configuration();
        settings.vocabulary_enabled = true;
        settings.beam_width = 1;
        settings.vocabulary_weight = -1.0f;
        broker.set_settings(settings, now);
        const auto early = begin_with_gain(2, 0);
        require(early.status == XSL_OK && requests.back().at("sidetone") == false &&
                requests.back().at("low_latency_monitoring") == false,
                "zero gain uses capture without monitoring");
        require(request(2, XSL_PTT_END, early.session).status == XSL_OK, "early end");
        const auto before_late_capture = deliveries.size();
        broker.accept({{"type", "capture"}, {"session", early.session}}, now + 100ms);
        require(deliveries.size() == before_late_capture, "capture after release is not delivered");
        broker.accept({{"type", "result"}, {"session", early.session}, {"status", XSL_NO_AUDIO}}, now);
        require(deliveries.back().status == XSL_NO_AUDIO, "no audio");
        require(request(2, XSL_PTT_BEGIN).status == XSL_OK, "restarted");
        broker.engine_lost(now);
        require(deliveries.back().status == XSL_ENGINE_LOST, "engine loss");
        broker.engine_online(now);
        const auto generation = requests.back().at("generation");
        broker.accept({{"type", "state"}, {"generation", generation}, {"state", XSL_MODEL_READY}}, now);
        uint64_t local = 0;
        require(broker.begin_local({}, now, local) == XSL_OK && requests.back().at("monitor_gain_percent") == 27,
                "local PTT uses configured gain");
        broker.accept({{"type", "result"}, {"session", local}, {"status", XSL_NO_AUDIO}}, now);
        const auto warm = request(1, XSL_PTT_BEGIN);
        broker.accept({{"type", "capture"}, {"session", warm.session}}, now);
        require(request(1, XSL_PTT_END, warm.session).status == XSL_OK, "warm end");
        now += 10s;
        broker.tick(now);
        require(deliveries.back().status == XSL_TIMEOUT &&
                deliveries.back().stage == XSL_STAGE_TRANSCRIPTION, "warm decode deadline");
        broker.accept({{"type", "result"}, {"session", warm.session}, {"status", XSL_CANCELLED}}, now);
        broker.accept({{"type", "state"}, {"generation", generation}, {"state", XSL_MODEL_UNLOADED}}, now);
        const auto cold = request(1, XSL_PTT_BEGIN);
        broker.accept({{"type", "capture"}, {"session", cold.session}}, now);
        require(request(1, XSL_PTT_END, cold.session).status == XSL_OK, "cold end");
        now += 10s;
        broker.tick(now);
        require(broker.busy(), "cold uses longer deadline");
        now += 10s;
        broker.tick(now);
        require(deliveries.back().status == XSL_TIMEOUT, "cold decode deadline");
        broker.accept({{"type", "result"}, {"session", cold.session}, {"status", XSL_CANCELLED}}, now);

        Settings replacement = settings;
        replacement.profile.path = "/bad-model";
        broker.set_settings(replacement, now);
        confirm_configuration();
        const auto failed = request(1, XSL_PTT_BEGIN);
        require(failed.status == XSL_OK, "new model attempted at first use");
        broker.accept({{"type", "result"}, {"session", failed.session}, {"status", XSL_MODEL_UNAVAILABLE}}, now);
        require(broker.settings().profile.path == settings.profile.path, "rollback to previous model");

        confirm_configuration();
        const auto lost_end = request(1, XSL_PTT_BEGIN);
        require(lost_end.status == XSL_OK, "release test begins");
        broker.accept({{"type", "capture"}, {"session", lost_end.session}}, now);
        blocked = "end";
        require(request(1, XSL_PTT_END, lost_end.session).status == XSL_ENGINE_RESTARTING, "rejected release");
        require(!broker.busy() && !broker.service_ready() && broker.restart_due(now + 1s) &&
                deliveries.back().status == XSL_ENGINE_RESTARTING && requests.back().at("type") == "cancel",
                "rejected release aborts and drains immediately");
        const auto count = deliveries.size();
        broker.accept({{"type", "result"}, {"session", cold.session}, {"status", XSL_CANCELLED}}, now);
        require(!broker.service_ready(), "older result does not end drain");
        broker.accept({{"type", "result"}, {"session", lost_end.session}, {"status", XSL_CANCELLED}}, now);
        require(broker.service_ready() && deliveries.size() == count, "single terminal for aborted session");

        blocked = "configure";
        broker.set_settings(replacement, now);
        require(request(1, XSL_PTT_BEGIN).status == XSL_ENGINE_RESTARTING, "rejected configuration blocks new PTT");
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
        reopened.accept({{"type", "state"}, {"generation", requests.back().at("generation")}, {"state", XSL_MODEL_MISSING}}, now);
        require(reopened.settings().profile.path == settings.profile.path, "last good model restored across providers");

        require(Profile{parakeet_v2_id, {}, {}, {}, {}}.supported() &&
                !Profile{"unknown-profile", {}, {}, {}, {}}.supported(), "supported profile set");
        const XslString registered{"autopilot", 9}, override_term{"gear", 4};
        require(request(1, XSL_SET_VOCABULARY, 0, &registered, 1).status == XSL_OK, "register ordinary vocabulary");
        struct OlderBeginRequest { XslPttBeginRequest begin; XslString extension; } hints{};
        hints.begin = {{XSL_ABI_VERSION, sizeof(hints), XSL_PTT_BEGIN}, 25};
        hints.begin.request.terms = &override_term;
        hints.begin.request.term_count = 1;
        hints.extension = {"cockpit", 7};
        broker.request(1, hints.begin.request, now);
        require(hints.begin.request.status == XSL_OK && requests.back().at("terms") == Json::array({"autopilot"}) &&
                requests.back().at("monitor_gain_percent") == 25 && !requests.back().contains("context"),
                "larger ABI v1 payload uses the registered vocabulary");
        broker.accept({{"type", "result"}, {"session", hints.begin.request.session}, {"status", XSL_NO_AUDIO}}, now);
        broker.shutdown(now);
        require(broker.subscribers().empty() && deliveries.back().kind == XSL_SERVICE_STOPPED, "shutdown");

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
