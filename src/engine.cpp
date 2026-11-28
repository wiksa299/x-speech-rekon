#include "audio.hpp"
#include "desktop.hpp"
#include "fluid_weights.hpp"
#include "models.hpp"

#include <transcribe.h>
#include <transcribe/listen.h>

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

namespace xsl {
namespace {

class Engine {
public:
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
            vulkan_devices_.emplace(id, ComputeDevice{device, label});
            devices.push_back({{"id", id}, {"label", label}});
        }
        Json hello = {{"type", "hello"}, {"protocol", 1}, {"version", transcribe_version()},
                      {"devices", std::move(devices)}};
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
            microphone_playing_ = 0;
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
        explicit Control(uint64_t session, bool microphone_test) : id(session), recording(microphone_test) {}
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
            if (active_) return {{"type", "result"}, {"session", session}, {"status", XSL_BUSY}};
            const bool microphone_test = message.value("microphone_test", false);
            microphone_playing_ = 0;
            if (microphone_test) microphone_take_ = session;
            active_ = std::make_shared<Control>(session, microphone_test);
            if (message.value("wav", "").empty()) start_capture(message, *active_);
            inference_.push_back({std::move(message), active_});
        } else if (type == "end" || type == "cancel") {
            if (active_ && active_->id == message.at("session").get<uint64_t>()) {
                if (type == "end") active_->released = true;
                else active_->cancelled = true;
            }
        } else if (type == "mic_play") {
            const auto session = message.at("session").get<uint64_t>();
            if (!session || active_ || microphone_playing_ || microphone_take_ != session || inference_.size() >= 16)
                return {{"type", "mic_playback"}, {"session", session}, {"ok", false}};
            microphone_playing_ = session;
            inference_.push_back({std::move(message), {}});
        } else if (type == "mic_stop" || type == "mic_clear") {
            const auto session = message.at("session").get<uint64_t>();
            if (microphone_playing_ == session) microphone_playing_ = 0;
            if (type == "mic_clear" && microphone_take_ == session) microphone_take_ = 0;
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
        if (value == XSL_MODEL_READY) {
            message["compute"] = actual_compute_;
            message["cpu_fallback"] = requested_compute_ != "cpu" && actual_compute_ == "CPU";
        }
        publish(message);
    }

    bool loaded() const {
        return model_ != nullptr;
    }

    void unload() {
        model_.reset();
    }

    bool load() {
        if (loaded()) return true;
        if (path_.empty()) return false;
        state(XSL_MODEL_LOADING);
        if (!matches_digest(fs::u8path(path_), sha256_, stopping_)) {
            state(XSL_MODEL_MISSING);
            return false;
        }
        std::string inference_path = path_;
        if (Profile{model_id_, {}, {}, {}, {}}.fluid()) {
            if (!fluid_weights_.prepare(model_id_, fs::u8path(path_), sha256_, stopping_)) {
                std::cerr << "Could not prepare verified Fluid weights\n";
                state(XSL_MODEL_MISSING);
                return false;
            }
            inference_path = fluid_weights_.path().u8string();
        }
        transcribe_model_load_params params{};
        transcribe_model_load_params_init(&params);
        transcribe_model* candidate = nullptr;
        const auto gpu = vulkan_devices_.find(requested_compute_);
        if (gpu != vulkan_devices_.end()) {
            params.backend = TRANSCRIBE_BACKEND_VULKAN;
            params.device = gpu->second.handle;
            if (transcribe_model_load_file(inference_path.c_str(), &params, &candidate) == TRANSCRIBE_OK)
                actual_compute_ = gpu->second.label;
        }
        if (!candidate) {
            params.backend = TRANSCRIBE_BACKEND_CPU;
            params.device = nullptr;
            if (transcribe_model_load_file(inference_path.c_str(), &params, &candidate) != TRANSCRIBE_OK) {
                state(XSL_MODEL_MISSING);
                return false;
            }
            actual_compute_ = "CPU";
        }
        model_.reset(candidate);
        state(XSL_MODEL_READY);
        return true;
    }

    void inference_loop() {
        while (true) {
            Job next;
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, [&] { return stopping_ || !inference_.empty() ||
                    (stored_take_ && stored_take_ != microphone_take_); });
                if (stopping_) return;
                if (stored_take_ && stored_take_ != microphone_take_) {
                    std::vector<float>().swap(take_samples_);
                    stored_take_ = 0;
                }
                if (inference_.empty()) continue;
                next = std::move(inference_.front());
                inference_.pop_front();
            }
            const auto type = next.request.value("type", "");
            if (type == "configure") {
                auto path = next.request.value("path", "");
                auto sha256 = next.request.value("sha256", "");
                auto compute = next.request.value("compute", "cpu");
                auto model = next.request.value("model", "");
                if (!Profile{model, {}, {}, {}, {}}.supported()) path.clear();
                if (path != path_ || sha256 != sha256_ || compute != requested_compute_ || model != model_id_) unload();
                if (path != path_ || sha256 != sha256_ || model != model_id_) fluid_weights_.reset();
                path_ = std::move(path);
                sha256_ = std::move(sha256);
                requested_compute_ = std::move(compute);
                cpu_threads_ = next.request.value("cpu_threads", 0);
                model_id_ = std::move(model);
                generation_ = next.request.value("generation", uint64_t(0));
                if (path_.empty()) state(XSL_MODEL_MISSING);
                else if (loaded()) state(XSL_MODEL_READY);
                else if (next.request.value("preload", false)) load();
                else state(XSL_MODEL_UNLOADED);
            } else if (type == "unload" && next.request.value("generation", uint64_t(0)) == generation_) {
                unload();
                state(path_.empty() ? XSL_MODEL_MISSING : XSL_MODEL_UNLOADED);
            } else if (type == "microphones") {
                publish({{"type", "microphones"}, {"items", microphone_list()}});
            } else if (type == "mic_play") {
                const auto session = next.request.at("session").get<uint64_t>();
                const bool ok = microphone_playing_ != session ||
                    (stored_take_ == session && play_microphone_take(take_samples_, session, microphone_playing_));
                auto expected = session;
                microphone_playing_.compare_exchange_strong(expected, 0);
                publish({{"type", "mic_playback"}, {"session", session}, {"ok", ok}});
            } else if (type == "begin") {
                Json response = next.control->recording.microphone_test ? measure_microphone(*next.control) :
                    transcribe(next.request, *next.control);
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
                                 {"items", scan_models(path, catalog, stopping_, next.value("require_directory", false))}});
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

    Json measure_microphone(Control& control) {
        auto& recording = control.recording;
        while (!recording.closed && !control.cancelled) std::this_thread::sleep_for(5ms);
        control.stop_capture();
        const auto count = recording.count.load(std::memory_order_acquire);
        uint32_t status = recording.error;
        if (control.cancelled) status = XSL_CANCELLED;
        else if (status == XSL_OK && !count) status = XSL_NO_AUDIO;
        Json result = {{"status", status}, {"microphone_test", true}};
        if (status != XSL_OK) return result;
        result.update(microphone_measurement(recording.samples.data(), count));
        if (microphone_take_ == control.id) {
            recording.samples.resize(count);
            take_samples_ = std::move(recording.samples);
            stored_take_ = control.id;
        }
        return result;
    }

    void start_capture(const Json& request, Control& control) {
        const auto microphone = request.value("microphone", "");
        const auto injection = fs::u8path(request.value("injection", ""));
        const auto gain_percent = request.value("sidetone", false) ?
            std::min(request.value("monitor_gain_percent", XSL_DEFAULT_MONITOR_GAIN_PERCENT),
                     XSL_MAX_MONITOR_GAIN_PERCENT) : 0u;
        const auto monitor_gain = static_cast<float>(gain_percent) / 100.0f;
        const bool low_latency = request.value("low_latency_monitoring", false);
        control.capture = std::thread([this, &control, microphone, injection, monitor_gain, low_latency] {
            record(control.recording, microphone, injection, monitor_gain, low_latency, control.released, control.cancelled,
                [&](float level, bool started, size_t count, const std::string& name, const CaptureMonitoring& monitoring) {
                    Json event = {{"type", started ? "capture" : "level"}, {"session", control.id}, {"level", level}};
                    if (control.recording.microphone_test) {
                        event["capture_ms"] = count * 1000 / sample_rate;
                        if (started) event["microphone"] = name;
                    }
                    if (started && monitoring.mode) {
                        event["monitoring"] = {{"mode", monitoring.mode}, {"fallback", monitoring.fallback},
                            {"stream_rate", monitoring.stream_rate}, {"output_rate", monitoring.output_rate},
                            {"output_period_frames", monitoring.output_period_frames}};
                    }
                    publish(event);
                });
        });
    }

    Json transcribe(const Json& request, Control& control);

    std::mutex mutex_, output_;
    std::condition_variable available_;
    std::deque<Job> inference_;
    std::deque<Json> file_jobs_;
    std::optional<Json> download_job_;
    bool download_active_ = false;
    std::shared_ptr<Control> active_;
    std::atomic<bool> stopping_{false}, download_cancelled_{false};
    std::atomic<uint64_t> microphone_take_{0}, microphone_playing_{0};
    std::vector<float> take_samples_;
    uint64_t stored_take_ = 0;
    struct ComputeDevice {
        transcribe_device_t handle;
        std::string label;
    };
    std::map<std::string, ComputeDevice> vulkan_devices_;
    FluidWeights fluid_weights_;
    std::unique_ptr<transcribe_model, decltype(&transcribe_model_free)> model_{nullptr, transcribe_model_free};
    std::string path_, sha256_, model_id_;
    std::string requested_compute_ = "cpu", actual_compute_;
    int cpu_threads_ = 0;
    uint64_t generation_ = 0;
};

Json Engine::transcribe(const Json& request, Control& control) {
    const auto error = [](uint32_t status) { return Json{{"status", status}}; };
    if (request.value("generation", uint64_t(0)) != generation_) return error(XSL_MODEL_UNAVAILABLE);
    if (control.cancelled) return error(XSL_CANCELLED);
    const auto sample = request.value("wav", "");
    auto& recording = control.recording;
    if (sample.empty()) {
        while (!recording.count && !recording.closed && !control.cancelled && !control.released)
            std::this_thread::sleep_for(5ms);
        if (!recording.count) {
            if (control.cancelled) return error(XSL_CANCELLED);
            return error(recording.error != XSL_OK ? uint32_t(recording.error) : XSL_NO_AUDIO);
        }
    } else {
        recording.samples = read_wav(fs::u8path(sample));
        recording.count = recording.samples.size();
        if (!recording.count) return error(XSL_BAD_REQUEST);
    }

    if (!load()) return error(XSL_MODEL_UNAVAILABLE);
    if (control.cancelled) return error(XSL_CANCELLED);
    transcribe_session_params session_parameters{};
    transcribe_session_params_init(&session_parameters);
    const auto cores = std::max(1u, std::thread::hardware_concurrency());
    session_parameters.n_threads = requested_compute_ == "cpu" && cpu_threads_ > 0 ?
        std::min(cpu_threads_, static_cast<int>(cores)) : default_cpu_threads(cores);
    transcribe_session* raw_session = nullptr;
    if (transcribe_session_init(model_.get(), &session_parameters, &raw_session) != TRANSCRIBE_OK)
        return error(XSL_ENGINE_ERROR);
    std::unique_ptr<transcribe_session, decltype(&transcribe_session_free)> session(raw_session, transcribe_session_free);
    transcribe_set_abort_callback(session.get(), +[](void* pointer) {
        return static_cast<Control*>(pointer)->cancelled.load();
    }, &control);

    auto terms = request.value("terms", std::vector<std::string>{});
    std::vector<const char*> pointers;
    for (const auto& term : terms) pointers.push_back(term.c_str());
    uint32_t vocabulary = XSL_VOCAB_NONE;
    transcribe_run_params params{};
    transcribe_run_params_init(&params);
    const bool streaming = request.value("early", false);
    transcribe_listen_decode_params decode_options{};
    transcribe_listen_decode_params_init(&decode_options);
    decode_options.beam_width = std::clamp(request.value("beam_width", 1), 1, int(XSL_MAX_CANDIDATES));
    decode_options.vocabulary_boost = std::clamp(request.value("vocabulary_weight", -1.0f), -1.0f, 4.0f);
    if (!streaming && (decode_options.beam_width > 1 || decode_options.vocabulary_boost >= 0.0f))
        params.family = &decode_options.ext;
    const auto language_hint = request.value("language", "");
    if (!language_hint.empty() && language_hint != "auto") params.language = language_hint.c_str();
    if (!pointers.empty() && decode_options.vocabulary_boost != 0.0f) {
        const bool supported = !streaming && transcribe_model_supports(model_.get(), TRANSCRIBE_FEATURE_VOCABULARY);
        vocabulary = supported ? XSL_VOCAB_APPLIED : XSL_VOCAB_UNSUPPORTED;
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
        return error(XSL_ENGINE_ERROR);
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
    if (control.cancelled) return error(XSL_CANCELLED);
    if (recording.error != XSL_OK) return error(recording.error);
    const auto count = recording.count.load(std::memory_order_acquire);
    if (!contains_audio(recording.samples.data(), count)) return error(XSL_NO_SPEECH);
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
    if (control.cancelled) return error(XSL_CANCELLED);
    if (status == TRANSCRIBE_ERR_OUTPUT_TRUNCATED || transcribe_was_truncated(session.get()))
        return error(XSL_OUTPUT_TRUNCATED);
    if (status != TRANSCRIBE_OK) return error(XSL_ENGINE_ERROR);
    std::string text = transcribe_full_text(session.get());
    if (text.find_first_not_of(" \r\t\n") == std::string::npos) return error(XSL_NO_SPEECH);
    if (text.size() > max_result_size) return error(XSL_OUTPUT_TRUNCATED);
    if (!valid_utf8(text)) return error(XSL_ENGINE_ERROR);
    const char* detected = transcribe_detected_language(session.get());
    const std::string language = detected && *detected ? detected : params.language ? params.language : "";
    Json candidates = Json::array();
    // Leave room for escaping, the primary text, and the rest of the frame.
    auto remaining = max_frame - 1024 - Json(text).dump().size();
    for (int i = 0; i < std::min(int(XSL_MAX_CANDIDATES), transcribe_n_hypotheses(session.get())); ++i) {
        const std::string_view candidate = transcribe_hypothesis_text(session.get(), i);
        if (candidate.empty() || candidate.size() > max_result_size || !valid_utf8(candidate)) continue;
        Json value = candidate;
        const auto bytes = value.dump().size() + 1;
        if (bytes > remaining) break;
        remaining -= bytes;
        candidates.push_back(std::move(value));
    }
    return {{"status", XSL_OK}, {"text", std::move(text)}, {"candidates", std::move(candidates)},
            {"language", language},
            {"vocabulary", vocabulary}, {"capture_ms", count * 1000 / sample_rate},
            {"compute_ms", std::chrono::duration_cast<std::chrono::milliseconds>(compute_time).count()}};
}


} // namespace
} // namespace xsl

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        if (transcribe_init_backends_default() != TRANSCRIBE_OK) return 1;
        return xsl::Engine{}.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
