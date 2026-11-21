#include "audio.hpp"
#include "models.hpp"

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
#include <shobjidl.h>
#endif

namespace xvr {
namespace {

std::string browse_folder() {
#ifdef _WIN32
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return {};
    struct Uninitialize { ~Uninitialize() { CoUninitialize(); } } uninitialize;
    IFileDialog* raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&raw)))) return {};
    const auto release = [](auto* p) { if (p) p->Release(); };
    std::unique_ptr<IFileDialog, decltype(release)> dialog(raw, release);
    if (FAILED(dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR)) ||
        FAILED(dialog->Show(nullptr))) return {};
    IShellItem* selected = nullptr;
    if (FAILED(dialog->GetResult(&selected))) return {};
    std::unique_ptr<IShellItem, decltype(release)> item(selected, release);
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> text(path, CoTaskMemFree);
    return fs::path(text.get()).u8string();
#else
    return {};
#endif
}

class Engine {
public:
    int run() {
        publish({{"type", "hello"}, {"protocol", 1}});
        std::thread inference([&] { inference_loop(); });
        std::thread files([&] { file_loop(); });
        Json message;
        while (read_frame(std::cin, message)) {
            const auto type = message.value("type", "");
            if (type == "shutdown") break;
            {
                std::lock_guard lock(mutex_);
                if (type == "begin") {
                    if (active_) continue;
                    active_ = std::make_shared<Control>(message.at("session").get<uint64_t>());
                    if (message.value("wav", "").empty()) start_capture(message, *active_);
                    inference_.push_back({std::move(message), active_});
                } else if (type == "end" || type == "cancel") {
                    if (active_ && active_->id == message.at("session").get<uint64_t>()) {
                        if (type == "end") active_->released = true;
                        else active_->cancelled = true;
                    }
                } else if (type == "cancel_download") {
                    download_cancelled_ = true;
                } else if (type == "scan" || type == "identify" || type == "download" || type == "browse") {
                    if (file_jobs_.size() < 8) {
                        if (type == "download") download_cancelled_ = false;
                        file_jobs_.push_back(std::move(message));
                    }
                } else if (inference_.size() < 16) {
                    inference_.push_back({std::move(message), {}});
                }
            }
            available_.notify_all();
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

    void publish(const Json& event) {
        std::lock_guard lock(output_);
        auto text = wire_frame(event);
        if (!text.empty()) std::cout << text << std::flush;
    }

    void state(uint32_t value) {
        publish({{"type", "state"}, {"generation", generation_}, {"state", value}});
    }

    bool load() {
        if (model_) return true;
        if (path_.empty()) return false;
        state(XVR_MODEL_LOADING);
        if (!matches_digest(fs::u8path(path_), sha256_, stopping_)) {
            state(XVR_MODEL_MISSING);
            return false;
        }
        transcribe_model_load_params params{};
        transcribe_model_load_params_init(&params);
        params.backend = TRANSCRIBE_BACKEND_CPU;
        transcribe_model* candidate = nullptr;
        if (transcribe_model_load_file(path_.c_str(), &params, &candidate) != TRANSCRIBE_OK) {
            state(XVR_MODEL_MISSING);
            return false;
        }
        model_.reset(candidate);
        state(XVR_MODEL_READY);
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
                if (path != path_ || sha256 != sha256_) model_.reset();
                path_ = std::move(path);
                sha256_ = std::move(sha256);
                generation_ = next.request.value("generation", uint64_t(0));
                if (!next.request.value("preload", false) || !load()) {
                    state(path_.empty() ? XVR_MODEL_MISSING : model_ ? XVR_MODEL_READY :
                        next.request.value("preload", false) ? XVR_MODEL_MISSING : XVR_MODEL_UNLOADED);
                }
            } else if (type == "unload" && next.request.value("generation", uint64_t(0)) == generation_) {
                model_.reset();
                state(path_.empty() ? XVR_MODEL_MISSING : XVR_MODEL_UNLOADED);
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
            const auto path = fs::u8path(next.value("path", ""));
            const auto ticket = next.value("ticket", uint64_t(0));
            try {
                if (type == "scan") {
                    publish({{"type", "scan"}, {"ticket", ticket},
                             {"items", scan_models(path, next.at("catalog"), stopping_)}});
                } else if (type == "identify") {
                    publish({{"type", "identify"}, {"ticket", ticket},
                             {"model", identify_model(path, next.at("catalog"), stopping_)}});
                } else if (type == "browse") {
                    publish({{"type", "browse"}, {"ticket", ticket}, {"path", browse_folder()}});
                } else {
                    const auto report = [&](uint64_t bytes) {
                        publish({{"type", "download_progress"}, {"ticket", ticket}, {"bytes", bytes}});
                    };
                    const auto status = download_model(path, next.at("entry"), download_cancelled_, report);
                    publish({{"type", "download"}, {"ticket", ticket}, {"status", status}});
                }
            } catch (const std::exception&) {
                publish({{"type", type}, {"ticket", ticket}, {"status", "error"}});
            }
        }
    }

    void start_capture(const Json& request, Control& control) {
        const auto microphone = request.value("microphone", "");
        const auto injection = fs::u8path(request.value("injection", ""));
        control.capture = std::thread([this, &control, microphone, injection] {
            record(control.recording, microphone, injection, control.released, control.cancelled,
                [&](float level, bool started) {
                    publish({{"type", started ? "capture" : "level"}, {"session", control.id}, {"level", level}});
                });
        });
    }

    Json transcribe(const Json& request, Control& control);

    std::mutex mutex_, output_;
    std::condition_variable available_;
    std::deque<Job> inference_;
    std::deque<Json> file_jobs_;
    std::shared_ptr<Control> active_;
    std::atomic<bool> stopping_{false}, download_cancelled_{false};
    std::unique_ptr<transcribe_model, decltype(&transcribe_model_free)> model_{nullptr, transcribe_model_free};
    std::string path_, sha256_;
    uint64_t generation_ = 0;
};

Json Engine::transcribe(const Json& request, Control& control) {
    const auto error = [](uint32_t status) { return Json{{"status", status}}; };
    if (request.value("generation", uint64_t(0)) != generation_) return error(XVR_MODEL_UNAVAILABLE);
    if (control.cancelled) return error(XVR_CANCELLED);
    const auto sample = request.value("wav", "");
    auto& recording = control.recording;
    if (sample.empty()) {
        while (!recording.count && !recording.closed && !control.cancelled && !control.released)
            std::this_thread::sleep_for(5ms);
        if (!recording.count) {
            if (control.cancelled) return error(XVR_CANCELLED);
            return error(recording.error != XVR_OK ? uint32_t(recording.error) : XVR_NO_AUDIO);
        }
    } else {
        recording.samples = read_wav(fs::u8path(sample));
        recording.count = recording.samples.size();
        if (!recording.count) return error(XVR_NO_AUDIO);
    }

    const auto compute_start = Clock::now();
    if (!load()) return error(XVR_MODEL_UNAVAILABLE);
    if (control.cancelled) return error(XVR_CANCELLED);
    transcribe_session_params session_parameters{};
    transcribe_session_params_init(&session_parameters);
    const auto cores = std::max(2u, std::thread::hardware_concurrency());
    session_parameters.n_threads = int(std::min(6u, cores / 2));
    transcribe_session* raw_session = nullptr;
    if (transcribe_session_init(model_.get(), &session_parameters, &raw_session) != TRANSCRIBE_OK)
        return error(XVR_ENGINE_ERROR);
    std::unique_ptr<transcribe_session, decltype(&transcribe_session_free)> session(raw_session, transcribe_session_free);
    transcribe_set_abort_callback(session.get(), +[](void* pointer) {
        return static_cast<Control*>(pointer)->cancelled.load();
    }, &control);

    auto terms = request.value("terms", std::vector<std::string>{});
    std::vector<const char*> pointers;
    for (const auto& term : terms) pointers.push_back(term.c_str());
    uint32_t vocabulary = XVR_VOCAB_NONE;
    transcribe_run_params params{};
    transcribe_run_params_init(&params);
    if (!pointers.empty()) {
        const bool supported = transcribe_model_supports(model_.get(), TRANSCRIBE_FEATURE_VOCABULARY);
        vocabulary = supported ? XVR_VOCAB_APPLIED : XVR_VOCAB_UNSUPPORTED;
        if (supported) {
            params.vocabulary = pointers.data();
            params.n_vocabulary = int32_t(pointers.size());
        }
    }

    const bool streaming = request.value("early", false);
    if (streaming && transcribe_stream_begin(session.get(), &params, nullptr) != TRANSCRIBE_OK)
        return error(XVR_ENGINE_ERROR);
    size_t fed = 0;
    auto status = TRANSCRIBE_OK;
    while (sample.empty() && !recording.closed && !control.cancelled && status == TRANSCRIBE_OK) {
        const auto count = recording.count.load(std::memory_order_acquire);
        if (streaming && count - fed >= sample_rate / 4) {
            status = transcribe_stream_feed(session.get(), recording.samples.data() + fed, int(count - fed), nullptr);
            fed = count;
        } else {
            std::this_thread::sleep_for(10ms);
        }
    }
    if (control.cancelled) return error(XVR_CANCELLED);
    if (recording.error != XVR_OK) return error(recording.error);
    const auto count = recording.count.load(std::memory_order_acquire);
    if (!contains_audio(recording.samples.data(), count)) return error(XVR_NO_SPEECH);
    const auto decode_start = streaming ? compute_start : Clock::now();
    if (streaming) {
        if (status == TRANSCRIBE_OK && count > fed)
            status = transcribe_stream_feed(session.get(), recording.samples.data() + fed, int(count - fed), nullptr);
        if (status == TRANSCRIBE_OK) status = transcribe_stream_finalize(session.get(), nullptr);
    } else {
        status = transcribe_run(session.get(), recording.samples.data(), int(count), &params);
    }
    if (control.cancelled) return error(XVR_CANCELLED);
    if (status == TRANSCRIBE_ERR_OUTPUT_TRUNCATED || transcribe_was_truncated(session.get()))
        return error(XVR_OUTPUT_TRUNCATED);
    if (status != TRANSCRIBE_OK) return error(XVR_ENGINE_ERROR);
    std::string text = transcribe_full_text(session.get());
    if (text.find_first_not_of(" \r\t\n") == std::string::npos) return error(XVR_NO_SPEECH);
    if (text.size() > max_result_size) return error(XVR_OUTPUT_TRUNCATED);
    if (!valid_utf8(text)) return error(XVR_ENGINE_ERROR);
    const auto* language = transcribe_detected_language(session.get());
    return {{"status", XVR_OK}, {"text", std::move(text)}, {"language", language ? language : ""},
            {"vocabulary", vocabulary}, {"capture_ms", count * 1000 / sample_rate},
            {"compute_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - decode_start).count()}};
}

} // namespace
} // namespace xvr

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        if (transcribe_init_backends_default() != TRANSCRIBE_OK) return 1;
        return xvr::Engine{}.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
