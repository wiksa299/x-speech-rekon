#include "audio.hpp"
#include "desktop.hpp"
#include "models.hpp"
#ifdef XSR_MOONSHINE
#include "moonshine.hpp"
#endif

#include <transcribe.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace xsr {
namespace {

class Engine {
public:
    explicit Engine(const fs::path& executable)
        : whisper_path_(executable.parent_path() / "whisper" / fs::path("xsr-whisper").concat(executable.extension().native())) {}
    ~Engine() { unload(); }

    int run() {
        Json devices = Json::array();
        for (int i = 0; i < transcribe_device_count(); ++i) {
            const auto device = transcribe_device_get(i);
            transcribe_device_info info{};
            transcribe_device_info_init(&info);
            if (transcribe_device_get_info(device, &info) != TRANSCRIBE_OK || !info.kind ||
                std::string_view(info.kind) != "vulkan" || info.device_type == TRANSCRIBE_DEVICE_TYPE_CPU) continue;
            const auto id = std::string("vulkan/") + (info.device_id && *info.device_id ? info.device_id : info.name);
            const auto label = std::string("Vulkan · ") + (info.description && *info.description ? info.description : info.name);
            vulkan_devices_.emplace(id, ComputeDevice{device, label, info.description ? info.description : ""});
            devices.push_back({{"id", id}, {"label", label}});
        }
        Json hello = {{"type", "hello"}, {"protocol", 1}, {"version", transcribe_version()}, {"devices", std::move(devices)}};
        std::error_code file_error;
        if (fs::is_regular_file(whisper_path_, file_error)) hello["whisper_version"] = XSR_WHISPER_VERSION;
#ifdef XSR_MOONSHINE
        hello["moonshine_models"] = moonshine_catalog();
        hello["moonshine_version"] = XSR_MOONSHINE_VERSION;
#endif
        publish(hello);
        std::thread inference([&] { inference_loop(); });
        std::thread files([&] { file_loop(); });
        std::thread downloads([&] { download_loop(); });
        Json message;
        while (read_frame(std::cin, message)) {
            const auto type = message.value("type", "");
            if (type == "shutdown") break;
            auto rejection = enqueue(type, std::move(message));
            if (!rejection.is_null()) publish(rejection);
        }
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            download_cancelled_ = true;
            if (active_) active_->cancelled = true;
        }
        available_.notify_all();
        inference.join();
        files.join();
        downloads.join();
        return 0;
    }

private:
    struct Control {
        explicit Control(uint64_t session) : id(session) {}
        ~Control() { stop_capture(); }
        void stop_capture() {
            released = true;
            if (capture.joinable()) capture.join();
        }
        uint64_t id;
        std::atomic<bool> released{false};
        std::atomic<bool> cancelled{false};
        Recording recording;
        std::thread capture;
    };
    struct Job { Json request; std::shared_ptr<Control> control; };

    Json enqueue(const std::string& type, Json message) {
        std::lock_guard lock(mutex_);
        if (type == "begin") {
            const auto session = message.at("session").get<uint64_t>();
            if (active_) return {{"type", "result"}, {"session", session}, {"status", XSR_BUSY}};
            active_ = std::make_shared<Control>(session);
            if (message.value("wav", "").empty()) start_capture(message, *active_);
            inference_.push_back({std::move(message), active_});
        } else if (type == "end" || type == "cancel") {
            if (active_ && active_->id == message.at("session").get<uint64_t>()) {
                if (type == "end") active_->released = true;
                else active_->cancelled = true;
            }
        } else if (type == "cancel_download") {
            download_cancelled_ = true;
        } else if (type == "download") {
            if (download_active_) return file_error(type, message, "busy");
            download_cancelled_ = false;
            download_active_ = true;
            download_job_ = std::move(message);
        } else if (type == "scan" || type == "identify" || type == "browse" || type == "browse_wav") {
            if (file_jobs_.size() >= 8) return file_error(type, message, "busy");
            file_jobs_.push_back(std::move(message));
        } else if (type == "configure" || type == "unload" || type == "microphones") {
            // A begin or unload is a barrier between distinct configurations.
            if (!inference_.empty() && inference_.back().request.value("type", "") == type) {
                inference_.back().request = std::move(message);
            } else if (inference_.size() < 16) {
                inference_.push_back({std::move(message), {}});
            } else if (type == "microphones") {
                return {{"type", type}, {"items", Json::array()}, {"status", "busy"}};
            } else {
                return {{"type", "inference_rejected"}, {"operation", type}, {"generation", message.at("generation")}};
            }
        }
        available_.notify_all();
        return {};
    }

    static Json file_error(const std::string& type, const Json& request, const char* status) {
        return {{"type", type}, {"ticket", request.value("ticket", uint64_t(0))}, {"status", status}};
    }

    void publish(const Json& event) {
        auto text = wire_frame(event);
        if (text.empty() && event.contains("ticket"))
            text = wire_frame(file_error(event.at("type"), event, "too_large"));
        std::lock_guard lock(output_);
        if (!text.empty()) std::cout << text << std::flush;
    }

    void state(uint32_t value) {
        Json message{{"type", "state"}, {"generation", generation_}, {"state", value}};
        if (value == XSR_MODEL_READY) {
            message["compute"] = actual_compute_;
            message["cpu_fallback"] = requested_compute_ != "cpu" && actual_compute_ == "CPU";
        }
        publish(message);
    }

    bool loaded() const {
#ifdef XSR_MOONSHINE
        if (moonshine_ != nullptr) return true;
#endif
        return model_ != nullptr || whisper_ != nullptr;
    }

    void unload() {
#ifdef XSR_MOONSHINE
        moonshine_.reset();
#endif
        model_.reset();
        whisper_.reset();
    }

    bool load() {
        if (loaded()) return true;
        if (path_.empty()) return false;
        state(XSR_MODEL_LOADING);
#ifdef XSR_MOONSHINE
        if (const auto* bundle = moonshine_bundle(sha256_)) {
            const auto path = fs::u8path(path_);
            if (matches_bundle(path, *bundle, stopping_))
                moonshine_.reset(xsr_moonshine_load(path.c_str(), bundle->at("id") == "moonshine-voice-medium"));
            actual_compute_ = "CPU";
            state(loaded() ? XSR_MODEL_READY : XSR_MODEL_MISSING);
            return loaded();
        }
#endif
        if (!matches_digest(fs::u8path(path_), sha256_, stopping_)) {
            state(XSR_MODEL_MISSING);
            return false;
        }
        if (model_id_ == whisper_cpp_id) {
            whisper_ = load_whisper(requested_compute_);
            // A driver failure can terminate the worker during model loading.
            if (!whisper_ && requested_compute_ != "cpu" && !stopping_) whisper_ = load_whisper("cpu");
            state(loaded() ? XSR_MODEL_READY : XSR_MODEL_MISSING);
            return loaded();
        }
        transcribe_model_load_params params{};
        transcribe_model_load_params_init(&params);
        transcribe_model* candidate = nullptr;
        const auto gpu = vulkan_devices_.find(requested_compute_);
        if (gpu != vulkan_devices_.end()) {
            params.backend = TRANSCRIBE_BACKEND_VULKAN;
            params.device = gpu->second.handle;
            if (transcribe_model_load_file(path_.c_str(), &params, &candidate) == TRANSCRIBE_OK)
                actual_compute_ = gpu->second.label;
        }
        if (!candidate) {
            params.backend = TRANSCRIBE_BACKEND_CPU;
            params.device = nullptr;
            if (transcribe_model_load_file(path_.c_str(), &params, &candidate) != TRANSCRIBE_OK) {
                state(XSR_MODEL_MISSING);
                return false;
            }
            actual_compute_ = "CPU";
        }
        model_.reset(candidate);
        state(XSR_MODEL_READY);
        return true;
    }

    void inference_loop() {
        while (true) {
            Job next;
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, [&] { return stopping_ || !inference_.empty(); });
                if (stopping_) return;
                next = std::move(inference_.front());
                inference_.pop_front();
            }
            const auto type = next.request.value("type", "");
            if (type == "configure") {
                auto path = next.request.value("path", "");
                auto sha256 = next.request.value("sha256", "");
                auto compute = next.request.value("compute", "cpu");
                auto model = next.request.value("model", "");
                if (path != path_ || sha256 != sha256_ || compute != requested_compute_ || model != model_id_) unload();
                path_ = std::move(path);
                sha256_ = std::move(sha256);
                requested_compute_ = std::move(compute);
                model_id_ = std::move(model);
                generation_ = next.request.value("generation", uint64_t(0));
                if (path_.empty()) state(XSR_MODEL_MISSING);
                else if (loaded()) state(XSR_MODEL_READY);
                else if (next.request.value("preload", false)) load();
                else state(XSR_MODEL_UNLOADED);
            } else if (type == "unload" && next.request.value("generation", uint64_t(0)) == generation_) {
                unload();
                state(path_.empty() ? XSR_MODEL_MISSING : XSR_MODEL_UNLOADED);
            } else if (type == "microphones") {
                publish({{"type", "microphones"}, {"items", microphone_list()}});
            } else if (type == "begin") {
                Json response = transcribe(next.request, *next.control);
                next.control->stop_capture();
                {
                    std::lock_guard lock(mutex_);
                    active_.reset();
                }
                response["type"] = "result";
                response["session"] = next.control->id;
                publish(response);
            }
        }
    }

    void file_loop() {
        while (true) {
            Json next;
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, [&] { return stopping_ || !file_jobs_.empty(); });
                if (stopping_) return;
                next = std::move(file_jobs_.front());
                file_jobs_.pop_front();
            }
            const auto type = next.value("type", "");
            const auto ticket = next.value("ticket", uint64_t(0));
            try {
                if (type == "scan" || type == "identify") {
                    const auto path = fs::u8path(next.at("path").get<std::string>());
                    const auto& catalog = next.at("catalog");
                    if (type == "scan")
                        publish({{"type", type}, {"ticket", ticket}, {"status", "ok"},
                                 {"items", scan_models(path, catalog, stopping_)}});
                    else
                        publish({{"type", type}, {"ticket", ticket}, {"status", "ok"},
                                 {"model", identify_model(path, catalog, stopping_)}});
                } else {
                    const auto selected = browse_path(type == "browse", stopping_);
                    publish({{"type", type}, {"ticket", ticket}, {"path", selected.path},
                             {"status", selected.failed ? "error" : "ok"}});
                }
            } catch (const std::exception&) {
                publish(file_error(type, next, "error"));
            }
        }
    }

    void download_loop() {
        for (;;) {
            Json next;
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, [&] { return stopping_ || download_job_.has_value(); });
                if (stopping_) return;
                next = std::move(*download_job_);
                download_job_.reset();
            }
            const auto ticket = next.value("ticket", uint64_t(0));
            std::string status = "error";
            try {
                const auto path = fs::u8path(next.at("path").get<std::string>());
                const auto progress = [&](uint64_t bytes) {
                    publish({{"type", "download_progress"}, {"ticket", ticket}, {"bytes", bytes}});
                };
                status = download_model(path, next.at("entry"), download_cancelled_, progress);
            } catch (const std::exception&) {}
            {
                std::lock_guard lock(mutex_);
                download_active_ = false;
            }
            publish({{"type", "download"}, {"ticket", ticket}, {"status", status}});
        }
    }

    void start_capture(const Json& request, Control& control) {
        const auto microphone = request.value("microphone", "");
        const auto injection = fs::u8path(request.value("injection", ""));
        const auto gain_percent = request.value("sidetone", false) ?
            std::min(request.value("monitor_gain_percent", XSR_DEFAULT_MONITOR_GAIN_PERCENT),
                     XSR_MAX_MONITOR_GAIN_PERCENT) : 0u;
        const auto monitor_gain = static_cast<float>(gain_percent) / 100.0f;
        control.capture = std::thread([this, &control, microphone, injection, monitor_gain] {
            record(control.recording, microphone, injection, monitor_gain, control.released, control.cancelled,
                [&](float level, bool started) {
                    publish({{"type", started ? "capture" : "level"}, {"session", control.id}, {"level", level}});
                });
        });
    }

    Json transcribe(const Json& request, Control& control);
    Json transcribe_whisper(const Json& request, Control& control);
    std::unique_ptr<Process> load_whisper(const std::string& compute);
    Json whisper_reply(Process& process, const std::atomic<bool>& cancelled, Clock::duration timeout) const;
#ifdef XSR_MOONSHINE
    Json transcribe_moonshine(const Json& request, Control& control);
    std::unique_ptr<MoonshineTranscriber, decltype(&xsr_moonshine_free)> moonshine_{nullptr, xsr_moonshine_free};
#endif

    std::mutex mutex_, output_;
    std::condition_variable available_;
    std::deque<Job> inference_;
    std::deque<Json> file_jobs_;
    std::optional<Json> download_job_;
    bool download_active_ = false;
    std::shared_ptr<Control> active_;
    std::atomic<bool> stopping_{false}, download_cancelled_{false};
    struct ComputeDevice {
        transcribe_device_t handle;
        std::string label;
        std::string description;
    };
    std::map<std::string, ComputeDevice> vulkan_devices_;
    std::unique_ptr<transcribe_model, decltype(&transcribe_model_free)> model_{nullptr, transcribe_model_free};
    fs::path whisper_path_;
    std::unique_ptr<Process> whisper_;
    std::string path_, sha256_, model_id_;
    std::string requested_compute_ = "cpu", actual_compute_;
    uint64_t generation_ = 0;
};

Json Engine::whisper_reply(Process& process, const std::atomic<bool>& cancelled, Clock::duration timeout) const {
    const auto deadline = Clock::now() + timeout;
    while (!stopping_ && !cancelled && Clock::now() < deadline) {
        auto messages = process.receive();
        if (!messages.empty()) return std::move(messages.front());
        if (process.failed()) break;
        std::this_thread::sleep_for(5ms);
    }
    return Json::object();
}

std::unique_ptr<Process> Engine::load_whisper(const std::string& compute) {
    const auto device = vulkan_devices_.find(compute);
    const auto requested = device == vulkan_devices_.end() ? "cpu" : compute;
    auto process = std::make_unique<Process>(whisper_path_);
    if (!process->start() ||
        !process->send({{"type", "load"}, {"path", path_}, {"compute", requested},
                        {"description", device == vulkan_devices_.end() ? "" : device->second.description}})) return {};
    const auto reply = whisper_reply(*process, stopping_, 8s);
    if (reply.value("type", "") != "loaded" || !reply.value("ok", false)) return {};
    actual_compute_ = device != vulkan_devices_.end() && reply.value("gpu", false) ? device->second.label : "CPU";
    return process;
}

Json Engine::transcribe_whisper(const Json& request, Control& control) {
    const auto error = [](uint32_t status) { return Json{{"status", status}}; };
    const auto& recording = control.recording;
    const bool capture = request.value("wav", "").empty();
    while (capture && !recording.closed && !control.cancelled)
        std::this_thread::sleep_for(10ms);
    if (control.cancelled) return error(XSR_CANCELLED);
    if (recording.error != XSR_OK) return error(recording.error);
    const auto count = recording.count.load(std::memory_order_acquire);
    if (!contains_audio(recording.samples.data(), count)) return error(XSR_NO_SPEECH);

    bool sent = true;
    const auto* bytes = reinterpret_cast<const unsigned char*>(recording.samples.data());
    for (size_t offset = 0; offset < count && sent && !control.cancelled; offset += whisper_chunk_samples) {
        const auto end = std::min(count, offset + whisper_chunk_samples) * sizeof(float);
        std::string pcm;
        pcm.reserve((end - offset * sizeof(float)) * 2);
        constexpr char hex[] = "0123456789abcdef";
        for (size_t i = offset * sizeof(float); i < end; ++i) {
            pcm += hex[bytes[i] >> 4];
            pcm += hex[bytes[i] & 15];
        }
        sent = whisper_->send({{"type", "audio"}, {"pcm", std::move(pcm)}});
    }
    if (sent && !control.cancelled && whisper_->send({{"type", "decode"}, {"terms", request.at("terms")}})) {
        auto reply = whisper_reply(*whisper_, control.cancelled, 30s);
        if (reply.value("type", "") == "result" && !control.cancelled) return reply;
    }
    unload();
    state(XSR_MODEL_UNLOADED);
    return error(control.cancelled ? XSR_CANCELLED : XSR_ENGINE_ERROR);
}

Json Engine::transcribe(const Json& request, Control& control) {
    const auto error = [](uint32_t status) { return Json{{"status", status}}; };
    if (request.value("generation", uint64_t(0)) != generation_) return error(XSR_MODEL_UNAVAILABLE);
    if (control.cancelled) return error(XSR_CANCELLED);
    const auto sample = request.value("wav", "");
    auto& recording = control.recording;
    if (sample.empty()) {
        while (!recording.count && !recording.closed && !control.cancelled && !control.released)
            std::this_thread::sleep_for(5ms);
        if (!recording.count) {
            if (control.cancelled) return error(XSR_CANCELLED);
            return error(recording.error != XSR_OK ? uint32_t(recording.error) : XSR_NO_AUDIO);
        }
    } else {
        recording.samples = read_wav(fs::u8path(sample));
        recording.count = recording.samples.size();
        if (!recording.count) return error(XSR_BAD_REQUEST);
    }

    if (!load()) return error(XSR_MODEL_UNAVAILABLE);
    if (control.cancelled) return error(XSR_CANCELLED);
    if (whisper_) return transcribe_whisper(request, control);
#ifdef XSR_MOONSHINE
    if (moonshine_ != nullptr) return transcribe_moonshine(request, control);
#endif
    transcribe_session_params session_parameters{};
    transcribe_session_params_init(&session_parameters);
    const auto cores = std::max(2u, std::thread::hardware_concurrency());
    session_parameters.n_threads = int(std::min(6u, cores / 2));
    transcribe_session* raw_session = nullptr;
    if (transcribe_session_init(model_.get(), &session_parameters, &raw_session) != TRANSCRIBE_OK)
        return error(XSR_ENGINE_ERROR);
    std::unique_ptr<transcribe_session, decltype(&transcribe_session_free)> session(raw_session, transcribe_session_free);
    transcribe_set_abort_callback(session.get(), +[](void* pointer) {
        return static_cast<Control*>(pointer)->cancelled.load();
    }, &control);

    auto terms = request.value("terms", std::vector<std::string>{});
    std::vector<const char*> pointers;
    for (const auto& term : terms) pointers.push_back(term.c_str());
    uint32_t vocabulary = XSR_VOCAB_NONE;
    transcribe_run_params params{};
    transcribe_run_params_init(&params);
    const bool streaming = request.value("early", false);
    const auto language_hint = request.value("language", "");
    if (!language_hint.empty() && language_hint != "auto") params.language = language_hint.c_str();
    const auto context = request.value("context", "");
    if (!streaming && !context.empty() && transcribe_model_supports(model_.get(), TRANSCRIBE_FEATURE_CONTEXT_PROMPT))
        params.prompt = context.c_str();
    if (!pointers.empty()) {
        const bool supported = !streaming && transcribe_model_supports(model_.get(), TRANSCRIBE_FEATURE_VOCABULARY);
        vocabulary = supported ? XSR_VOCAB_APPLIED : XSR_VOCAB_UNSUPPORTED;
        if (supported) {
            params.vocabulary = pointers.data();
            params.n_vocabulary = int32_t(pointers.size());
        }
    }

    Clock::duration compute_time{};
    const auto timed = [&](auto operation) {
        const auto start = Clock::now();
        const auto result = operation();
        compute_time += Clock::now() - start;
        return result;
    };
    if (streaming && timed([&] { return transcribe_stream_begin(session.get(), &params, nullptr); }) != TRANSCRIBE_OK)
        return error(XSR_ENGINE_ERROR);
    size_t fed = 0;
    auto status = TRANSCRIBE_OK;
    while (sample.empty() && !recording.closed && !control.cancelled && status == TRANSCRIBE_OK) {
        const auto count = recording.count.load(std::memory_order_acquire);
        if (streaming && count - fed >= sample_rate / 4) {
            status = timed([&] {
                return transcribe_stream_feed(session.get(), recording.samples.data() + fed, int(count - fed), nullptr);
            });
            fed = count;
        } else {
            std::this_thread::sleep_for(10ms);
        }
    }
    if (control.cancelled) return error(XSR_CANCELLED);
    if (recording.error != XSR_OK) return error(recording.error);
    const auto count = recording.count.load(std::memory_order_acquire);
    if (!contains_audio(recording.samples.data(), count)) return error(XSR_NO_SPEECH);
    if (streaming) {
        if (status == TRANSCRIBE_OK && count > fed) {
            status = timed([&] {
                return transcribe_stream_feed(session.get(), recording.samples.data() + fed, int(count - fed), nullptr);
            });
        }
        if (status == TRANSCRIBE_OK)
            status = timed([&] { return transcribe_stream_finalize(session.get(), nullptr); });
    } else {
        status = timed([&] { return transcribe_run(session.get(), recording.samples.data(), int(count), &params); });
    }
    if (control.cancelled) return error(XSR_CANCELLED);
    if (status == TRANSCRIBE_ERR_OUTPUT_TRUNCATED || transcribe_was_truncated(session.get()))
        return error(XSR_OUTPUT_TRUNCATED);
    if (status != TRANSCRIBE_OK) return error(XSR_ENGINE_ERROR);
    std::string text = transcribe_full_text(session.get());
    if (text.find_first_not_of(" \r\t\n") == std::string::npos) return error(XSR_NO_SPEECH);
    if (text.size() > max_result_size) return error(XSR_OUTPUT_TRUNCATED);
    if (!valid_utf8(text)) return error(XSR_ENGINE_ERROR);
    const auto* language = transcribe_detected_language(session.get());
    return {{"status", XSR_OK}, {"text", std::move(text)},
            {"language", language && *language ? language : params.language ? params.language : ""},
            {"vocabulary", vocabulary}, {"capture_ms", count * 1000 / sample_rate},
            {"compute_ms", std::chrono::duration_cast<std::chrono::milliseconds>(compute_time).count()}};
}

#ifdef XSR_MOONSHINE
Json Engine::transcribe_moonshine(const Json& request, Control& control) {
    const auto error = [](uint32_t status) { return Json{{"status", status}}; };
    auto& recording = control.recording;
    const bool capture = request.value("wav", "").empty();
    while (capture && !recording.closed && !control.cancelled)
        std::this_thread::sleep_for(10ms);
    if (control.cancelled) return error(XSR_CANCELLED);
    if (recording.error != XSR_OK) return error(recording.error);
    const auto count = recording.count.load(std::memory_order_acquire);
    if (!contains_audio(recording.samples.data(), count)) return error(XSR_NO_SPEECH);

    const auto terms = request.value("terms", std::vector<std::string>{});
    const bool encodable = std::none_of(terms.begin(), terms.end(), [](const auto& term) {
        return term.find(',') != std::string::npos || term.find('\0') != std::string::npos;
    });
    std::string csv;
    if (encodable) {
        for (const auto& term : terms) {
            if (!csv.empty()) csv += ',';
            csv += term;
        }
    }
    const auto start = Clock::now();
    // Replace the previous client's terms even when this request has no vocabulary.
    if (!xsr_moonshine_terms(moonshine_.get(), csv.empty() ? nullptr : csv.c_str())) return error(XSR_ENGINE_ERROR);
    std::vector<char> output(max_result_size + 1);
    const auto status = xsr_moonshine_decode(moonshine_.get(), recording.samples.data(), count, sample_rate,
                                             output.data(), output.size());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    if (control.cancelled) return error(XSR_CANCELLED);
    if (status == MoonshineResult::truncated) return error(XSR_OUTPUT_TRUNCATED);
    if (status != MoonshineResult::ok) return error(XSR_ENGINE_ERROR);
    std::string text(output.data());
    if (text.find_first_not_of(" \r\t\n") == std::string::npos) return error(XSR_NO_SPEECH);
    if (!valid_utf8(text)) return error(XSR_ENGINE_ERROR);
    const auto vocabulary = terms.empty() ? XSR_VOCAB_NONE :
        csv.empty() ? XSR_VOCAB_UNSUPPORTED : XSR_VOCAB_APPLIED;
    return {{"status", XSR_OK}, {"text", std::move(text)}, {"language", "en"},
            {"vocabulary", vocabulary}, {"capture_ms", count * 1000 / sample_rate},
            {"compute_ms", elapsed.count()}};
}
#endif

} // namespace
} // namespace xsr

int main(int argc, char** argv) {
    if (argc < 1) return 1;
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        if (transcribe_init_backends_default() != TRANSCRIBE_OK) return 1;
        return xsr::Engine{xsr::executable_path(argv[0])}.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
