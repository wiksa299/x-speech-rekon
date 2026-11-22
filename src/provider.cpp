#include "core.hpp"
#include "desktop.hpp"
#include "window.hpp"

#include <XPLMDataAccess.h>
#include <XPLMMenus.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace xsr;
using xsr::localized;

struct ModelName { const char* id; const char* label; };
constexpr ModelName names[] = {
    {"parakeet-tdt-0.6b-v3", "Parakeet v3"},
    {parakeet_v2_id, "Parakeet v2"},
    {nemotron_id, "Nemotron 3.5 Streaming"},
    {whisper_cpp_id, "Whisper Small.en · whisper.cpp"},
    {whisper_transcribe_id, "Whisper Small.en · transcribe.cpp"},
    {XSR_MODEL_QWEN3_ASR_06B, "Qwen3-ASR 0.6B"},
    {"moonshine-streaming-tiny", "Moonshine Tiny"},
    {"moonshine-streaming-small", "Moonshine Small"},
    {"moonshine-streaming-medium", "Moonshine Medium"},
    {"moonshine-voice-small", "Moonshine Voice Small"},
    {"moonshine-voice-medium", "Moonshine Voice Medium"},
};

const char* model_label(const std::string& id) {
    for (const auto& model : names) if (id == model.id) return model.label;
    return "—";
}

const char* status_label(uint32_t code) {
    switch (code) {
    case XSR_OK: return localized("Ready", "Prêt", "Listo");
    case XSR_BUSY: return localized("Microphone in use", "Microphone occupé", "Micrófono ocupado");
    case XSR_MODEL_UNAVAILABLE: return localized("Model unavailable", "Modèle indisponible", "Modelo no disponible");
    case XSR_MIC_UNAVAILABLE: return localized("Microphone unavailable", "Microphone indisponible", "Micrófono no disponible");
    case XSR_ENGINE_LOST: return localized("Engine stopped", "Moteur arrêté", "Motor detenido");
    case XSR_ENGINE_RESTARTING: return localized("Engine starting", "Moteur en démarrage", "Iniciando el motor");
    case XSR_TIMEOUT: return localized("Timed out", "Délai dépassé", "Tiempo agotado");
    case XSR_CANCELLED: return localized("Cancelled", "Annulé", "Cancelado");
    case XSR_NO_AUDIO: return localized("No audio captured", "Aucun son capturé", "No se ha capturado audio");
    case XSR_NO_SPEECH: return localized("No speech detected", "Aucune parole détectée", "No se ha detectado habla");
    case XSR_OUTPUT_TRUNCATED: return localized("Transcript too long", "Transcription trop longue", "Transcripción demasiado larga");
    default: return localized("Transcription failed", "Échec de la transcription", "Error de transcripción");
    }
}

void emphasized_text(const char* text, float extra_size = 3) {
    const auto pos = ImGui::GetCursorScreenPos();
    auto* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() + extra_size;
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImGui::GetWindowDrawList()->AddText(font, size, pos, ImGui::GetColorU32(ImGuiCol_Text), text, nullptr, width);
    const auto bounds = font->CalcTextSizeA(size, std::numeric_limits<float>::max(), width, text);
    ImGui::Dummy({0, bounds.y + 2});
}

constexpr ImGuiChildFlags panel_flags = ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY;
constexpr ImVec4 green_text{0.57f, 0.84f, 0.68f, 1};
constexpr ImVec4 amber_text{0.94f, 0.73f, 0.46f, 1};
constexpr ImVec4 primary_button{0.16f, 0.36f, 0.57f, 1};

fs::path plugin_root() {
    char file[1024]{};
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, file, nullptr, nullptr);
    return fs::u8path(file).parent_path().parent_path();
}

fs::path preferences() {
    char path[1024]{};
    XPLMGetPrefsPath(path);
    return fs::u8path(path).parent_path() / "x-speech-rekon.json";
}

fs::path engine_path(const fs::path& root) {
#ifdef _WIN32
    return root / "bin" / "xsr-engine.exe";
#else
    return root / "bin" / "xsr-engine";
#endif
}

Json json_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return file ? Json::parse(file, nullptr, false) : Json();
}

bool string_fields(const Json& value, std::initializer_list<const char*> fields) {
    return value.is_object() && std::all_of(fields.begin(), fields.end(), [&](const char* key) {
        const auto field = value.find(key);
        return field != value.end() && field->is_string();
    });
}

bool valid_digest(std::string_view digest) {
    return digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

bool valid_catalog_entry(const Json& entry) {
    if (!string_fields(entry, {"id", "quant", "revision", "file", "sha256"}) ||
        !entry.contains("bytes") || !entry["bytes"].is_number_unsigned() || entry["bytes"] == 0 ||
        !entry.contains("urls") || !entry["urls"].is_array() || entry["urls"].empty()) return false;
    return valid_digest(entry["sha256"].get_ref<const std::string&>()) &&
        std::all_of(entry["urls"].begin(), entry["urls"].end(), [](const Json& url) {
            return url.is_string() && url.get_ref<const std::string&>().rfind("https://", 0) == 0;
        });
}

Profile decode_profile(const Json& value) {
    if (!string_fields(value, {"id", "quant", "revision", "path", "sha256"}) ||
        !valid_digest(value["sha256"].get_ref<const std::string&>())) return {};
    return {value["id"], value["quant"], value["revision"], value["path"], value["sha256"]};
}

struct FileRequest {
    uint64_t id = 0;
    Clock::time_point deadline{};
    explicit operator bool() const { return id != 0; }
    bool matches(uint64_t ticket) const { return id != 0 && id == ticket; }
    bool expired(Clock::time_point now) const { return id != 0 && now >= deadline; }
};

class PreferencesWriter {
public:
    explicit PreferencesWriter(fs::path path) : path_(std::move(path)), worker_([this] { run(); }) {}
    ~PreferencesWriter() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_one();
        worker_.join();
    }
    bool failed() const { return failed_; }
    void save(std::string contents) {
        {
            std::lock_guard lock(mutex_);
            pending_ = std::move(contents);
        }
        changed_.notify_one();
    }
private:
    void run() {
        for (;;) {
            std::string next;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
                if (!pending_) return;
                next = std::move(*pending_);
                pending_.reset();
            }
            auto temporary = path_;
            temporary += ".tmp";
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << next;
            output.close();
            if (output.fail()) {
                failed_ = true;
                continue;
            }
#ifdef _WIN32
            failed_ = !MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
            std::error_code error;
            fs::rename(temporary, path_, error);
            failed_ = bool(error);
#endif
        }
    }
    fs::path path_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<std::string> pending_;
    bool stopping_ = false;
    std::atomic<bool> failed_{false};
    std::thread worker_;
};

std::string plugin_string(int id, bool signature) {
    char name[256]{}, sig[256]{};
    XPLMGetPluginInfo(id, name, nullptr, sig, nullptr);
    return signature ? sig : name;
}

enum class TestSource { microphone, reference, wav };

const char* test_source_label(TestSource type) {
    switch (type) {
    case TestSource::reference:
        return localized("Built-in recording", "Enregistrement fourni", "Grabación incluida");
    case TestSource::wav:
        return localized("WAV file", "Fichier WAV", "Archivo WAV");
    default:
        return localized("Microphone", "Microphone", "Micrófono");
    }
}

const char* activity_status(uint32_t status, uint32_t stage, TestSource source = TestSource::microphone) {
    if (status == XSR_TIMEOUT && stage == XSR_STAGE_TRANSCRIPTION)
        return localized("Transcription timed out", "Délai de transcription dépassé", "Tiempo de transcripción agotado");
    if (status == XSR_BAD_REQUEST && source != TestSource::microphone)
        return localized("Unreadable WAV or longer than 30 seconds", "WAV illisible ou de plus de 30 secondes",
                         "WAV ilegible o de más de 30 segundos");
    return status_label(status);
}

const char* latency_label(TestSource source) {
    return source == TestSource::microphone ?
        localized("%u ms after release", "%u ms après relâchement", "%u ms tras soltar") :
        localized("Test completed in %u ms", "Test terminé en %u ms", "Prueba completada en %u ms");
}

struct Incident {
    std::time_t when;
    uint32_t status;
    uint32_t stage;
    TestSource source;
};

std::string incident_time(std::time_t when) {
    std::tm clock{};
#ifdef _WIN32
    if (localtime_s(&clock, &when) != 0) return {};
#else
    if (!localtime_r(&when, &clock)) return {};
#endif
    char formatted[32]{};
    std::strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M:%S", &clock);
    return formatted;
}

struct Activity {
    uint64_t sequence;
    int client;
    uint64_t session;
    std::string name;
    uint32_t status;
    uint32_t stage = XSR_STAGE_NONE;
    uint32_t vocabulary = XSR_VOCAB_NONE;
    std::string model, quant, text;
    std::optional<Clock::time_point> waiting_since;
    std::optional<uint32_t> latency_ms;
    TestSource source = TestSource::microphone;
    bool pending = false;
    bool model_was_loaded = false;
};

class Provider {
public:
    Provider() : root_(plugin_root()), preferences_(preferences()), writer_(preferences_), process_(engine_path(root_)),
        broker_([&](const Json& command) { return process_.send(command); },
                [&](const Delivery& delivery) { pending_events_.push_back(delivery); }),
        window_([&] { draw(); }, 620, 810, root_ / "NotoSans-Regular.ttf") {
        catalog_ = json_file(root_ / "catalog.json");
        catalog_error_ = !catalog_.is_array() || !std::all_of(catalog_.begin(), catalog_.end(), valid_catalog_entry);
        if (catalog_error_) catalog_ = Json::array();
        load_preferences();
        XPLMHostApplicationID host{};
        XPLMGetVersions(&xplane_version_, &sdk_version_, &host);
        menu_index_ = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "X-Speech Rekon", nullptr, 0);
        menu_ = XPLMCreateMenu("X-Speech Rekon", XPLMFindPluginsMenu(), menu_index_, menu_callback, this);
        XPLMAppendMenuItem(menu_, localized("Settings", "Réglages", "Ajustes"), nullptr, 0);
        XPLMRegisterFlightLoopCallback(tick_callback, -1, this);
    }

    ~Provider() {
        shutting_down_ = true;
        XPLMUnregisterFlightLoopCallback(tick_callback, this);
        if (engine_restart_.valid()) engine_restart_.wait();
        broker_.shutdown(Clock::now());
        dispatch_events();
        process_.stop();
        XPLMDestroyMenu(menu_);
        XPLMRemoveMenuItem(XPLMFindPluginsMenu(), menu_index_);
    }

    bool service_ready() const { return broker_.service_ready(); }
    int model_state() const { return static_cast<int>(broker_.model_state()); }
    bool ptt_busy() const { return broker_.busy(); }

    void request(int sender, XsrRequest& command) {
        if (shutting_down_ || sender < 0 || !XPLMIsPluginEnabled(sender)) return;
        const auto now = Clock::now();
        broker_.request(sender, command, now);
        if (command.struct_size < sizeof(command) || command.version != XSR_ABI_VERSION) return;
        if (command.operation == XSR_SUBSCRIBE && command.status == XSR_OK) {
            subscribers_[sender] = {plugin_string(sender, false), plugin_string(sender, true)};
        } else if (command.operation == XSR_UNSUBSCRIBE && command.status == XSR_OK) {
            subscribers_.erase(sender);
            cancel_activity(sender);
        } else if (command.operation == XSR_PTT_BEGIN) {
            add_activity(sender, command.session, command.status, plugin_string(sender, false),
                         TestSource::microphone, broker_.model_state() == XSR_MODEL_READY, now);
        } else if (command.operation == XSR_PTT_END && command.status == XSR_OK) {
            if (auto* row = pending_activity(sender, command.session)) row->waiting_since = now;
        }
    }

private:
    static void menu_callback(void* owner, void*) {
        auto& self = *static_cast<Provider*>(owner);
        self.window_.show("X-Speech Rekon");
        if (self.broker_.ready()) self.process_.send({{"type", "microphones"}});
    }

    static float tick_callback(float, float, int, void* owner) {
        auto& self = *static_cast<Provider*>(owner);
        try { return self.tick(); }
        catch (const std::exception&) {
            XPLMDebugString("X-Speech Rekon: provider update failed\n");
            self.protocol_fault_ = true;
            return 0.05f;
        }
    }

    void record_incident(uint32_t status, uint32_t stage = XSR_STAGE_NONE,
                         TestSource source = TestSource::microphone) {
        switch (status) {
        case XSR_BAD_REQUEST:
        case XSR_MODEL_UNAVAILABLE:
        case XSR_MIC_UNAVAILABLE:
        case XSR_ENGINE_LOST:
        case XSR_ENGINE_RESTARTING:
        case XSR_TIMEOUT:
        case XSR_NO_AUDIO:
        case XSR_NO_SPEECH:
        case XSR_OUTPUT_TRUNCATED:
        case XSR_ENGINE_ERROR:
            break;
        default:
            return;
        }
        const auto now = std::time(nullptr);
        if (!incidents_.empty()) {
            const auto& last = incidents_.front();
            if (last.status == status && last.stage == stage && last.source == source &&
                last.when == now) return;
        }
        incidents_.push_front({now, status, stage, source});
        if (incidents_.size() > 8) incidents_.pop_back();
    }

    void engine_started(bool started, Clock::time_point now) {
        waiting_for_hello_ = started;
        if (started) {
            hello_deadline_ = now + 5s;
        } else {
            retry_at_ = now + 2s;
            if (engine_status_ != XSR_ENGINE_LOST) record_incident(XSR_ENGINE_LOST);
            engine_status_ = XSR_ENGINE_LOST;
        }
    }

    void restart_engine(bool recovery = false) {
        const auto now = Clock::now();
        if (recovery && now < retry_at_) return;
        try {
            engine_restart_ = std::async(std::launch::async, [this] { return process_.start(); });
        } catch (const std::system_error&) {
            engine_started(false, now);
            XPLMDebugString("X-Speech Rekon: could not schedule engine restart\n");
            return;
        }
        if (recovery && (broker_.ready() || waiting_for_hello_)) record_incident(XSR_ENGINE_LOST);
        broker_.engine_lost(now);
        protocol_fault_ = false;
        clear_file_requests();
        waiting_for_hello_ = false;
        boot_selection_ = !broker_.settings().profile.available();
        engine_status_ = XSR_ENGINE_RESTARTING;
        retry_at_ = now + 500ms;
    }

    void clear_file_requests() {
        if (scan_ticket_) scan_error_ = true;
        if (identify_ticket_) model_error_ = true;
        if (browse_ticket_) folder_picker_failed_ = true;
        if (wav_ticket_) wav_picker_failed_ = true;
        if (download_ticket_) download_state_ = "error";
        scan_ticket_ = identify_ticket_ = browse_ticket_ = wav_ticket_ = download_ticket_ = {};
        scan_deferred_ = false;
    }

    float tick() {
        const auto now = Clock::now();
        for (auto it = subscribers_.begin(); it != subscribers_.end();) {
            if (!XPLMIsPluginEnabled(it->first) || plugin_string(it->first, true) != it->second.second) {
                broker_.forget(it->first, now);
                cancel_activity(it->first);
                it = subscribers_.erase(it);
            } else {
                ++it;
            }
        }
        broker_.tick(now);
        if (engine_restart_.valid()) {
            if (engine_restart_.wait_for(0ms) != std::future_status::ready) return 0.05f;
            bool started = false;
            try { started = engine_restart_.get(); }
            catch (const std::exception&) { XPLMDebugString("X-Speech Rekon: engine restart failed\n"); }
            engine_started(started, now);
        } else if (protocol_fault_ || process_.failed() || broker_.restart_due(now) ||
                   (waiting_for_hello_ && now > hello_deadline_)) {
            restart_engine(true);
            dispatch_events();
            return 0.05f;
        }
        for (const auto& message : process_.receive()) on_engine_event(message, now);
        if (scan_deferred_ && !scan_ticket_ && !browse_ticket_ && !wav_ticket_) scan();
        if (scan_ticket_.expired(now) || identify_ticket_.expired(now) || browse_ticket_.expired(now) ||
            wav_ticket_.expired(now) || download_ticket_.expired(now)) {
            record_incident(XSR_TIMEOUT);
            // Recycling the engine also closes a file picker or transfer that no longer replies.
            restart_engine(true);
        }
        if (local_held_ && !window_.visible()) {
            broker_.cancel_local(now);
            local_held_ = false;
        }
        dispatch_events();
        const auto& profile = broker_.settings().profile;
        const auto& last_good = broker_.last_good();
        if (profile.path != persisted_profile_ || last_good.path != persisted_good_path_ ||
            last_good.sha256 != persisted_good_hash_ || (writer_.failed() && now >= preferences_retry_at_)) {
            save_preferences();
        }
        return broker_.busy() ? -1.0f : 0.05f;
    }

    void on_engine_event(const Json& message, Clock::time_point now) {
        const auto type = message.value("type", "");
        const auto ticket = message.value("ticket", uint64_t(0));
        if (type == "hello") {
            if (message.value("protocol", 0) != 1) throw std::runtime_error("Unsupported engine protocol");
            const auto& devices = message.at("devices");
            if (!devices.is_array() || !std::all_of(devices.begin(), devices.end(), [](const Json& device) {
                return string_fields(device, {"id", "label"});
            })) throw std::runtime_error("Invalid engine devices");
            engine_version_ = message.at("version").get<std::string>();
            moonshine_version_ = message.value("moonshine_version", "");
            whisper_version_ = message.value("whisper_version", "");
            waiting_for_hello_ = false;
            engine_status_ = XSR_OK;
            compute_devices_ = devices;
            auto& entries = catalog_.get_ref<Json::array_t&>();
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [](const Json& entry) { return entry.contains("parts"); }), entries.end());
            const auto official = message.value("moonshine_models", Json::array());
            if (official.is_array())
                for (const auto& entry : official)
                    if (entry.is_object() && entry.contains("parts")) catalog_.push_back(entry);
            broker_.engine_online(now);
            scan();
            if (window_.visible()) process_.send({{"type", "microphones"}});
        } else if (type == "capture" || type == "level" || type == "result" || type == "state" ||
                   type == "inference_rejected") {
            if (message.value("session", uint64_t(0)) == test_session_ && (type == "level" || type == "capture"))
                test_level_ = message.value("level", 0.0f);
            if (type == "result" && message.at("status") == XSR_OK &&
                (!string_fields(message, {"text", "language"}) || !message.at("vocabulary").is_number_unsigned() ||
                 !message.at("capture_ms").is_number_unsigned() || !message.at("compute_ms").is_number_unsigned()))
                throw std::runtime_error("Invalid engine result");
            const auto path = broker_.settings().profile.path;
            const auto hash = broker_.settings().profile.sha256;
            broker_.accept(message, now);
            const auto& active = broker_.settings().profile;
            if (active.path != path || active.sha256 != hash) {
                selected_id_ = active.id;
                selected_quant_ = active.quant;
                profile_recovered_ = true;
                record_incident(XSR_MODEL_UNAVAILABLE);
            }
        } else if (type == "microphones") {
            if (message.value("status", "ok") != "ok") return;
            const auto& items = message.at("items");
            if (!items.is_array() || !std::all_of(items.begin(), items.end(), [](const Json& item) {
                return string_fields(item, {"id", "name"});
            })) throw std::runtime_error("Invalid engine microphone list");
            microphones_ = items;
        } else if (type == "scan" && scan_ticket_.matches(ticket)) {
            scan_ticket_ = {};
            scan_error_ = message.value("status", "error") != "ok";
            if (scan_error_) {
                record_incident(XSR_ENGINE_ERROR);
                if (boot_selection_ && !startup_path_.empty()) identify(startup_path_);
                return;
            }
            const auto& items = message.at("items");
            if (!items.is_array() || !std::all_of(items.begin(), items.end(), [](const Json& item) {
                return string_fields(item, {"state", "path"}) && (item["state"] != "candidate" ||
                    string_fields(item, {"id", "quant"}));
            })) throw std::runtime_error("Invalid engine scan result");
            models_ = items;
            if (boot_selection_) {
                boot_selection_ = false;
                std::string candidate;
                if (!startup_path_.empty()) {
                    for (const auto& file : models_) {
                        if (file.value("state", "") == "candidate" && file.value("path", "") == startup_path_)
                            candidate = startup_path_;
                    }
                }
                if (candidate.empty() && broker_.last_good().available()) candidate = broker_.last_good().path;
                if (candidate.empty()) candidate = candidate_path();
                if (candidate.empty()) {
                    for (const auto& file : models_) {
                        if (file.value("state", "") == "candidate") {
                            candidate = file.at("path").get<std::string>();
                            selected_id_ = file.at("id").get<std::string>();
                            selected_quant_ = file.at("quant").get<std::string>();
                            break;
                        }
                    }
                }
                if (!candidate.empty()) identify(candidate);
            }
        } else if (type == "identify" && identify_ticket_.matches(ticket)) {
            identify_ticket_ = {};
            boot_selection_ = false;
            const auto profile = decode_profile(message.value("model", Json{}));
            if (profile.available()) {
                draft_.profile = profile;
                selected_id_ = draft_.profile.id;
                selected_quant_ = draft_.profile.quant;
                broker_.set_settings(draft_, now);
                save_preferences();
                model_error_ = profile_recovered_ = false;
            } else {
                model_error_ = true;
                record_incident(XSR_MODEL_UNAVAILABLE);
                if (!broker_.settings().profile.available() && broker_.last_good().available()) {
                    draft_.profile = broker_.last_good();
                    broker_.set_settings(draft_, now);
                    selected_id_ = draft_.profile.id;
                    selected_quant_ = draft_.profile.quant;
                    model_error_ = false;
                    profile_recovered_ = true;
                }
            }
        } else if (type == "browse" && browse_ticket_.matches(ticket)) {
            browse_ticket_ = {};
            folder_picker_failed_ = message.value("status", "error") != "ok";
            if (folder_picker_failed_) record_incident(XSR_ENGINE_ERROR);
            const auto path = message.value("path", "");
            if (!path.empty() && path != folder_.u8string()) {
                folder_ = fs::u8path(path);
                models_ = Json::array();
                save_preferences();
                scan();
            }
        } else if (type == "browse_wav" && wav_ticket_.matches(ticket)) {
            wav_ticket_ = {};
            wav_picker_failed_ = message.value("status", "error") != "ok";
            if (wav_picker_failed_) record_incident(XSR_ENGINE_ERROR, XSR_STAGE_NONE, TestSource::wav);
            auto path = message.value("path", "");
            if (!wav_picker_failed_ && !path.empty()) start_test(TestSource::wav, std::move(path));
        } else if (type == "download_progress" && download_ticket_.matches(ticket)) {
            downloaded_ = message.value("bytes", uint64_t(0));
        } else if (type == "download" && download_ticket_.matches(ticket)) {
            download_ticket_ = {};
            download_state_ = message.value("status", "error");
            if (download_state_ == "ok") scan();
            else if (download_state_ != "cancelled") record_incident(XSR_ENGINE_ERROR);
        }
    }

    void dispatch_events() {
        std::vector<Delivery> events;
        events.swap(pending_events_);
        for (const auto& event : events) {
            std::optional<uint32_t> latency;
            if (event.kind == XSR_RESULT) {
                if (auto* row = pending_activity(event.client, event.session)) {
                    row->status = event.status;
                    row->stage = event.stage;
                    row->pending = false;
                    record_incident(event.status, event.stage, row->source);
                    if (event.status == XSR_OK && row->waiting_since) {
                        latency = static_cast<uint32_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - *row->waiting_since).count());
                        row->latency_ms = latency;
                    }
                    if (event.result.is_object()) {
                        row->model = event.result.value("id", "");
                        row->quant = event.result.value("quant", "");
                        row->text = event.result.value("text", "");
                        row->vocabulary = event.result.value("vocabulary", uint32_t(XSR_VOCAB_NONE));
                    }
                }
            }
            if (event.client == local_client) {
                if (event.session != test_session_) continue;
                if (event.kind == XSR_CAPTURE_STARTED) listening_ = true;
                if (event.kind == XSR_RESULT) {
                    listening_ = local_held_ = false;
                    test_pending_ = false;
                    test_level_ = 0;
                    test_status_ = event.status;
                    test_text_ = event.result.is_object() ? event.result.value("text", "") : "";
                    test_latency_ms_ = latency;
                }
                continue;
            }
            std::string text, model, revision, quant, language;
            uint32_t vocab = XSR_VOCAB_NONE, capture_ms = 0, compute_ms = 0;
            if (event.status == XSR_OK && event.kind == XSR_RESULT) {
                text = event.result.value("text", "");
                model = event.result.value("id", "");
                revision = event.result.value("revision", "");
                quant = event.result.value("quant", "");
                language = event.result.value("language", "");
                vocab = event.result.value("vocabulary", uint32_t(XSR_VOCAB_NONE));
                capture_ms = event.result.value("capture_ms", 0u);
                compute_ms = event.result.value("compute_ms", 0u);
            }
            if (event.kind == XSR_RESULT) {
                const auto log = "X-Speech Rekon PTT " + status_name(event.status) + " " + model + " " +
                    std::to_string(capture_ms) + "ms audio " + std::to_string(compute_ms) + "ms compute\n";
                XPLMDebugString(log.c_str());
            }
            if (!subscribers_.count(event.client) || !XPLMIsPluginEnabled(event.client)) continue;
            XsrEvent outgoing{XSR_ABI_VERSION, sizeof(XsrEvent), event.kind, event.status, event.session,
                vocab, event.stage, capture_ms, compute_ms, text.c_str(), model.c_str(), revision.c_str(),
                quant.c_str(), language.c_str()};
            XPLMSendMessageToPlugin(event.client, XSR_MESSAGE_EVENT, &outgoing);
        }
    }

    void add_activity(int client, uint64_t session, uint32_t status, std::string name,
                      TestSource source, bool model_was_loaded, Clock::time_point now) {
        record_incident(status, XSR_STAGE_NONE, source);
        Activity row{};
        row.sequence = ++activity_sequence_;
        row.client = client;
        row.session = session;
        row.status = status;
        row.name = std::move(name);
        row.pending = status == XSR_OK;
        row.source = source;
        row.model_was_loaded = model_was_loaded;
        if (row.pending && source != TestSource::microphone) row.waiting_since = now;
        history_.push_front(std::move(row));
        if (history_.size() > 12) {
            const auto oldest = std::find_if(history_.rbegin(), history_.rend(), [](const Activity& item) {
                return !item.pending;
            });
            if (oldest != history_.rend()) history_.erase(std::prev(oldest.base()));
        }
    }

    Activity* pending_activity(int client, uint64_t session) {
        for (auto& row : history_)
            if (row.pending && row.client == client && row.session == session) return &row;
        return nullptr;
    }

    void cancel_activity(int client) {
        for (auto& row : history_) {
            if (!row.pending || row.client != client) continue;
            row.pending = false;
            row.status = XSR_CANCELLED;
        }
    }

    void load_preferences() {
        folder_ = root_ / "models";
        const auto saved = json_file(preferences_);
        if (!saved.is_object()) return;
        try {
            folder_ = fs::u8path(saved.value("folder", folder_.u8string()));
            selected_id_ = saved.value("id", selected_id_);
            selected_quant_ = saved.value("quant", selected_quant_);
            startup_path_ = saved.value("path", "");
            const auto last_good = decode_profile(saved.value("last_good", Json{}));
            broker_.restore_last_good(last_good);
            persisted_good_path_ = last_good.path;
            persisted_good_hash_ = last_good.sha256;
            draft_.microphone = saved.value("microphone", "");
            draft_.compute = saved.value("compute", "cpu");
            draft_.language = saved.value("language", "auto");
            if (draft_.language != "en-US" && draft_.language != "fr-FR" && draft_.language != "es-ES")
                draft_.language = "auto";
            draft_.early = saved.value("early", false);
            draft_.sidetone = saved.value("sidetone", false);
            draft_.load = saved.value("load", 0) == 1 ? LoadPolicy::on_activate : LoadPolicy::on_use;
            draft_.unload = static_cast<UnloadPolicy>(std::clamp(saved.value("unload", 2), 0, 2));
            draft_.idle_minutes = std::clamp(saved.value("idle_minutes", 5), 1, 60);
        } catch (const Json::exception&) {}
        broker_.set_settings(draft_, Clock::now());
    }

    void save_preferences() {
        const auto& settings = broker_.settings();
        persisted_profile_ = settings.profile.path;
        const auto path = settings.profile.path.empty() ? startup_path_ : settings.profile.path;
        const auto id = settings.profile.id.empty() ? selected_id_ : settings.profile.id;
        const auto quant = settings.profile.quant.empty() ? selected_quant_ : settings.profile.quant;
        const auto& last_good = broker_.last_good();
        persisted_good_path_ = last_good.path;
        persisted_good_hash_ = last_good.sha256;
        preferences_retry_at_ = Clock::now() + 5s;
        Json saved = {{"folder", folder_.u8string()}, {"path", path}, {"id", id}, {"quant", quant},
            {"microphone", settings.microphone}, {"compute", settings.compute}, {"language", settings.language},
            {"early", settings.early}, {"sidetone", settings.sidetone}, {"load", int(settings.load)},
            {"unload", int(settings.unload)}, {"idle_minutes", settings.idle_minutes}};
        if (last_good.available()) saved["last_good"] = {{"id", last_good.id}, {"quant", last_good.quant},
            {"revision", last_good.revision}, {"path", last_good.path}, {"sha256", last_good.sha256}};
        writer_.save(saved.dump(2, ' ', false, Json::error_handler_t::replace));
    }

    bool file_request(FileRequest& pending, Json command, Clock::duration timeout) {
        const auto ticket = ++next_ticket_;
        command["ticket"] = ticket;
        if (!process_.send(command)) {
            record_incident(XSR_ENGINE_RESTARTING);
            return false;
        }
        pending = {ticket, Clock::now() + timeout};
        return true;
    }

    void scan() {
        scan_deferred_ = bool(scan_ticket_) || bool(browse_ticket_) || bool(wav_ticket_);
        if (scan_deferred_) return;
        scan_error_ = !file_request(scan_ticket_, {{"type", "scan"}, {"path", folder_.u8string()},
                                                  {"catalog", catalog_}}, 90s);
    }

    void identify(const std::string& path) {
        profile_recovered_ = false;
        model_error_ = !file_request(identify_ticket_, {{"type", "identify"}, {"path", path},
                                                       {"catalog", catalog_}}, 3min);
    }

    std::string candidate_path() const {
        for (const auto& model : models_) {
            if (model.value("state", "") == "candidate" && model.value("id", "") == selected_id_ &&
                model.value("quant", "") == selected_quant_)
                return model.at("path").get<std::string>();
        }
        return {};
    }

    const Json* selected_entry() const {
        for (const auto& entry : catalog_) {
            if (entry.at("id") == selected_id_ && entry.at("quant") == selected_quant_) return &entry;
        }
        return nullptr;
    }

    std::string compute_label(const std::string& id) const {
        if (id == "cpu") return "CPU";
        for (const auto& device : compute_devices_) if (device.at("id") == id) return device.at("label");
        return localized("Vulkan unavailable", "Vulkan indisponible", "Vulkan no disponible");
    }

    void draw();
    void draw_models();
    bool draw_recognition_settings();
    bool draw_compute_and_memory();
    void draw_test_page();
    void draw_tests();
    void draw_activity();
    void draw_recent_requests();
    void draw_settings();
    void draw_diagnostic();
    void start_test(TestSource source, std::string wav = {});
    std::string microphone_label() const;
    std::vector<std::pair<std::string, std::string>> diagnostic_fields() const;

    fs::path root_, preferences_, folder_;
    PreferencesWriter writer_;
    Process process_;
    std::future<bool> engine_restart_;
    Broker broker_;
    Window window_;
    XPLMMenuID menu_ = nullptr;
    int menu_index_ = 0;
    std::map<int, std::pair<std::string, std::string>> subscribers_;
    std::deque<Activity> history_;
    std::deque<Incident> incidents_;
    std::future<bool> clipboard_copy_;
    std::optional<bool> clipboard_result_;
    int xplane_version_ = 0, sdk_version_ = 0;
    uint64_t activity_sequence_ = 0;
    std::vector<Delivery> pending_events_;
    Json catalog_ = Json::array(), models_ = Json::array(), microphones_ = Json::array(), compute_devices_ = Json::array();
    Settings draft_;
    std::string selected_id_ = "parakeet-tdt-0.6b-v3", selected_quant_ = "Q8_0";
    std::string startup_path_, persisted_profile_, download_state_, test_text_, engine_version_, moonshine_version_, whisper_version_;
    std::string persisted_good_path_, persisted_good_hash_;
    uint64_t next_ticket_ = 0, downloaded_ = 0, download_total_ = 0, test_session_ = 0;
    FileRequest scan_ticket_, identify_ticket_, browse_ticket_, download_ticket_, wav_ticket_;
    uint32_t engine_status_ = XSR_ENGINE_RESTARTING, test_status_ = XSR_OK;
    TestSource test_source_ = TestSource::microphone;
    std::optional<uint32_t> test_latency_ms_;
    bool wav_picker_failed_ = false, folder_picker_failed_ = false, scan_error_ = false, catalog_error_ = false;
    float test_level_ = 0;
    bool model_error_ = false, boot_selection_ = true, waiting_for_hello_ = false, shutting_down_ = false;
    bool local_held_ = false, test_pending_ = false, listening_ = false;
    bool scan_deferred_ = false, protocol_fault_ = false, profile_recovered_ = false;
    Clock::time_point retry_at_{}, hello_deadline_{}, preferences_retry_at_{};
};

void Provider::draw() {
    const auto& active = broker_.settings().profile;
    const auto state = broker_.model_state();
    const char* summary = status_label(broker_.ready() ? XSR_MODEL_UNAVAILABLE : engine_status_);
    ImVec4 color = amber_text;
    if (broker_.ready()) {
        switch (state) {
        case XSR_MODEL_READY:
            summary = localized("Model loaded", "Modèle chargé", "Modelo cargado");
            color = green_text;
            break;
        case XSR_MODEL_LOADING:
            summary = localized("Loading model", "Chargement du modèle", "Cargando el modelo");
            break;
        case XSR_MODEL_UNLOADED:
            summary = localized("Model ready to load", "Modèle prêt à charger", "Modelo listo para cargar");
            color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            break;
        }
    }
    const auto& actual = broker_.actual_compute();
    const auto compute = broker_.cpu_fallback() ? localized("CPU (fallback)", "CPU (repli)", "CPU (respaldo)") :
        actual.empty() ? (active.moonshine_voice() ? "CPU" : compute_label(draft_.compute)) : actual;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    if (active.id.empty()) {
        ImGui::TextWrapped("%s · %s", summary, compute.c_str());
    } else {
        ImGui::TextWrapped("%s · %s %s · %s", summary, model_label(active.id), active.quant.c_str(), compute.c_str());
    }
    ImGui::PopStyleColor();

    if (local_held_ && !window_.pointer_pressed()) {
        const auto now = Clock::now();
        const auto status = broker_.end_local(test_session_, now);
        if (status == XSR_OK) {
            if (auto* row = pending_activity(local_client, test_session_)) row->waiting_since = now;
        } else if (test_pending_) test_status_ = status;
        local_held_ = false;
        listening_ = false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12, 12});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 6});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, {0.164f, 0.176f, 0.199f, 1});
    ImGui::PushStyleColor(ImGuiCol_PopupBg, {0.16f, 0.176f, 0.199f, 1});
    ImGui::PushStyleColor(ImGuiCol_Border, {0.25f, 0.27f, 0.30f, 1});
    ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.207f, 0.226f, 0.267f, 1});
    ImGui::PushStyleColor(ImGuiCol_Button, {0.205f, 0.256f, 0.330f, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.23f, 0.45f, 0.68f, 1});
    ImGui::PushStyleColor(ImGuiCol_Tab, {0.165f, 0.187f, 0.225f, 1});
    ImGui::PushStyleColor(ImGuiCol_TabSelected, {0.21f, 0.32f, 0.48f, 1});
    if (ImGui::BeginTabBar("provider-pages", ImGuiTabBarFlags_DrawSelectedOverline)) {
        if (ImGui::BeginTabItem(localized("Settings###settings", "Réglages###settings", "Ajustes###settings"))) {
            draw_settings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(localized("Test###test", "Test###test", "Prueba###test"))) {
            draw_test_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(localized("Activity###activity", "Activité###activity", "Actividad###activity"))) {
            draw_activity();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(localized("Diagnostics###diagnostic", "Diagnostic###diagnostic", "Diagnóstico###diagnostic"))) {
            draw_diagnostic();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopStyleColor(8);
    ImGui::PopStyleVar(4);
}

void Provider::draw_settings() {
    if (!ImGui::BeginChild("settings-scroll", {}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) {
        ImGui::EndChild();
        return;
    }
    if (ImGui::BeginChild("model", {}, panel_flags)) draw_models();
    ImGui::EndChild();
    bool changed = false;
    if (ImGui::BeginChild("compute-and-memory", {}, panel_flags)) changed = draw_compute_and_memory();
    ImGui::EndChild();
    if (ImGui::BeginChild("recognition", {}, panel_flags)) changed |= draw_recognition_settings();
    ImGui::EndChild();
    if (changed) {
        draft_.profile = broker_.settings().profile;
        broker_.set_settings(draft_, Clock::now());
        save_preferences();
    }
    ImGui::EndChild();
}

void Provider::draw_models() {
    emphasized_text(localized("Model", "Modèle", "Modelo"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##model", model_label(selected_id_))) {
        for (const auto& model : names) {
            const Profile choice{model.id, {}, {}, {}, {}};
            if (choice.whisper_cpp() && whisper_version_.empty()) continue;
            if (choice.moonshine_voice() && std::none_of(catalog_.begin(), catalog_.end(),
                    [&](const Json& entry) { return entry.at("id") == model.id; })) continue;
            if (ImGui::Selectable(model.label, selected_id_ == model.id)) {
                selected_id_ = model.id;
                selected_quant_ = choice.moonshine_voice() ? "ORT" : "Q8_0";
            }
        }
        ImGui::EndCombo();
    }
    if (selected_id_ == parakeet_v2_id || selected_id_ == whisper_cpp_id || selected_id_ == whisper_transcribe_id) {
        ImGui::TextDisabled("%s", localized("English only", "Anglais uniquement", "Solo inglés"));
    }
    if (selected_quant_ == "ORT") {
        ImGui::TextDisabled("%s", localized("CPU · English", "CPU · Anglais", "CPU · Inglés"));
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(localized("Quantization", "Quantification", "Cuantización"));
        const bool label_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
        ImGui::SameLine(0, 5);
        ImGui::TextDisabled("(?)");
        if ((label_hovered || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) && ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 24 * ImGui::GetFontSize());
            ImGui::TextUnformatted(localized(
                "Quantization reduces the model’s size. Q4 is smaller, Q8 preserves more of the original precision, and Q6 is in between.",
                "La quantification réduit la taille du modèle. Q4 est plus compact, Q8 préserve mieux la précision d’origine, et Q6 est entre les deux.",
                "La cuantización reduce el tamaño del modelo. Q4 ocupa menos, Q8 conserva mejor la precisión original y Q6 queda entre ambos."));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (ImGui::GetContentRegionAvail().x >= 345) ImGui::SameLine();
        for (const char* quant : {"Q4_K_M", "Q6_K", "Q8_0"}) {
            const bool available = std::any_of(catalog_.begin(), catalog_.end(), [&](const Json& entry) {
                return entry.at("id") == selected_id_ && entry.at("quant") == quant;
            });
            const bool selected = selected_quant_ == quant;
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::BeginDisabled(!available);
            if (ImGui::Button(quant)) selected_quant_ = quant;
            ImGui::EndDisabled();
            if (selected) ImGui::PopStyleColor();
            if (std::strcmp(quant, "Q8_0") != 0) ImGui::SameLine();
        }
    }
    const auto* artifact = selected_entry();
    const auto candidate = candidate_path();
    if (artifact) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%.1f %s · %s", artifact->at("bytes").get<double>() / 1000000.0,
            localized("MB", "Mo", "MB"),
            candidate.empty() ? localized("Not installed", "Absent du dossier", "Sin instalar") :
                                localized("Found locally", "Présent dans le dossier", "Encontrado en la carpeta"));
        ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(!broker_.ready() || identify_ticket_ || browse_ticket_ || wav_ticket_ ||
                         candidate.empty() || broker_.busy());
    ImGui::PushStyleColor(ImGuiCol_Button, primary_button);
    if (ImGui::Button(localized("Use this model", "Utiliser ce modèle", "Usar este modelo"))) identify(candidate);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!broker_.ready() || !artifact || !candidate.empty() || download_ticket_ || browse_ticket_);
    if (ImGui::Button(localized("Download", "Télécharger", "Descargar"))) {
        if (file_request(download_ticket_, {{"type", "download"}, {"path", folder_.u8string()}, {"entry", *artifact}},
                         30min)) {
            download_total_ = artifact->at("bytes").get<uint64_t>();
            downloaded_ = 0;
            download_state_.clear();
        } else download_state_ = "error";
    }
    ImGui::EndDisabled();
    if (download_ticket_) {
        ImGui::ProgressBar(download_total_ ? float(double(downloaded_) / double(download_total_)) : 0.f);
        if (ImGui::Button(localized("Cancel transfer", "Annuler le transfert", "Cancelar descarga")))
            process_.send({{"type", "cancel_download"}});
    } else if (download_state_ == "ok") {
        ImGui::TextUnformatted(localized("Download verified", "Téléchargement vérifié", "Descarga verificada"));
    } else if (download_state_ == "cancelled") {
        ImGui::TextUnformatted(status_label(XSR_CANCELLED));
    } else if (!download_state_.empty()) {
        ImGui::TextWrapped("%s", localized("Could not install the download", "Installation du téléchargement impossible",
                                           "No se pudo instalar la descarga"));
    }
    if (catalog_error_) ImGui::TextWrapped("%s", localized("Invalid model catalog", "Catalogue des modèles invalide",
                                                            "Catálogo de modelos no válido"));
    if (model_error_) ImGui::TextWrapped("%s", status_label(XSR_MODEL_UNAVAILABLE));
    if (profile_recovered_) ImGui::TextWrapped("%s", localized("Restored the last working model",
        "Dernier modèle utilisable restauré", "Se restauró el último modelo funcional"));
    if (!ImGui::TreeNode(localized("Models folder", "Dossier des modèles", "Carpeta de modelos"))) return;
    ImGui::TextWrapped("%s", folder_.u8string().c_str());
    ImGui::BeginDisabled(!broker_.ready() || browse_ticket_ || wav_ticket_ || scan_ticket_ || identify_ticket_);
#ifdef _WIN32
    ImGui::BeginDisabled(bool(download_ticket_));
    if (ImGui::Button(localized("Browse…", "Parcourir…", "Examinar…"))) {
        folder_picker_failed_ = !file_request(browse_ticket_, {{"type", "browse"}}, 10min);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
#endif
    if (ImGui::Button(localized("Rescan", "Actualiser", "Volver a buscar"))) scan();
    ImGui::EndDisabled();
    if (scan_error_) ImGui::TextWrapped("%s", localized("Could not scan the models folder",
        "Impossible d’analyser le dossier des modèles", "No se pudo examinar la carpeta de modelos"));
    if (folder_picker_failed_) ImGui::TextWrapped("%s", localized("Could not open the folder picker",
        "Impossible d’ouvrir le sélecteur de dossier", "No se pudo abrir el selector de carpetas"));
    const bool incompatible = std::any_of(models_.begin(), models_.end(), [](const Json& model) {
        return model.value("state", "") != "candidate";
    });
    if (incompatible && ImGui::TreeNode(localized("Incompatible files", "Fichiers incompatibles", "Archivos incompatibles"))) {
        for (const auto& model : models_) {
            if (model.value("state", "") == "candidate") continue;
            const auto path = fs::u8path(model.at("path").get<std::string>()).filename().u8string();
            ImGui::TextWrapped("%s", path.c_str());
        }
        ImGui::TreePop();
    }
    ImGui::TreePop();
}

std::string Provider::microphone_label() const {
    if (draft_.microphone.empty())
        return localized("System default", "Par défaut du système", "Predeterminado del sistema");
    for (const auto& mic : microphones_)
        if (mic.at("id") == draft_.microphone) return mic.at("name").get<std::string>();
    return localized("Selected microphone missing", "Microphone sélectionné absent", "Falta el micrófono seleccionado");
}

bool Provider::draw_recognition_settings() {
    emphasized_text(localized("Recognition", "Reconnaissance", "Reconocimiento"));
    const char* default_input = localized("System default", "Par défaut du système", "Predeterminado del sistema");
    const auto current = microphone_label();
    ImGui::TextUnformatted(localized("Microphone", "Microphone", "Micrófono"));
    ImGui::BeginDisabled(broker_.busy());
    bool changed = false;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##microphone", current.c_str())) {
        if (ImGui::Selectable(default_input, draft_.microphone.empty())) {
            draft_.microphone.clear();
            changed = true;
        }
        for (const auto& mic : microphones_) {
            auto id = mic.at("id").get<std::string>();
            const auto name = mic.at("name").get<std::string>();
            ImGui::PushID(id.c_str());
            if (ImGui::Selectable(name.c_str(), draft_.microphone == id)) {
                draft_.microphone = id;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const auto& active = broker_.settings().profile;
    if (active.nemotron() || active.qwen()) {
        const char* ids[] = {"auto", "en-US", "fr-FR", "es-ES"};
        const char* languages[] = {localized("Auto", "Automatique", "Automático"), "English", "Français", "Español"};
        int index = 0;
        for (int i = 1; i < 4; ++i) if (draft_.language == ids[i]) index = i;
        ImGui::TextUnformatted(localized("Recognition language", "Langue de reconnaissance", "Idioma de reconocimiento"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##recognition-language", &index, languages, 4)) {
            draft_.language = ids[index];
            changed = true;
        }
    }
    changed |= ImGui::Checkbox(localized("Microphone monitoring", "Retour micro", "Retorno del micrófono"), &draft_.sidetone);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", localized(
        "Play your microphone quietly on the system's default audio output during PTT. Use headphones.",
        "Écouter le micro à faible volume sur la sortie par défaut du système pendant le PTT. Utilise un casque.",
        "Escucha el micrófono a bajo volumen en la salida predeterminada durante el PTT. Usa auriculares."));

    ImGui::BeginDisabled(!active.streaming());
    changed |= ImGui::Checkbox(localized("Early decoding", "Décodage anticipé", "Decodificación anticipada"), &draft_.early);
    ImGui::EndDisabled();
    if (active.nemotron() && draft_.early) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", localized("Vocabulary requires decoding after release.",
            "Le vocabulaire s’applique au décodage après relâchement.",
            "El vocabulario se aplica al decodificar después de soltar."));
        ImGui::PopStyleColor();
    }

    ImGui::EndDisabled();
    return changed;
}

bool Provider::draw_compute_and_memory() {
    const char* heading = localized("Compute and memory", "Calcul et mémoire", "Cálculo y memoria");
    const char* automatic = localized("Applies automatically", "Application automatique", "Aplicación automática");
    const float automatic_width = ImGui::CalcTextSize(automatic).x;
    const float room_for_heading = ImGui::GetContentRegionAvail().x;
    const float automatic_x = ImGui::GetCursorPosX() + room_for_heading - automatic_width;
    emphasized_text(heading);
    if (1.2f * ImGui::CalcTextSize(heading).x + automatic_width + 16 <= room_for_heading)
        ImGui::SameLine(automatic_x);
    ImGui::TextDisabled("%s", automatic);
    ImGui::BeginDisabled(broker_.busy());
    bool changed = false;
    ImGui::TextUnformatted(localized("Compute device", "Périphérique de calcul", "Dispositivo de cálculo"));
    const bool cpu_only = broker_.settings().profile.moonshine_voice();
    const auto compute = cpu_only ? "CPU" : compute_label(draft_.compute);
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(cpu_only || !broker_.ready() || (compute_devices_.empty() && draft_.compute == "cpu"));
    if (ImGui::BeginCombo("##compute", compute.c_str())) {
        if (ImGui::Selectable("CPU", draft_.compute == "cpu")) {
            draft_.compute = "cpu";
            changed = true;
        }
        for (const auto& device : compute_devices_) {
            const auto id = device.at("id").get<std::string>();
            const auto label = device.at("label").get<std::string>();
            ImGui::PushID(id.c_str());
            if (ImGui::Selectable(label.c_str(), draft_.compute == id)) {
                draft_.compute = id;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    int load = int(draft_.load), unload = int(draft_.unload);
    const char* loads[] = {
        localized("On first use", "Au premier usage", "En el primer uso"),
        localized("On activation", "À l'activation", "Al activar")
    };
    const char* unloads[] = {
        localized("After each use", "Après chaque usage", "Después de cada uso"),
        localized("On deactivation", "À la désactivation", "Al desactivar"),
        localized("After inactivity", "Après inactivité", "Tras la inactividad")
    };
    const float room = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const bool two_columns = room >= 315;
    const float combo_width = (room - gap) / 2;
    const float second_column = ImGui::GetCursorPosX() + combo_width + gap;
    const char* unload_label = localized("Unload model", "Décharger", "Liberar memoria");
    ImGui::TextUnformatted(localized("Load model", "Charger", "Cargar"));
    if (two_columns) {
        ImGui::SameLine(second_column);
        ImGui::TextUnformatted(unload_label);
    }
    ImGui::SetNextItemWidth(two_columns ? combo_width : -1);
    changed |= ImGui::Combo("##load", &load, loads, 2);
    if (two_columns) ImGui::SameLine(second_column);
    else ImGui::TextUnformatted(unload_label);
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::Combo("##unload", &unload, unloads, 3);
    draft_.load = static_cast<LoadPolicy>(load);
    draft_.unload = static_cast<UnloadPolicy>(unload);
    if (draft_.unload == UnloadPolicy::after_idle) {
        ImGui::TextUnformatted(localized("Idle time (min)", "Inactivité (min)", "Inactividad (min)"));
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::SliderInt("##idle-minutes", &draft_.idle_minutes, 1, 60);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    const bool restarting = engine_restart_.valid() || waiting_for_hello_;
    ImGui::BeginDisabled(restarting || broker_.busy() || download_ticket_ || identify_ticket_ || browse_ticket_ || wav_ticket_);
    if (ImGui::Button(localized("Restart engine", "Redémarrer le moteur", "Reiniciar el motor"))) restart_engine();
    ImGui::EndDisabled();
    return changed;
}

void Provider::start_test(TestSource source, std::string wav) {
    if (source == TestSource::reference) {
        const auto& settings = broker_.settings();
        const char* file = localized("test-en.wav", "test-fr.wav", "test-es.wav");
        if (settings.profile.nemotron() || settings.profile.qwen()) {
            if (settings.language == "en-US") file = "test-en.wav";
            if (settings.language == "fr-FR") file = "test-fr.wav";
            if (settings.language == "es-ES") file = "test-es.wav";
        } else if (settings.profile.english_only()) {
            file = "test-en.wav";
        }
        wav = (root_ / file).u8string();
    }
    const auto now = Clock::now();
    test_text_.clear();
    test_source_ = source;
    test_latency_ms_.reset();
    wav_picker_failed_ = false;
    test_level_ = 0;
    test_status_ = broker_.begin_local(std::move(wav), now, test_session_);
    add_activity(local_client, test_session_, test_status_, "", source,
                 broker_.model_state() == XSR_MODEL_READY, now);
    test_pending_ = test_status_ == XSR_OK;
    local_held_ = test_pending_ && source == TestSource::microphone;
    listening_ = false;
}

void Provider::draw_test_page() {
    if (ImGui::BeginChild("test-scroll", {}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) {
        if (ImGui::BeginChild("recognition-test", {}, panel_flags)) draw_tests();
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void Provider::draw_tests() {
    emphasized_text(localized("Recognition test", "Test de reconnaissance", "Prueba de reconocimiento"));
    const bool unavailable = !broker_.ready() || broker_.model_state() == XSR_MODEL_MISSING;
    ImGui::BeginDisabled(unavailable || wav_ticket_ || (broker_.busy() && !local_held_));
    ImGui::PushStyleColor(ImGuiCol_Button, primary_button);
    ImGui::Button(localized("Hold to talk", "Maintenir pour parler", "Mantén pulsado para hablar"), {-1, 40});
    ImGui::PopStyleColor();
    if (ImGui::IsItemActivated() && !local_held_) start_test(TestSource::microphone);
    ImGui::EndDisabled();

    ImGui::TextDisabled("%s", localized("Microphone level", "Niveau du microphone", "Nivel del micrófono"));
    ImGui::ProgressBar(local_held_ ? test_level_ : 0.0f, {-1, 7}, "");
    ImGui::BeginDisabled(unavailable || broker_.busy() || wav_ticket_);
    if (ImGui::Button(localized("Test the built-in recording", "Tester l’enregistrement fourni", "Probar la grabación incluida"), {-1, 0}))
        start_test(TestSource::reference);
    ImGui::BeginDisabled(bool(browse_ticket_));
    if (ImGui::Button(localized("Choose a WAV file…", "Choisir un fichier WAV…", "Elegir un archivo WAV…"), {-1, 0})) {
        wav_picker_failed_ = !file_request(wav_ticket_, {{"type", "browse_wav"}}, 10min);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (wav_ticket_)
        ImGui::TextDisabled("%s", localized("Waiting for a file…", "En attente d’un fichier…", "Esperando un archivo…"));
    else if (wav_picker_failed_)
        ImGui::TextWrapped("%s", localized("Could not open the WAV file picker", "Impossible d’ouvrir le sélecteur WAV",
                                          "No se pudo abrir el selector WAV"));

    ImGui::Separator();
    ImGui::TextUnformatted(localized("Transcript", "Transcription", "Transcripción"));
    if (local_held_) {
        ImGui::TextWrapped("%s", listening_ ? localized("Listening…", "Écoute…", "Escuchando…") :
            localized("Starting microphone…", "Démarrage du micro…", "Iniciando micrófono…"));
    } else if (test_pending_) {
        ImGui::TextUnformatted(localized("Transcribing…", "Transcription en cours…", "Transcribiendo…"));
    } else if (!test_text_.empty()) {
        emphasized_text(test_text_.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (test_latency_ms_) {
            char duration[96]{};
            std::snprintf(duration, sizeof(duration), latency_label(test_source_), *test_latency_ms_);
            ImGui::TextWrapped("%s · %s", test_source_label(test_source_), duration);
        } else {
            ImGui::TextWrapped("%s", test_source_label(test_source_));
        }
        ImGui::PopStyleColor();
    } else if (test_status_ != XSR_OK) {
        ImGui::TextWrapped("%s", activity_status(test_status_, XSR_STAGE_NONE, test_source_));
    } else if (unavailable) {
        ImGui::TextWrapped("%s", broker_.ready() ?
            localized("Choose an installed model to start a test.", "Choisis un modèle installé pour lancer un test.",
                      "Elige un modelo instalado para iniciar una prueba.") : status_label(engine_status_));
    } else {
        ImGui::TextDisabled("%s", localized("No result yet", "Aucun résultat pour le moment", "Aún no hay resultados"));
    }
}

void Provider::draw_activity() {
    if (!ImGui::BeginChild("activity-scroll", {}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) {
        ImGui::EndChild();
        return;
    }
    const char* heading = localized("Recent requests", "Requêtes récentes", "Solicitudes recientes");
    const auto subscribers_label = std::string(localized("Subscribers", "Abonnés", "Suscriptores")) +
        " (" + std::to_string(subscribers_.size()) + ")";
    const float room = ImGui::GetContentRegionAvail().x;
    const float button_width = ImGui::CalcTextSize(subscribers_label.c_str()).x + 2 * ImGui::GetStyle().FramePadding.x;
    const float button_x = ImGui::GetCursorPosX() + room - button_width;
    emphasized_text(heading);
    if (1.2f * ImGui::CalcTextSize(heading).x + button_width + 16 <= room) ImGui::SameLine(button_x);
    const auto button = subscribers_label + "###subscribers-menu";
    if (ImGui::Button(button.c_str())) ImGui::OpenPopup("subscriber-list");
    const auto button_corner = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos({button_corner.x, button_corner.y + 6}, ImGuiCond_Appearing, {1, 0});
    ImGui::SetNextWindowSizeConstraints({std::min(222.f, room), 0}, {std::min(380.f, room), 295});
    if (ImGui::BeginPopup("subscriber-list")) {
        if (subscribers_.empty()) {
            ImGui::TextDisabled("%s", localized("No subscribers", "Aucun abonné", "Sin suscriptores"));
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(1.f, std::min(344.f, room - 36)));
        for (const auto& item : subscribers_) ImGui::TextUnformatted(item.second.first.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    draw_recent_requests();
    ImGui::EndChild();
}

void Provider::draw_recent_requests() {
    if (history_.empty())
        ImGui::TextDisabled("%s", localized("No requests", "Aucune requête", "No hay solicitudes"));
    for (const auto& row : history_) {
        ImGui::PushID(static_cast<int>(row.sequence));
        const bool visible = ImGui::BeginChild("request", {}, panel_flags);
        if (visible) {
            const char* source = row.client != local_client ? row.name.c_str() : test_source_label(row.source);
            const char* state = activity_status(row.status, row.stage, row.source);
            if (row.pending)
                state = row.waiting_since ? localized("Transcribing", "Transcription en cours", "Transcribiendo") :
                    localized("Capturing", "Capture en cours", "Grabando");
            else if (row.status == XSR_OK)
                state = localized("Transcribed", "Transcrit", "Transcrito");
            constexpr float source_emphasis = 2;
            const float source_scale = (ImGui::GetFontSize() + source_emphasis) / ImGui::GetFontSize();
            const float source_width = ImGui::CalcTextSize(source).x * source_scale;
            const float state_width = ImGui::CalcTextSize(state).x;
            const float room = ImGui::GetContentRegionAvail().x;
            const float state_x = ImGui::GetCursorPosX() + room - state_width;
            ImGui::PushStyleColor(ImGuiCol_Text, {0.76f, 0.84f, 0.96f, 1});
            emphasized_text(source, source_emphasis);
            ImGui::PopStyleColor();
            if (source_width + state_width + 24 <= room) ImGui::SameLine(state_x);
            const auto state_color = row.pending ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) :
                row.status == XSR_OK ? green_text : amber_text;
            ImGui::PushStyleColor(ImGuiCol_Text, state_color);
            ImGui::TextWrapped("%s", state);
            ImGui::PopStyleColor();
            if (!row.text.empty()) emphasized_text(row.text.c_str());
            if (row.latency_ms) {
                char duration[96]{};
                std::snprintf(duration, sizeof(duration), latency_label(row.source), *row.latency_ms);
                const char* warm = row.model_was_loaded ?
                    localized("Model already loaded", "Modèle déjà chargé", "Modelo ya cargado") :
                    localized("Model unloaded at start", "Modèle non chargé au départ", "Modelo sin cargar al inicio");
                ImGui::TextWrapped("%s · %s", duration, warm);
            }
            if (!row.model.empty()) {
                const char* vocab = row.vocabulary == XSR_VOCAB_APPLIED ?
                    localized("Vocabulary applied", "Vocabulaire appliqué", "Vocabulario aplicado") :
                    row.vocabulary == XSR_VOCAB_UNSUPPORTED ?
                    localized("Vocabulary unsupported", "Vocabulaire non pris en charge", "Vocabulario no compatible") :
                    localized("No vocabulary", "Sans vocabulaire", "Sin vocabulario");
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s %s · %s", model_label(row.model), row.quant.c_str(), vocab);
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
}

std::vector<std::pair<std::string, std::string>> Provider::diagnostic_fields() const {
    const auto& profile = broker_.settings().profile;
    const char* missing = localized("Unavailable", "Indisponible", "No disponible");
    const auto engine_status = broker_.service_ready() ? XSR_OK :
        broker_.ready() ? XSR_ENGINE_RESTARTING : engine_status_;
    const char* model_state = missing;
    switch (broker_.model_state()) {
    case XSR_MODEL_READY: model_state = localized("Loaded", "Chargé", "Cargado"); break;
    case XSR_MODEL_LOADING: model_state = localized("Loading", "Chargement", "Cargando"); break;
    case XSR_MODEL_UNLOADED: model_state = localized("Unloaded", "Déchargé", "Descargado"); break;
    }

    auto actual = broker_.model_state() == XSR_MODEL_READY ? broker_.actual_compute() : "";
    if (actual.empty()) actual = missing;
    else if (broker_.cpu_fallback())
        actual += localized(" (CPU fallback)", " (repli CPU)", " (respaldo CPU)");

    const auto& version = profile.moonshine_voice() ? moonshine_version_ :
        profile.whisper_cpp() ? whisper_version_ : engine_version_;
    const char* inference_engine = profile.moonshine_voice() ? "Moonshine Voice " :
        profile.whisper_cpp() ? "whisper.cpp " : "transcribe.cpp ";
    return {
        {"X-Speech Rekon", XSR_VERSION},
        {"X-Plane / SDK", std::to_string(xplane_version_) + " / " + std::to_string(sdk_version_)},
        {localized("Engine", "Moteur", "Motor"), status_label(engine_status)},
        {localized("Engine version", "Version du moteur", "Versión del motor"),
         version.empty() ? missing : inference_engine + version},
        {localized("Model", "Modèle", "Modelo"),
         profile.available() ? std::string(model_label(profile.id)) + " " + profile.quant : missing},
        {localized("Model state", "État du modèle", "Estado del modelo"), model_state},
        {localized("Selected microphone", "Microphone sélectionné", "Micrófono seleccionado"), microphone_label()},
        {localized("Requested compute", "Calcul demandé", "Cálculo solicitado"), profile.moonshine_voice() ? "CPU" : compute_label(draft_.compute)},
        {localized("Effective compute", "Calcul effectif", "Cálculo efectivo"), std::move(actual)},
        {localized("Settings storage", "Sauvegarde des réglages", "Guardado de ajustes"), writer_.failed() ?
            localized("Write failed, retrying", "Échec, nouvelle tentative", "Error, reintentando") : "OK"},
    };
}

void Provider::draw_diagnostic() {
    if (!ImGui::BeginChild("diagnostic-scroll", {}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) {
        ImGui::EndChild();
        return;
    }
    const auto fields = diagnostic_fields();
    if (clipboard_copy_.valid() && clipboard_copy_.wait_for(0ms) == std::future_status::ready) {
        try { clipboard_result_ = clipboard_copy_.get(); }
        catch (const std::exception&) { clipboard_result_ = false; }
    }
    ImGui::BeginDisabled(clipboard_copy_.valid());
    if (ImGui::Button(localized("Copy diagnostics", "Copier le diagnostic", "Copiar el diagnóstico"))) {
        std::string report;
        for (const auto& [label, value] : fields) report += label + ": " + value + "\n";
        if (!incidents_.empty()) {
            report += "\n";
            report += localized("Recent incidents", "Derniers incidents", "Incidentes recientes");
            report += "\n";
            for (const auto& incident : incidents_) {
                report += incident_time(incident.when) + " " + status_name(incident.status) + " " +
                    activity_status(incident.status, incident.stage, incident.source) + "\n";
            }
        }
        clipboard_result_.reset();
        try {
            clipboard_copy_ = std::async(std::launch::async, [report = std::move(report)] {
                return copy_diagnostic_text(report);
            });
        } catch (const std::system_error&) {
            clipboard_result_ = false;
        }
    }
    ImGui::EndDisabled();
    if (clipboard_result_) {
        const char* feedback = *clipboard_result_ ?
            localized("Copied", "Diagnostic copié", "Diagnóstico copiado") :
            localized("Could not copy to the system clipboard", "Copie dans le presse-papiers impossible",
                      "No se pudo copiar al portapapeles");
        ImGui::TextWrapped("%s", feedback);
    }

    if (ImGui::BeginChild("diagnostic-state", {}, panel_flags)) {
        emphasized_text(localized("Current state", "État actuel", "Estado actual"));
        const float label_width = std::min(174.f, ImGui::GetContentRegionAvail().x * 0.44f);
        if (ImGui::BeginTable("diagnostic-fields", 2, ImGuiTableFlags_NoPadOuterX)) {
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, label_width);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            for (const auto& [label, value] : fields) {
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", label.c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", value.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    if (ImGui::BeginChild("diagnostic-incidents", {}, panel_flags)) {
        emphasized_text(localized("Recent incidents", "Derniers incidents", "Incidentes recientes"));
        if (incidents_.empty()) {
            ImGui::TextDisabled("%s", localized("No incidents", "Aucun incident", "Sin incidentes"));
        }
        for (const auto& incident : incidents_) {
            ImGui::TextDisabled("%s", incident_time(incident.when).c_str());
            ImGui::TextWrapped("%s", activity_status(incident.status, incident.stage, incident.source));
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

std::unique_ptr<Provider> provider;

struct StatusDataRef {
    const char* name;
    XPLMGetDatai_f read;
    XPLMDataRef handle = nullptr;
};

StatusDataRef status_datarefs[] = {
    {XSR_DATAREF_SERVICE_READY, [](void*) -> int { return provider && provider->service_ready(); }},
    {XSR_DATAREF_MODEL_STATE, [](void*) -> int { return provider ? provider->model_state() : XSR_MODEL_MISSING; }},
    {XSR_DATAREF_PTT_BUSY, [](void*) -> int { return provider && provider->ptt_busy(); }},
};
}

PLUGIN_API int XPluginStart(char* name, char* signature, char* description) {
    std::strcpy(name, "X-Speech Rekon");
    std::strcpy(signature, XSR_PROVIDER_SIGNATURE);
    std::strcpy(description, "Local push-to-talk transcription service");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    for (auto& ref : status_datarefs) {
        ref.handle = XPLMRegisterDataAccessor(ref.name, xplmType_Int, 0, ref.read, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr);
        if (!ref.handle) XPLMDebugString("X-Speech Rekon: could not register a status dataref\n");
    }
    return 1;
}

PLUGIN_API void XPluginStop() {
    provider.reset();
    for (auto& ref : status_datarefs) {
        if (ref.handle) XPLMUnregisterDataAccessor(ref.handle);
        ref.handle = nullptr;
    }
}
PLUGIN_API int XPluginEnable() {
    try {
        provider = std::make_unique<Provider>();
        return 1;
    } catch (const std::exception&) {
        XPLMDebugString("X-Speech Rekon: enable failed\n");
        return 0;
    }
}
PLUGIN_API void XPluginDisable() { provider.reset(); }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID sender, int message, void* parameter) {
    if (provider && message == XSR_MESSAGE_REQUEST && parameter) {
        try { provider->request(sender, *static_cast<XsrRequest*>(parameter)); }
        catch (const std::exception&) { XPLMDebugString("X-Speech Rekon: invalid request\n"); }
    }
}
