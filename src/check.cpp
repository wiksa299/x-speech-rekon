#include "core.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace xsl;

class Check {
public:
    explicit Check(fs::path executable)
        : process_(std::move(executable)),
          broker_([&](Json message) {
              if (!injection_.empty() && message.value("type", "") == "begin")
                  message["injection"] = injection_;
              return process_.send(message);
          }, [&](const Delivery& message) { deliveries_.push_back(message); }) {}

    void start() {
        if (!process_.start()) throw std::runtime_error("could not start xsl-engine");
        devices_ = message("hello", 5s).value("devices", Json::array());
    }

    Json message(std::string_view type, Clock::duration timeout) {
        const auto end = Clock::now() + timeout;
        while (Clock::now() < end) {
            poll();
            const auto item = std::find_if(inbox_.begin(), inbox_.end(), [&](const Json& value) {
                return value.value("type", "") == type;
            });
            if (item != inbox_.end()) {
                auto out = std::move(*item);
                inbox_.erase(item);
                return out;
            }
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("timeout waiting for " + std::string(type));
    }

    void probe_microphone(const fs::path& wav) {
        const auto deadline = Clock::now() + 4s;
        while (!broker_.service_ready() && Clock::now() < deadline) {
            poll();
            std::this_thread::sleep_for(5ms);
        }
        injection_ = wav.u8string();
        uint64_t session = 0;
        check_status(broker_.begin_microphone_test(Clock::now(), session));
        while (Clock::now() < deadline) {
            poll();
            const auto result = std::find_if(deliveries_.begin(), deliveries_.end(), [&](const Delivery& value) {
                return value.session == session && value.kind == XSL_RESULT;
            });
            if (result != deliveries_.end()) {
                check_status(result->status);
                if (result->client != microphone_client || !result->result.value("microphone_test", false) ||
                    result->result.at("capture_ms").get<unsigned>() == 0 ||
                    result->result.at("peak_dbfs").get<float>() <= -60 || broker_.model_state() != XSL_MODEL_MISSING)
                    throw std::runtime_error("unexpected microphone test result");
                const auto metrics = result->result.dump();
                send({{"type", "mic_clear"}, {"session", session}});
                send({{"type", "mic_play"}, {"session", session}});
                const auto playback = message("mic_playback", 2s);
                if (playback.at("session") != session || playback.value("ok", true))
                    throw std::runtime_error("cleared recording can still be played");
                std::cout << "Microphone test OK without a model · " << metrics << '\n';
                return;
            }
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("timeout waiting for microphone test");
    }

    void fetch(const fs::path& folder, const Json& catalog, const std::string& selection) {
        for (const auto& entry : catalog) {
            if (!matches(entry, selection)) continue;
            send({{"type", "download"}, {"path", folder.u8string()}, {"entry", entry}});
            const auto result = message("download", 5min);
            if (result.value("status", "") != "ok")
                throw std::runtime_error("download failed for " + entry.at("file").get<std::string>() +
                                         " (" + result.value("status", "") + ")");
            std::cout << "Verified " << entry.at("file").get<std::string>() << '\n';
        }
    }

    void transcribe_models(const fs::path& folder, const Json& catalog, const fs::path& wav,
                           const std::string& selection, bool microphone, std::string compute, const std::string& language,
                           int beam_width) {
        if (compute == "vulkan") {
            if (devices_.empty()) throw std::runtime_error("no Vulkan device available");
            compute = devices_.front().at("id");
        }
        const auto device = std::find_if(devices_.begin(), devices_.end(), [&](const Json& item) {
            return item.at("id") == compute;
        });
        const std::string expected_compute = device == devices_.end() ? "CPU" : device->at("label").get<std::string>();
        const bool expected_fallback = compute != "cpu" && device == devices_.end();
        send({{"type", "scan"}, {"path", folder.u8string()}, {"catalog", catalog}});
        const auto found = message("scan", 10s).at("items");
        for (const auto& entry : catalog) {
            const auto name = entry.at("id").get<std::string>() + "/" + entry.at("quant").get<std::string>();
            if (!matches(entry, selection)) continue;
            const auto matches_profile = [&](const Json& item) {
                return item.value("id", "") == entry.at("id") && item.value("quant", "") == entry.at("quant");
            };
            auto file = std::find_if(found.begin(), found.end(), [&](const Json& item) {
                return matches_profile(item) && !item.value("previous", false);
            });
            if (file == found.end()) file = std::find_if(found.begin(), found.end(), matches_profile);
            if (file == found.end()) throw std::runtime_error("model unavailable: " + name);
            send({{"type", "identify"}, {"path", file->at("path")}, {"catalog", catalog}});
            const auto model = message("identify", 20s).at("model");
            if (!model.is_object()) throw std::runtime_error("model inspection failed: " + name);
            Settings settings;
            settings.profile = {model.at("id"), model.at("quant"), model.at("revision"),
                                model.at("path"), model.at("sha256")};
            settings.compute = compute;
            settings.language = language;
            settings.vocabulary_enabled = true;
            settings.beam_width = beam_width;
            broker_.set_settings(settings, Clock::now());
            const auto confirmed = Clock::now() + 5s;
            while (!broker_.service_ready() && Clock::now() < confirmed) {
                poll();
                std::this_thread::sleep_for(5ms);
            }

            uint64_t session = 0;
            deliveries_.clear();
            check_status(broker_.begin_local(wav.u8string(), Clock::now(), session));
            result(session, XSL_VOCAB_NONE, name + " offline");

            settings.early = true;
            settings.load = LoadPolicy::on_activate;
            broker_.set_settings(settings, Clock::now());
            const auto load_deadline = Clock::now() + 2s;
            while (broker_.model_state() == XSL_MODEL_LOADING && Clock::now() < load_deadline) {
                poll();
                std::this_thread::sleep_for(5ms);
            }
            if (broker_.model_state() != XSL_MODEL_READY)
                throw std::runtime_error("loaded model did not report ready after reconfiguration: " + name);
            const auto model_compute = expected_compute;
            const bool model_fallback = expected_fallback;
            if (broker_.actual_compute() != model_compute || broker_.cpu_fallback() != model_fallback)
                throw std::runtime_error("unexpected compute device for " + name + ": " + broker_.actual_compute());
            std::cout << "Compute device: " << model_compute << (model_fallback ? " (fallback)" : "") << '\n';
            injection_ = microphone ? "" : wav.u8string();
            command(XSL_SUBSCRIBE);
            XslString word{"landing gear", 12};
            command(XSL_SET_VOCABULARY, 0, &word, 1);
            deliveries_.clear();
            session = command(XSL_PTT_BEGIN);
            if (microphone) {
                std::cout << "Speak into the default microphone for four seconds.\n" << std::flush;
                const auto end = Clock::now() + 4s;
                while (Clock::now() < end) {
                    poll();
                    std::this_thread::sleep_for(25ms);
                }
            } else {
                wait_for_capture(session);
            }
            command(XSL_PTT_END, session);
            result(session, settings.profile.supports_vocabulary(settings.early) ? XSL_VOCAB_APPLIED : XSL_VOCAB_UNSUPPORTED,
                   name + " PTT");
            if (microphone) return;
        }
    }

    void verify_model_discovery(const fs::path& folder, const Json& catalog) {
        const auto entry = std::find_if(catalog.begin(), catalog.end(), [](const Json& model) {
            return model.at("id") == "moonshine-streaming-small";
        });
        if (entry == catalog.end()) throw std::runtime_error("Missing Moonshine Small fixture");
        const auto original = folder / fs::u8path(entry->at("file").get<std::string>());
        const auto fixture = fs::temp_directory_path() / fs::u8path(
            "xsl modèles " + std::to_string(Clock::now().time_since_epoch().count()));
        if (!fs::create_directory(fixture)) throw std::runtime_error("cannot create test directory");
        struct RemoveFixture {
            fs::path path;
            ~RemoveFixture() { std::error_code error; fs::remove_all(path, error); }
        } cleanup{fixture};
        const auto renamed = fixture / "recognized-without-extension";
        fs::copy_file(original, renamed);
        {
            std::ifstream input(original, std::ios::binary);
            std::ofstream output(fixture / "truncated.gguf", std::ios::binary);
            char header[128];
            input.read(header, sizeof(header));
            output.write(header, input.gcount());
        }
        send({{"type", "scan"}, {"path", fixture.u8string()}, {"catalog", catalog}});
        const auto items = message("scan", 10s).at("items");
        bool valid = false, invalid = false;
        for (const auto& item : items) {
            valid |= item.value("id", "") == entry->at("id") && item.value("state", "") == "candidate";
            invalid |= item.value("state", "") == "invalid";
        }
        if (!valid || !invalid) throw std::runtime_error("unexpected renamed/truncated GGUF states");
        send({{"type", "identify"}, {"path", renamed.u8string()}, {"catalog", catalog}});
        const auto result = message("identify", 20s).at("model");
        if (!result.is_object() || result.at("revision") != entry->at("revision"))
            throw std::runtime_error("renamed model changed its identity");
        std::cout << "Renamed and truncated GGUF checks passed\n";
    }

private:
    static bool matches(const Json& entry, const std::string& selection) {
        return selection.empty() ||
               selection == entry.at("id").get<std::string>() + "/" + entry.at("quant").get<std::string>();
    }
    void send(const Json& message) {
        if (!process_.send(message)) throw std::runtime_error("could not send engine command");
    }
    static void check_status(uint32_t status) {
        if (status != XSL_OK) throw std::runtime_error(status_name(status));
    }
    uint64_t command(uint32_t op, uint64_t id = 0, const XslString* terms = nullptr, uint32_t count = 0) {
        XslRequest request{};
        request.version = XSL_ABI_VERSION;
        request.struct_size = sizeof(request);
        request.operation = op;
        request.session = id;
        request.terms = terms;
        request.term_count = count;
        broker_.request(1, request, Clock::now());
        check_status(request.status);
        return request.session;
    }
    void wait_for_capture(uint64_t session) {
        const auto end = Clock::now() + 3s;
        while (Clock::now() < end) {
            poll();
            for (const auto& event : deliveries_) {
                if (event.session == session && event.kind == XSL_CAPTURE_STARTED) return;
                if (event.session == session && event.kind == XSL_RESULT) check_status(event.status);
            }
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("no CAPTURE_STARTED");
    }
    void result(uint64_t session, uint32_t vocabulary, const std::string& name) {
        const auto end = Clock::now() + 22s;
        while (Clock::now() < end) {
            poll();
            for (const auto& event : deliveries_) {
                if (event.session != session || event.kind != XSL_RESULT) continue;
                check_status(event.status);
                if (event.result.at("vocabulary") != vocabulary || event.result.value("text", "").empty())
                    throw std::runtime_error("unexpected result for " + name);
                const auto& candidates = event.result.at("candidates");
                if (!candidates.is_array() || candidates.size() > XSL_MAX_CANDIDATES ||
                    (!candidates.empty() && candidates.front() != event.result.at("text")))
                    throw std::runtime_error("unexpected ranked candidates for " + name);
                std::cout << name << " [" << event.result.value("compute_ms", 0)
                          << " ms, " << event.result.value("language", "") << "] " << event.result.value("text", "") << '\n';
                if (candidates.size() > 1) std::cout << "Candidates: " << candidates.dump() << '\n';
                return;
            }
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("no transcription result for " + name);
    }
    void poll() {
        for (const auto& event : process_.receive()) {
            const auto type = event.value("type", "");
            const auto now = Clock::now();
            if (type == "hello") {
                if (event.value("protocol", 0) != 1) throw std::runtime_error("wrong engine protocol");
                broker_.engine_online(now);
                inbox_.push_back(event);
            } else if (type == "state" || type == "capture" || type == "result" || type == "inference_rejected") {
                broker_.accept(event, now);
            } else if (type != "level" && type != "download_progress") {
                inbox_.push_back(event);
            }
        }
        broker_.tick(Clock::now());
        if (process_.failed()) throw std::runtime_error("engine stopped");
    }
    Process process_;
    Broker broker_;
    std::vector<Json> inbox_;
    std::vector<Delivery> deliveries_;
    std::string injection_;
    Json devices_ = Json::array();
};

int run(const std::vector<fs::path>& args) {
    try {
        const auto option = [&](const char* flag) {
            const auto item = std::find(args.begin(), args.end(), fs::path(flag));
            return item != args.end() && std::next(item) != args.end() ? *std::next(item) : fs::path{};
        };
        const auto engine = option("--engine");
        if (engine.empty()) throw std::runtime_error("--engine is required");
        Check check(engine);
        check.start();
        if (std::find(args.begin(), args.end(), fs::path("--probe")) != args.end()) {
            check.start();
            std::cout << "Engine protocol 1 OK\n";
            return 0;
        }
        if (const auto wav = option("--probe-microphone"); !wav.empty()) {
            check.probe_microphone(wav);
            return 0;
        }
        std::ifstream input(option("--catalog"));
        const auto catalog = Json::parse(input);
        const auto models = option("--models");
        if (models.empty()) throw std::runtime_error("--models is required");
        const auto selection = option("--profile").u8string();
        if (!selection.empty() && std::none_of(catalog.begin(), catalog.end(), [&](const Json& entry) {
            return selection == entry.at("id").get<std::string>() + "/" + entry.at("quant").get<std::string>();
        })) throw std::runtime_error("unknown profile: " + selection);
        if (std::find(args.begin(), args.end(), fs::path("--fetch")) != args.end())
            check.fetch(models, catalog, selection);
        if (std::find(args.begin(), args.end(), fs::path("--verify-files")) != args.end())
            check.verify_model_discovery(models, catalog);
        const auto wav = option("--wav");
        if (!wav.empty()) {
            const auto microphone = std::find(args.begin(), args.end(), fs::path("--mic")) != args.end();
            const auto compute = option("--compute").u8string();
            const auto language = option("--language").u8string();
            const auto width = option("--beam").u8string();
            if (!width.empty() && (width.size() != 1 || width[0] < '1' || width[0] > '4'))
                throw std::runtime_error("--beam expects 1, 2, 3 or 4");
            check.transcribe_models(models, catalog, wav, selection, microphone,
                                    compute.empty() ? "cpu" : compute, language.empty() ? "auto" : language,
                                    width.empty() ? 1 : width[0] - '0');
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    return run(std::vector<xsl::fs::path>(argv + 1, argv + argc));
}
