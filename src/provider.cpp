#include "core.hpp"
#include "activity.hpp"
#include "desktop.hpp"
#include "model_catalog.hpp"
#include "window.hpp"
#include "localization.hpp"

#include <XPLMDataAccess.h>
#include <XPLMMenus.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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

struct ModelName { const char* id; const char* label; };
constexpr ModelName names[] = {
    {"parakeet-tdt-0.6b-v3", "Parakeet v3"},
    {parakeet_v2_id, "Parakeet v2"},
    {nemotron_id, "Nemotron 3.5 Streaming"},
    {whisper_cpp_id, "Whisper Small.en · whisper.cpp"},
    {whisper_transcribe_id, "Whisper Small.en · transcribe.cpp"},
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
    case XSR_OK: return tr("ready");
    case XSR_BUSY: return tr("microphone_in_use");
    case XSR_MODEL_UNAVAILABLE: return tr("model_unavailable");
    case XSR_MIC_UNAVAILABLE: return tr("microphone_unavailable");
    case XSR_ENGINE_LOST: return tr("engine_stopped");
    case XSR_ENGINE_RESTARTING: return tr("engine_starting");
    case XSR_TIMEOUT: return tr("timed_out");
    case XSR_CANCELLED: return tr("cancelled");
    case XSR_NO_AUDIO: return tr("no_audio_captured");
    case XSR_NO_SPEECH: return tr("no_speech_detected");
    case XSR_OUTPUT_TRUNCATED: return tr("transcript_too_long");
    default: return tr("transcription_failed");
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

constexpr auto panel_flags = xsr::settings_panel_flags;
constexpr ImVec4 green_text{0.57f, 0.84f, 0.68f, 1};
constexpr ImVec4 amber_text{0.94f, 0.73f, 0.46f, 1};
constexpr ImVec4 red_text{0.96f, 0.60f, 0.60f, 1};
constexpr ImVec4 primary_button{0.16f, 0.36f, 0.57f, 1};

struct MicrophoneTest {
    uint64_t session = 0;
    uint32_t status = XSR_OK, capture_ms = 0, clipped = 0;
    float level = -120, peak = -120;
    std::optional<float> rms;
    std::string device;
    bool capturing = false, ending = false, has_take = false;
    bool playing = false, playback_failed = false;
};

fs::path plugin_root() {
    char file[1024]{};
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, file, nullptr, nullptr);
    return fs::u8path(file).parent_path().parent_path();
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
    const Profile profile{entry.at("id"), {}, {}, {}, {}};
    return profile.supported() && valid_digest(entry["sha256"].get_ref<const std::string&>()) &&
        std::all_of(entry["urls"].begin(), entry["urls"].end(), [](const Json& url) {
            return url.is_string() && url.get_ref<const std::string&>().rfind("https://", 0) == 0;
        });
}

Profile decode_profile(const Json& value) {
    if (!string_fields(value, {"id", "quant", "revision", "path", "sha256"}) ||
        !valid_digest(value["sha256"].get_ref<const std::string&>())) return {};
    Profile profile{value["id"], value["quant"], value["revision"], value["path"], value["sha256"]};
    return profile.supported() ? profile : Profile{};
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
        return tr("built_in_recording");
    case TestSource::wav:
        return tr("wav_file");
    default:
        return tr("microphone");
    }
}

const char* activity_status(uint32_t status, uint32_t stage, TestSource source = TestSource::microphone) {
    if (status == XSR_TIMEOUT && stage == XSR_STAGE_CAPTURE_START)
        return tr("microphone_start_timed_out");
    if (status == XSR_TIMEOUT && stage == XSR_STAGE_CAPTURE)
        return tr("capture_time_limit_reached");
    if (status == XSR_TIMEOUT && stage == XSR_STAGE_TRANSCRIPTION)
        return tr("transcription_timed_out");
    if (status == XSR_BAD_REQUEST && source != TestSource::microphone)
        return tr("unreadable_wav_or_longer_than_30_seconds");
    return status_label(status);
}

const char* activity_phase_label(ActivityPhase phase) {
    switch (phase) {
    case ActivityPhase::awaiting_audio:
        return tr("starting_microphone_waiting_for_audio");
    case ActivityPhase::capturing:
        return tr("listening_audio_received");
    case ActivityPhase::released_before_audio:
        return tr("ptt_released_capture_unconfirmed");
    case ActivityPhase::transcribing:
        return tr("transcribing");
    }
    return "";
}

const char* latency_label(TestSource source) {
    return source == TestSource::microphone ?
        tr("release_latency") :
        tr("test_latency");
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
    ActivityProgress progress;
    std::optional<uint32_t> latency_ms;
    TestSource source = TestSource::microphone;
    bool pending = false;
    bool model_was_loaded = false;
};

class Provider {
public:
    Provider() : root_(plugin_root()), preferences_(preferences_path()), writer_(preferences_), process_(engine_path(root_)),
        broker_([&](const Json& command) { return process_.send(command); },
                [&](const Delivery& delivery) { pending_events_.push_back(delivery); }),
        window_([&] { draw(); }, 620, 810, root_ / "NotoSans-Regular.ttf", xplm_WindowDecorationRoundRectangle, true) {
        catalog_ = json_file(root_ / "catalog.json");
        catalog_error_ = !catalog_.is_array() || catalog_.empty() ||
            !std::all_of(catalog_.begin(), catalog_.end(), valid_catalog_entry);
        if (catalog_error_) catalog_ = Json::parse(built_in_model_catalog);
        initialize_interface(false);
        load_preferences();
        XPLMHostApplicationID host{};
        XPLMGetVersions(&xplane_version_, &sdk_version_, &host);
        menu_index_ = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "X-Speech Rekon", nullptr, 0);
        menu_ = XPLMCreateMenu("X-Speech Rekon", XPLMFindPluginsMenu(), menu_index_, menu_callback, this);
        settings_item_ = XPLMAppendMenuItem(menu_, tr("settings"), nullptr, 0);
        XPLMRegisterFlightLoopCallback(tick_callback, -1, this);
        if (show_micro_indicator_) update_micro_indicator();
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
            if (subscribers_[sender].second == diagnostic_signature) notify_diagnostic(sender);
        } else if (command.operation == XSR_UNSUBSCRIBE && command.status == XSR_OK) {
            subscribers_.erase(sender);
            cancel_activity(sender);
        } else if (command.operation == XSR_PTT_BEGIN) {
            add_activity(sender, command.session, command.status, plugin_string(sender, false),
                         TestSource::microphone, broker_.model_state() == XSR_MODEL_READY, now);
        } else if (command.operation == XSR_PTT_END && command.status == XSR_OK) {
            if (auto* row = pending_activity(sender, command.session)) row->progress.processing_since = now;
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
        const bool microphone_interrupted = microphone_test_.session != 0;
        microphone_test_ = {};
        if (microphone_interrupted) microphone_test_.status = XSR_ENGINE_LOST;
        last_monitoring_ = {};
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

    void notify_diagnostic(XPLMPluginID id) {
        if (id != XPLM_NO_PLUGIN_ID && XPLMIsPluginEnabled(id)) {
            auto preference = interface_locale().preference();
            XPLMSendMessageToPlugin(id, interface_language_message, preference.data());
        }
    }

    void refresh_language() {
        if (interface_locale().refresh())
            XPLMSetMenuItemName(menu_, settings_item_, tr("settings"), 0);
    }

    float tick() {
        refresh_language();
        const auto now = Clock::now();
        if (microphone_panel_open_ && !window_.visible()) close_microphone_panel();
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
        if (micro_window_ && micro_window_->visible() && !micro_window_->pointer_pressed() &&
            micro_window_->bounds() != micro_bounds_) {
            micro_bounds_ = micro_window_->bounds();
            save_preferences();
        }
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
            const auto session = message.value("session", uint64_t(0));
            const bool level_event = type == "level" || type == "capture";
            if (type == "capture" && session && message.contains("monitoring")) {
                const auto& monitoring = message.at("monitoring");
                if (string_fields(monitoring, {"mode"}) && monitoring.contains("fallback") && monitoring["fallback"].is_boolean() &&
                    monitoring.contains("stream_rate") && monitoring["stream_rate"].is_number_unsigned() &&
                    monitoring.contains("output_rate") && monitoring["output_rate"].is_number_unsigned() &&
                    monitoring.contains("output_period_frames") && monitoring["output_period_frames"].is_number_unsigned())
                    last_monitoring_ = monitoring;
            }
            if (session == test_session_ && level_event)
                test_level_ = message.value("level", 0.0f);
            if (session && session == microphone_test_.session && level_event && microphone_test_.capturing) {
                const auto level = std::clamp(message.value("level", 0.0f), 0.000001f, 1.0f);
                microphone_test_.level = 20.0f * std::log10(level);
                microphone_test_.peak = std::max(microphone_test_.peak, microphone_test_.level);
                microphone_test_.capture_ms = message.at("capture_ms").get<uint32_t>();
                if (type == "capture") microphone_test_.device = message.value("microphone", "");
            }
            if (type == "result" && message.at("status") == XSR_OK) {
                if (message.value("microphone_test", false)) {
                    if (!message.at("capture_ms").is_number_unsigned() || !message.at("clipped").is_number_unsigned() ||
                        !message.at("peak_dbfs").is_number() ||
                        !(message.at("rms_dbfs").is_null() || message.at("rms_dbfs").is_number()))
                        throw std::runtime_error("Invalid microphone test result");
                } else if (!string_fields(message, {"text", "language"}) || !message.at("vocabulary").is_number_unsigned() ||
                           !message.at("capture_ms").is_number_unsigned() || !message.at("compute_ms").is_number_unsigned()) {
                    throw std::runtime_error("Invalid engine result");
                }
            }
            const auto path = broker_.settings().profile.path;
            const auto hash = broker_.settings().profile.sha256;
            broker_.accept(message, now);
            const auto& active = broker_.settings().profile;
            if (active.path != path || active.sha256 != hash) {
                if (!model_picker_edited_) {
                    selected_id_ = active.id;
                    selected_quant_ = active.quant;
                }
                profile_recovered_ = true;
                record_incident(XSR_MODEL_UNAVAILABLE);
            }
        } else if (type == "mic_playback") {
            if (message.value("session", uint64_t(0)) == microphone_test_.session && microphone_test_.playing) {
                microphone_test_.playing = false;
                microphone_test_.playback_failed = !message.at("ok").get<bool>();
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
                if (!model_picker_edited_) {
                    selected_id_ = draft_.profile.id;
                    selected_quant_ = draft_.profile.quant;
                }
                broker_.set_settings(draft_, now);
                save_preferences();
                model_error_ = profile_recovered_ = false;
            } else {
                model_error_ = true;
                record_incident(XSR_MODEL_UNAVAILABLE);
                if (!broker_.settings().profile.available() && broker_.last_good().available()) {
                    draft_.profile = broker_.last_good();
                    broker_.set_settings(draft_, now);
                    if (!model_picker_edited_) {
                        selected_id_ = draft_.profile.id;
                        selected_quant_ = draft_.profile.quant;
                    }
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
            if (event.client == microphone_client) {
                if (event.kind != XSR_RESULT || event.session != microphone_test_.session) continue;
                auto& test = microphone_test_;
                test.capturing = test.ending = false;
                test.status = event.status;
                test.has_take = event.status == XSR_OK;
                if (test.has_take) {
                    test.peak = test.level = event.result.at("peak_dbfs").get<float>();
                    test.capture_ms = event.result.at("capture_ms").get<uint32_t>();
                    test.clipped = event.result.at("clipped").get<uint32_t>();
                    const auto& rms = event.result.at("rms_dbfs");
                    if (!rms.is_null()) test.rms = rms.get<float>();
                }
                continue;
            }
            std::optional<uint32_t> latency;
            if (event.kind == XSR_CAPTURE_STARTED) {
                if (auto* row = pending_activity(event.client, event.session))
                    row->progress.confirm_audio(Clock::now());
            } else if (event.kind == XSR_RESULT) {
                if (auto* row = pending_activity(event.client, event.session)) {
                    row->status = event.status;
                    row->stage = event.stage;
                    row->pending = false;
                    record_incident(event.status, event.stage, row->source);
                    if (event.status == XSR_OK && row->progress.processing_since) {
                        latency = static_cast<uint32_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                Clock::now() - *row->progress.processing_since).count());
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
        row.progress.requested_at = now;
        row.progress.microphone = source == TestSource::microphone;
        if (row.pending && !row.progress.microphone) row.progress.processing_since = now;
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
            const Profile preferred{saved.value("id", selected_id_), saved.value("quant", selected_quant_), {},
                                    saved.value("path", ""), {}};
            if (preferred.supported()) {
                selected_id_ = preferred.id;
                selected_quant_ = preferred.quant;
                startup_path_ = preferred.path;
            }
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
            draft_.low_latency_monitoring = saved.value("low_latency_monitoring", false);
            draft_.monitor_gain_percent = std::clamp(saved.value("monitor_gain_percent", int(XSR_DEFAULT_MONITOR_GAIN_PERCENT)),
                                                    0, int(XSR_MAX_MONITOR_GAIN_PERCENT));
            show_micro_indicator_ = saved.value("show_micro_indicator", false);
            show_micro_requester_ = saved.value("show_micro_requester", false);
            micro_opacity_ = std::clamp(saved.value("micro_opacity", 65), 0, 100);
            micro_border_opacity_ = std::clamp(saved.value("micro_border_opacity", 50), 0, 100);
            micro_bounds_ = saved.value("micro_bounds", Window::Bounds{});
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
        Json saved = {{"interface_language", interface_locale().preference()}, {"folder", folder_.u8string()}, {"path", path}, {"id", id}, {"quant", quant},
            {"microphone", settings.microphone}, {"compute", settings.compute}, {"language", settings.language},
            {"early", settings.early}, {"sidetone", settings.sidetone},
            {"low_latency_monitoring", settings.low_latency_monitoring}, {"monitor_gain_percent", settings.monitor_gain_percent},
            {"load", int(settings.load)},
            {"unload", int(settings.unload)}, {"idle_minutes", settings.idle_minutes},
            {"show_micro_indicator", show_micro_indicator_}, {"show_micro_requester", show_micro_requester_},
            {"micro_opacity", micro_opacity_}, {"micro_border_opacity", micro_border_opacity_}, {"micro_bounds", micro_bounds_}};
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
        return tr("vulkan_unavailable");
    }

    void draw();
    void draw_models();
    bool draw_recognition_settings();
    bool draw_microphone_selector();
    void draw_microphone_panel();
    void draw_microphone_levels();
    void request_microphone_panel();
    void close_microphone_panel();
    void clear_microphone_test();
    bool draw_compute_and_memory();
    void draw_test_page();
    void draw_tests();
    void draw_activity();
    void draw_recent_requests();
    void draw_micro_indicator();
    void update_micro_indicator();
    void draw_micro_settings();
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
    std::unique_ptr<Window> micro_window_;
    Window::Bounds micro_bounds_{};
    int micro_opacity_ = 65, micro_border_opacity_ = 50;
    bool show_micro_indicator_ = false, show_micro_requester_ = false;
    XPLMMenuID menu_ = nullptr;
    int menu_index_ = 0, settings_item_ = 0;
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
    MicrophoneTest microphone_test_;
    bool microphone_panel_open_ = false, microphone_panel_requested_ = false;
    Json last_monitoring_;
    bool model_error_ = false, boot_selection_ = true, waiting_for_hello_ = false, shutting_down_ = false;
    bool model_picker_edited_ = false;
    bool local_held_ = false, test_pending_ = false, listening_ = false;
    bool scan_deferred_ = false, protocol_fault_ = false, profile_recovered_ = false;
    Clock::time_point retry_at_{}, hello_deadline_{}, preferences_retry_at_{};
};

void Provider::draw() {
    refresh_language();
    const auto& active = broker_.settings().profile;
    const auto state = broker_.model_state();
    const char* summary = status_label(broker_.ready() ? XSR_MODEL_UNAVAILABLE : engine_status_);
    ImVec4 color = amber_text;
    if (broker_.ready()) {
        switch (state) {
        case XSR_MODEL_READY:
            summary = tr("model_loaded");
            color = green_text;
            break;
        case XSR_MODEL_LOADING:
            summary = tr("loading_model");
            break;
        case XSR_MODEL_UNLOADED:
            summary = tr("model_ready_to_load");
            color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            break;
        }
    }
    const auto& actual = broker_.actual_compute();
    const auto compute = broker_.cpu_fallback() ? tr("cpu_fallback") :
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
            if (auto* row = pending_activity(local_client, test_session_)) row->progress.processing_since = now;
        } else if (test_pending_) test_status_ = status;
        local_held_ = false;
        listening_ = false;
    }

    xsr::push_settings_style();
    if (ImGui::BeginTabBar("provider-pages", ImGuiTabBarFlags_DrawSelectedOverline)) {
        if (ImGui::BeginTabItem(ui_label("settings"))) {
            draw_settings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ui_label("test"))) {
            draw_test_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ui_label("activity"))) {
            draw_activity();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ui_label("diagnostics"))) {
            draw_diagnostic();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    draw_microphone_panel();
    xsr::pop_settings_style();
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
    if (ImGui::BeginChild("microphone-settings", {}, panel_flags)) {
        window_.section_heading(tr("microphone"));
        ImGui::TextWrapped("%s", microphone_label().c_str());
        if (ImGui::Button(ui_label("configure_microphone")))
            request_microphone_panel();
        ImGui::BeginDisabled(broker_.busy());
        changed |= ImGui::Checkbox(ui_label("microphone_monitoring"), &draft_.sidetone);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("monitoring_tooltip"));
        if (draft_.sidetone) {
            int gain = static_cast<int>(draft_.monitor_gain_percent);
            if (ImGui::SliderInt(ui_label("monitoring_volume"),
                                 &gain, 0, XSR_MAX_MONITOR_GAIN_PERCENT, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
                draft_.monitor_gain_percent = static_cast<uint32_t>(gain);
                changed = true;
            }
            const char* modes[] = {ui_label("standard"),
                                   ui_label("low_latency")};
            int selected = draft_.low_latency_monitoring ? 1 : 0;
            ImGui::TextUnformatted(tr("monitoring_mode"));
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##monitoring-mode", &selected, modes, IM_ARRAYSIZE(modes))) {
                draft_.low_latency_monitoring = selected == 1;
                changed = true;
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    if (changed) {
        draft_.profile = broker_.settings().profile;
        broker_.set_settings(draft_, Clock::now());
        save_preferences();
    }
    if (ImGui::BeginChild("micro-indicator-settings", {}, panel_flags)) draw_micro_settings();
    ImGui::EndChild();
    if (ImGui::BeginChild("interface-settings", {}, panel_flags)) {
        window_.section_heading(tr("interface_language"));
        auto& locale = interface_locale();
        const auto& current = locale.preference();
        const char* preview = current == "auto" ? tr("follow_xplane") : locale.languages().at(current).c_str();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##interface-language", preview)) {
            std::string next;
            if (ImGui::Selectable(ui_label("follow_xplane"), current == "auto")) next = "auto";
            for (const auto& [code, name] : locale.languages()) {
                const auto label = name + "###" + code;
                if (ImGui::Selectable(label.c_str(), current == code)) next = code;
            }
            ImGui::EndCombo();
            if (!next.empty() && next != current) {
                locale.select(next);
                XPLMSetMenuItemName(menu_, settings_item_, tr("settings"), 0);
                notify_diagnostic(XPLMFindPluginBySignature(diagnostic_signature));
                save_preferences();
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

void Provider::update_micro_indicator() {
    if (!show_micro_indicator_) {
        if (micro_window_) micro_window_->hide();
        return;
    }
    constexpr int width = 236;
    const int height = show_micro_requester_ ? 68 : 46;
    Window::Bounds bounds = micro_window_ ? micro_window_->bounds() : micro_bounds_;
    if (bounds == Window::Bounds{}) {
        int left, top, right, bottom;
        XPLMGetScreenBoundsGlobal(&left, &top, &right, &bottom);
        bounds = {left + 24, bottom + 24 + height, left + 24 + width, bottom + 24};
    }
    const auto current = bounds;
    bounds[1] = bounds[3] + height;
    bounds[2] = bounds[0] + width;
    if (!micro_window_) {
        micro_window_ = std::make_unique<Window>([this] { draw_micro_indicator(); }, width, height,
            root_ / "NotoSans-Regular.ttf", xplm_WindowDecorationSelfDecorated);
        micro_window_->set_padding({12.f, 8.f});
        micro_window_->set_scroll_enabled(false);
        micro_window_->set_bounds(bounds);
    } else if (bounds != current) micro_window_->set_bounds(bounds);
    micro_window_->set_drag_area(height);
    micro_window_->set_background_opacity(static_cast<float>(micro_opacity_) / 100.0f);
    micro_window_->set_border_opacity(static_cast<float>(micro_border_opacity_) / 100.0f);
    if (!micro_window_->visible()) micro_window_->show("Rekon");
}

void Provider::draw_micro_settings() {
    window_.section_heading(tr("microphone_indicator"));
    bool changed = ImGui::Checkbox(ui_label("show_in_flight"),
                                   &show_micro_indicator_);
    changed |= ImGui::Checkbox(ui_label("show_requesting_plugin"), &show_micro_requester_);
    changed |= ImGui::SliderInt(ui_label("background_opacity"), &micro_opacity_, 0, 100, "%d%%",
                                ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderInt(ui_label("border_opacity"), &micro_border_opacity_, 0, 100, "%d%%",
                                ImGuiSliderFlags_AlwaysClamp);
    if (changed) {
        update_micro_indicator();
        save_preferences();
    }
}

void Provider::draw_micro_indicator() {
    const auto active = std::find_if(history_.begin(), history_.end(), [](const Activity& row) {
        return row.pending && row.progress.microphone && !row.progress.processing_since;
    });
    const bool listening = active != history_.end() && active->progress.phase() == ActivityPhase::capturing;
    const auto color = active == history_.end() ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) :
        listening ? green_text : amber_text;
    const char* state = active == history_.end() ? tr("idle") :
        listening ? tr("capture") :
        tr("waiting_for_audio");
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const auto ink = ImGui::GetColorU32(color);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRect({at.x + 6, at.y}, {at.x + 14, at.y + 14}, ink, 4.0f, 0, 1.5f);
    draw->PathArcTo({at.x + 10, at.y + 9}, 7.0f, 0.0f, 3.14159265f);
    draw->PathStroke(ink, 0, 1.5f);
    draw->AddLine({at.x + 10, at.y + 16}, {at.x + 10, at.y + 21}, ink, 1.5f);
    draw->AddLine({at.x + 6, at.y + 21}, {at.x + 14, at.y + 21}, ink, 1.5f);
    ImGui::Indent(27);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    emphasized_text(state);
    ImGui::PopStyleColor();
    if (show_micro_requester_ && active != history_.end()) {
        const char* name = active->client == local_client ? test_source_label(active->source) : active->name.c_str();
        ImGui::TextUnformatted(name);
    }
    ImGui::Unindent(27);
}

void Provider::draw_models() {
    window_.section_heading(tr("model"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##model", model_label(selected_id_))) {
        for (const auto& model : names) {
            const Profile choice{model.id, {}, {}, {}, {}};
            if (choice.moonshine_voice() && std::none_of(catalog_.begin(), catalog_.end(),
                    [&](const Json& entry) { return entry.at("id") == model.id; })) continue;
            if (ImGui::Selectable(model.label, selected_id_ == model.id)) {
                selected_id_ = model.id;
                selected_quant_ = choice.moonshine_voice() ? "ORT" : "Q8_0";
                model_picker_edited_ = true;
            }
        }
        ImGui::EndCombo();
    }
    if (selected_id_ == parakeet_v2_id || selected_id_ == whisper_cpp_id || selected_id_ == whisper_transcribe_id) {
        ImGui::TextDisabled("%s", tr("english_only"));
    }
    if (selected_quant_ == "ORT") {
        ImGui::TextDisabled("%s", tr("cpu_english"));
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("quantization"));
        const bool label_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
        ImGui::SameLine(0, 5);
        ImGui::TextDisabled("(?)");
        if ((label_hovered || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) && ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 24 * ImGui::GetFontSize());
            ImGui::TextUnformatted(tr("quantization_tooltip"));
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
            if (ImGui::Button(quant)) {
                selected_quant_ = quant;
                model_picker_edited_ = true;
            }
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
            tr("mb"),
            candidate.empty() ? tr("not_installed") :
                                tr("found_locally"));
        ImGui::PopStyleColor();
    }
    const bool missing_whisper = selected_id_ == whisper_cpp_id && broker_.ready() && whisper_version_.empty();
    if (missing_whisper) ImGui::TextWrapped("%s", tr("whisper_engine_missing_from_this_installation"));
    ImGui::BeginDisabled(!broker_.ready() || missing_whisper || identify_ticket_ || browse_ticket_ || wav_ticket_ ||
                         candidate.empty() || broker_.busy());
    ImGui::PushStyleColor(ImGuiCol_Button, primary_button);
    if (ImGui::Button(ui_label("use_this_model"))) identify(candidate);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!broker_.ready() || !artifact || download_ticket_ || browse_ticket_);
    if (ImGui::Button(ui_label("download"))) {
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
        if (ImGui::Button(ui_label("cancel_transfer")))
            process_.send({{"type", "cancel_download"}});
    } else if (download_state_ == "ok") {
        ImGui::TextUnformatted(tr("download_verified"));
    } else if (download_state_ == "cancelled") {
        ImGui::TextUnformatted(status_label(XSR_CANCELLED));
    } else if (download_state_ == "exists") {
        ImGui::TextWrapped("%s", tr("model_filename_conflict"));
    } else if (!download_state_.empty()) {
        ImGui::TextWrapped("%s", tr("could_not_install_the_download"));
    }
    if (catalog_error_) ImGui::TextWrapped("%s", tr("invalid_model_catalog"));
    if (model_error_) ImGui::TextWrapped("%s", status_label(XSR_MODEL_UNAVAILABLE));
    if (profile_recovered_) ImGui::TextWrapped("%s", tr("restored_the_last_working_model"));
    if (!ImGui::TreeNode(ui_label("models_folder"))) return;
    ImGui::TextWrapped("%s", folder_.u8string().c_str());
    ImGui::BeginDisabled(!broker_.ready() || browse_ticket_ || wav_ticket_ || scan_ticket_ || identify_ticket_);
#ifdef _WIN32
    ImGui::BeginDisabled(bool(download_ticket_));
    if (ImGui::Button(ui_label("browse"))) {
        folder_picker_failed_ = !file_request(browse_ticket_, {{"type", "browse"}}, 10min);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
#endif
    if (ImGui::Button(ui_label("rescan"))) scan();
    ImGui::EndDisabled();
    if (scan_error_) ImGui::TextWrapped("%s", tr("could_not_scan_the_models_folder"));
    if (folder_picker_failed_) ImGui::TextWrapped("%s", tr("could_not_open_the_folder_picker"));
    const bool incompatible = std::any_of(models_.begin(), models_.end(), [](const Json& model) {
        return model.value("state", "") != "candidate";
    });
    if (incompatible && ImGui::TreeNode(ui_label("incompatible_files"))) {
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
        return tr("system_default");
    for (const auto& mic : microphones_)
        if (mic.at("id") == draft_.microphone) return mic.at("name").get<std::string>();
    return tr("selected_microphone_missing");
}

bool Provider::draw_microphone_selector() {
    const char* default_input = ui_label("system_default");
    const auto current = microphone_label();
    bool changed = false;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##microphone", current.c_str())) {
        if (ImGui::Selectable(default_input, draft_.microphone.empty()) && !draft_.microphone.empty()) {
            draft_.microphone.clear();
            changed = true;
        }
        for (const auto& mic : microphones_) {
            auto id = mic.at("id").get<std::string>();
            const auto name = mic.at("name").get<std::string>();
            ImGui::PushID(id.c_str());
            if (ImGui::Selectable(name.c_str(), draft_.microphone == id) && draft_.microphone != id) {
                draft_.microphone = id;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void Provider::request_microphone_panel() {
    microphone_panel_requested_ = true;
    if (broker_.ready()) process_.send({{"type", "microphones"}});
}

void Provider::clear_microphone_test() {
    if (microphone_test_.capturing) broker_.cancel_microphone_test(Clock::now());
    if (microphone_test_.session && broker_.ready())
        process_.send({{"type", "mic_clear"}, {"session", microphone_test_.session}});
    microphone_test_ = {};
}

void Provider::close_microphone_panel() {
    microphone_panel_open_ = false;
    clear_microphone_test();
}

void Provider::draw_microphone_panel() {
    const char* title = ui_label("microphone");
    if (microphone_panel_requested_) {
        microphone_panel_requested_ = false;
        microphone_panel_open_ = true;
        ImGui::OpenPopup(title);
    }
    if (!ImGui::IsPopupOpen(title)) return;
    const auto display = ImGui::GetIO().DisplaySize;
    const float width = std::clamp(display.x - 24, 220.f, 570.f);
    ImGui::SetNextWindowSizeConstraints({width, 0}, {width, std::max(140.f, display.y - 24)});
    bool keep_open = true;
    if (ImGui::BeginPopupModal(title, &keep_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (!microphone_panel_open_ || !keep_open) {
            ImGui::CloseCurrentPopup();
        } else {
            if (ImGui::BeginChild("mic-input", {}, panel_flags)) {
                window_.section_heading(tr("audio_input"));
                ImGui::BeginDisabled(broker_.busy() || microphone_test_.playing);
                if (draw_microphone_selector()) {
                    clear_microphone_test();
                    draft_.profile = broker_.settings().profile;
                    broker_.set_settings(draft_, Clock::now());
                    save_preferences();
                }
                ImGui::BeginDisabled(!broker_.ready());
                if (ImGui::Button(ui_label("refresh_devices")))
                    process_.send({{"type", "microphones"}});
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                if (!microphone_test_.device.empty()) {
                    ImGui::TextWrapped("%s · %s", tr("captured_from"),
                                       microphone_test_.device.c_str());
                } else if (draft_.microphone.empty()) {
                    const auto device = std::find_if(microphones_.begin(), microphones_.end(),
                        [](const Json& item) { return item.value("default", false); });
                    if (device != microphones_.end())
                        ImGui::TextWrapped("%s · %s", tr("detected_default"),
                                           device->at("name").get_ref<const std::string&>().c_str());
                }
            }
            ImGui::EndChild();
            if (ImGui::BeginChild("mic-levels", {}, panel_flags)) draw_microphone_levels();
            ImGui::EndChild();
            if (ImGui::BeginChild("mic-review", {}, panel_flags)) {
                const auto& take = microphone_test_;
                window_.section_heading(tr("listen_to_the_recording"));
                ImGui::BeginDisabled(!take.has_take || (!take.playing && broker_.busy()));
                const auto label = take.playing ? ui_label("stop_playback") :
                    ui_label("play_recording");
                if (ImGui::Button(label)) {
                    const auto sent = process_.send({{"type", take.playing ? "mic_stop" : "mic_play"}, {"session", take.session}});
                    if (!take.playing) microphone_test_.playing = sent;
                    microphone_test_.playback_failed = !sent;
                }
                ImGui::EndDisabled();
                const auto note = take.playback_failed ? tr("audio_output_unavailable") :
                    take.has_take ? tr("recording_retention") :
                    tr("make_a_recording_with_the_guided_test");
                ImGui::TextWrapped("%s", note);
            }
            ImGui::EndChild();
            if (ImGui::Button(ui_label("close"))) {
                keep_open = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (!keep_open || (microphone_panel_open_ && !ImGui::IsPopupOpen(title))) close_microphone_panel();
}

void Provider::draw_microphone_levels() {
    auto& take = microphone_test_;
    window_.section_heading(tr("capture_level"));
    const char* status = status_label(take.status);
    const char* guide = tr("microphone_test_instructions");
    ImVec4 color = take.status == XSR_OK ? green_text : amber_text;
    if (take.capturing) {
        status = take.ending ? tr("finishing") :
            take.capture_ms ? tr("listening") :
            tr("starting_microphone");
        if (take.capture_ms < 2000)
            guide = tr("stay_quiet_for_two_seconds");
        else if (take.capture_ms < 7000)
            guide = tr("speak_normally_as_in_flight");
        else
            guide = tr("speak_a_little_louder_now");
    } else if (take.has_take) {
        color = amber_text;
        if (take.clipped || take.peak >= -2) {
            color = take.clipped ? red_text : amber_text;
            status = take.clipped ? tr("clipping_detected") :
                tr("close_to_clipping");
            guide = tr("lower_input_gain");
        } else if (take.capture_ms < 2200) {
            status = tr("test_too_short");
            guide = tr("repeat_the_test_and_speak_when_prompted");
        } else if (!take.rms) {
            status = status_label(XSR_NO_SPEECH);
            guide = tr("check_input_permissions");
        } else if (*take.rms < -40 || take.peak < -30) {
            status = tr("signal_is_weak");
            guide = tr("raise_input_gain");
        } else {
            color = green_text;
            status = tr("good_level");
            guide = tr("your_voice_has_room_before_clipping");
        }
    } else if (take.status == XSR_MIC_UNAVAILABLE || take.status == XSR_NO_AUDIO) {
        guide = tr("check_input_permissions");
    } else if (broker_.busy()) {
        status = status_label(XSR_BUSY);
        color = amber_text;
    } else if (!broker_.service_ready()) {
        status = status_label(XSR_ENGINE_RESTARTING);
        color = amber_text;
    }
    ImGui::TextColored(color, "%s", status);

    const auto pos = ImGui::GetCursorScreenPos();
    const float width = std::max(1.f, ImGui::GetContentRegionAvail().x);
    constexpr float height = 12.f;
    const auto x = [&](float db) { return pos.x + width * (1 + std::clamp(db, -60.f, 0.f) / 60.f); };
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + width, pos.y + height}, ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
    const float level = take.capturing ? take.level : take.peak;
    const auto segment = [&](float low, float high, ImU32 fill) {
        if (level > low) draw->AddRectFilled({x(low), pos.y}, {x(std::min(high, level)), pos.y + height}, fill);
    };
    segment(-60, -18, IM_COL32(99, 150, 209, 255));
    segment(-18, -6, IM_COL32(111, 195, 155, 255));
    segment(-6, -2, IM_COL32(234, 192, 117, 255));
    segment(-2, 0, IM_COL32(239, 143, 143, 255));
    if (take.peak > -60) draw->AddLine({x(take.peak), pos.y - 2}, {x(take.peak), pos.y + height + 2}, IM_COL32_WHITE, 2);
    ImGui::Dummy({width, height});
    ImGui::TextDisabled("−60");
    ImGui::SameLine(std::max(10.f, ImGui::GetCursorPosX() + width - ImGui::CalcTextSize("0 dBFS").x));
    ImGui::TextDisabled("0 dBFS");

    if (ImGui::BeginTable("mic-stats", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", tr("max_peak"));
        if (take.has_take || take.capture_ms) ImGui::Text("%.1f dBFS", take.peak);
        else ImGui::TextDisabled("—");
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", tr("average_voice_level"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("speech_rms_tooltip"));
        if (take.rms) ImGui::Text("%.1f dBFS", *take.rms);
        else ImGui::TextDisabled("—");
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", tr("clipping"));
        ImGui::TextUnformatted(take.clipped ? tr("yes") : take.has_take ?
            tr("none") : "—");
        ImGui::EndTable();
    }
    ImGui::TextWrapped("%s", guide);
    const bool can_start = broker_.service_ready() && !broker_.busy() && !take.playing;
    ImGui::BeginDisabled(take.capturing ? take.ending : !can_start);
    ImGui::PushStyleColor(ImGuiCol_Button, primary_button);
    if (ImGui::Button(take.capturing ? ui_label("finish_test") :
                                      ui_label("start_test"))) {
        const auto now = Clock::now();
        if (take.capturing) {
            take.ending = broker_.end_microphone_test(take.session, now) == XSR_OK;
        } else {
            clear_microphone_test();
            take.status = broker_.begin_microphone_test(now, take.session);
            take.capturing = take.status == XSR_OK;
        }
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    ImGui::SameLine();
    const auto seconds = microphone_test_seconds - std::min(microphone_test_seconds, take.capture_ms / 1000);
    if (take.capturing) ImGui::TextDisabled(tr("seconds_remaining"), seconds);
    else ImGui::TextDisabled("%u s", microphone_test_seconds);
}

bool Provider::draw_recognition_settings() {
    window_.section_heading(tr("recognition"));
    ImGui::BeginDisabled(broker_.busy());
    bool changed = false;
    const auto& active = broker_.settings().profile;
    if (active.nemotron()) {
        const char* ids[] = {"auto", "en-US", "fr-FR", "es-ES"};
        const char* languages[] = {ui_label("auto"), "English", "Français", "Español"};
        int index = 0;
        for (int i = 1; i < 4; ++i) if (draft_.language == ids[i]) index = i;
        ImGui::TextUnformatted(tr("recognition_language"));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##recognition-language", &index, languages, 4)) {
            draft_.language = ids[index];
            changed = true;
        }
    }
    ImGui::BeginDisabled(!active.streaming());
    changed |= ImGui::Checkbox(ui_label("early_decoding"), &draft_.early);
    ImGui::EndDisabled();
    if (active.nemotron() && draft_.early) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", tr("vocabulary_requires_decoding_after_release"));
        ImGui::PopStyleColor();
    }

    ImGui::EndDisabled();
    return changed;
}

bool Provider::draw_compute_and_memory() {
    const char* heading = tr("compute_and_memory");
    const char* automatic = tr("applies_automatically");
    const float automatic_width = ImGui::CalcTextSize(automatic).x;
    const float room_for_heading = ImGui::GetContentRegionAvail().x;
    const float automatic_x = ImGui::GetCursorPosX() + room_for_heading - automatic_width;
    window_.section_heading(heading);
    if (1.2f * ImGui::CalcTextSize(heading).x + automatic_width + 16 <= room_for_heading)
        ImGui::SameLine(automatic_x);
    ImGui::TextDisabled("%s", automatic);
    ImGui::BeginDisabled(broker_.busy());
    bool changed = false;
    ImGui::TextUnformatted(tr("compute_device"));
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
        ui_label("on_first_use"),
        ui_label("on_activation")
    };
    const char* unloads[] = {
        ui_label("after_each_use"),
        ui_label("on_deactivation"),
        ui_label("after_inactivity")
    };
    const float room = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const bool two_columns = room >= 315;
    const float combo_width = (room - gap) / 2;
    const float second_column = ImGui::GetCursorPosX() + combo_width + gap;
    const char* unload_label = tr("unload_model");
    ImGui::TextUnformatted(tr("load_model"));
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
        ImGui::TextUnformatted(tr("idle_time_min"));
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::SliderInt("##idle-minutes", &draft_.idle_minutes, 1, 60);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    const bool restarting = engine_restart_.valid() || waiting_for_hello_;
    ImGui::BeginDisabled(restarting || broker_.busy() || download_ticket_ || identify_ticket_ || browse_ticket_ || wav_ticket_);
    if (ImGui::Button(ui_label("restart_engine"))) restart_engine();
    ImGui::EndDisabled();
    return changed;
}

void Provider::start_test(TestSource source, std::string wav) {
    if (source == TestSource::reference) {
        const auto& settings = broker_.settings();
        const auto simulator = XPLMGetLanguage();
        const char* file = simulator == xplm_Language_French ? "test-fr.wav" :
                           simulator == xplm_Language_Spanish ? "test-es.wav" : "test-en.wav";
        if (settings.profile.nemotron()) {
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
    window_.section_heading(tr("recognition_test"));
    if (ImGui::Button(ui_label("check_microphone")))
        request_microphone_panel();
    const bool unavailable = !broker_.ready() || broker_.model_state() == XSR_MODEL_MISSING;
    ImGui::BeginDisabled(unavailable || wav_ticket_ || (broker_.busy() && !local_held_));
    ImGui::PushStyleColor(ImGuiCol_Button, primary_button);
    ImGui::Button(ui_label("hold_to_talk"), {-1, 40});
    ImGui::PopStyleColor();
    if (ImGui::IsItemActivated() && !local_held_) start_test(TestSource::microphone);
    ImGui::EndDisabled();

    ImGui::TextDisabled("%s", tr("microphone_level"));
    ImGui::ProgressBar(local_held_ ? test_level_ : 0.0f, {-1, 7}, "");
    ImGui::BeginDisabled(unavailable || broker_.busy() || wav_ticket_);
    if (ImGui::Button(ui_label("test_the_built_in_recording"), {-1, 0}))
        start_test(TestSource::reference);
    ImGui::BeginDisabled(bool(browse_ticket_));
    if (ImGui::Button(ui_label("choose_a_wav_file"), {-1, 0})) {
        wav_picker_failed_ = !file_request(wav_ticket_, {{"type", "browse_wav"}}, 10min);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (wav_ticket_)
        ImGui::TextDisabled("%s", tr("waiting_for_a_file"));
    else if (wav_picker_failed_)
        ImGui::TextWrapped("%s", tr("could_not_open_the_wav_file_picker"));

    ImGui::Separator();
    ImGui::TextUnformatted(tr("transcript"));
    if (local_held_) {
        ImGui::TextWrapped("%s", listening_ ? tr("provider_listening") :
            tr("starting_microphone"));
    } else if (test_pending_) {
        ImGui::TextUnformatted(tr("test_transcribing"));
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
            tr("test_requires_model") : status_label(engine_status_));
    } else {
        ImGui::TextDisabled("%s", tr("no_result_yet"));
    }
}

void Provider::draw_activity() {
    if (!ImGui::BeginChild("activity-scroll", {}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) {
        ImGui::EndChild();
        return;
    }
    const char* heading = tr("recent_requests");
    const auto subscribers_label = std::string(tr("subscribers")) +
        " (" + std::to_string(subscribers_.size()) + ")";
    const float room = ImGui::GetContentRegionAvail().x;
    const float button_width = ImGui::CalcTextSize(subscribers_label.c_str()).x + 2 * ImGui::GetStyle().FramePadding.x;
    const float button_x = ImGui::GetCursorPosX() + room - button_width;
    window_.section_heading(heading);
    if (1.2f * ImGui::CalcTextSize(heading).x + button_width + 16 <= room) ImGui::SameLine(button_x);
    const auto button = subscribers_label + "###subscribers-menu";
    if (ImGui::Button(button.c_str())) ImGui::OpenPopup("subscriber-list");
    const auto button_corner = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos({button_corner.x, button_corner.y + 6}, ImGuiCond_Appearing, {1, 0});
    ImGui::SetNextWindowSizeConstraints({std::min(222.f, room), 0}, {std::min(380.f, room), 295});
    if (ImGui::BeginPopup("subscriber-list")) {
        if (subscribers_.empty()) {
            ImGui::TextDisabled("%s", tr("no_subscribers"));
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
        ImGui::TextDisabled("%s", tr("no_requests"));
    for (const auto& row : history_) {
        ImGui::PushID(static_cast<int>(row.sequence));
        const bool visible = ImGui::BeginChild("request", {}, panel_flags);
        if (visible) {
            const char* source = row.client != local_client ? row.name.c_str() : test_source_label(row.source);
            const auto phase = row.progress.phase();
            const char* state = activity_status(row.status, row.stage, row.source);
            if (row.pending) state = activity_phase_label(phase);
            else if (row.status == XSR_OK)
                state = tr("transcribed");
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
            ImVec4 state_color = row.status == XSR_OK ? green_text : amber_text;
            if (row.pending && phase == ActivityPhase::transcribing)
                state_color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            else if (row.pending && phase != ActivityPhase::capturing)
                state_color = amber_text;
            ImGui::PushStyleColor(ImGuiCol_Text, state_color);
            ImGui::TextWrapped("%s", state);
            ImGui::PopStyleColor();
            if (!row.text.empty()) emphasized_text(row.text.c_str());
            if (row.progress.capture_delay_ms) {
                ImGui::TextDisabled(tr("first_audio_delay"), *row.progress.capture_delay_ms);
            } else if (!row.pending && phase == ActivityPhase::released_before_audio) {
                ImGui::TextDisabled("%s", activity_phase_label(phase));
            }
            if (row.latency_ms) {
                char duration[96]{};
                std::snprintf(duration, sizeof(duration), latency_label(row.source), *row.latency_ms);
                const char* warm = row.model_was_loaded ?
                    tr("model_already_loaded") :
                    tr("model_unloaded_at_start");
                ImGui::TextWrapped("%s · %s", duration, warm);
            }
            if (!row.model.empty()) {
                const char* vocab = row.vocabulary == XSR_VOCAB_APPLIED ?
                    tr("vocabulary_applied") :
                    row.vocabulary == XSR_VOCAB_UNSUPPORTED ?
                    tr("vocabulary_unsupported") :
                    tr("no_vocabulary");
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
    const char* missing = tr("unavailable");
    const auto engine_status = broker_.service_ready() ? XSR_OK :
        broker_.ready() ? XSR_ENGINE_RESTARTING : engine_status_;
    const char* model_state = missing;
    switch (broker_.model_state()) {
    case XSR_MODEL_READY: model_state = tr("loaded"); break;
    case XSR_MODEL_LOADING: model_state = tr("loading"); break;
    case XSR_MODEL_UNLOADED: model_state = tr("unloaded"); break;
    }

    auto actual = broker_.model_state() == XSR_MODEL_READY ? broker_.actual_compute() : "";
    if (actual.empty()) actual = missing;
    else if (broker_.cpu_fallback())
        actual += tr("cpu_fallback_suffix");

    const auto& version = profile.moonshine_voice() ? moonshine_version_ :
        profile.whisper_cpp() ? whisper_version_ : engine_version_;
    const char* inference_engine = profile.moonshine_voice() ? "Moonshine Voice " :
        profile.whisper_cpp() ? "whisper.cpp " : "transcribe.cpp ";
    std::string monitoring = missing;
    if (last_monitoring_.is_object()) {
        const auto mode = last_monitoring_.at("mode").get<std::string>();
        if (mode == "capture_only") {
            monitoring = tr("capture_only_monitoring_unavailable");
        } else {
            monitoring = mode == "low_latency" ? tr("low_latency") :
                last_monitoring_.at("fallback").get<bool>() ? tr("standard_fallback") :
                tr("standard");
            const auto output_rate = last_monitoring_.at("output_rate").get<uint32_t>();
            const auto frames = last_monitoring_.at("output_period_frames").get<uint32_t>();
            char detail[112];
            std::snprintf(detail, sizeof(detail), tr("monitoring_stream_details"),
                last_monitoring_.at("stream_rate").get<uint32_t>(), output_rate, frames);
            monitoring += detail;
        }
    }
    return {
        {"X-Speech Rekon", XSR_VERSION},
        {"X-Plane / SDK", std::to_string(xplane_version_) + " / " + std::to_string(sdk_version_)},
        {tr("engine"), status_label(engine_status)},
        {tr("engine_version"),
         version.empty() ? missing : inference_engine + version},
        {tr("model"),
         profile.available() ? std::string(model_label(profile.id)) + " " + profile.quant : missing},
        {tr("model_state"), model_state},
        {tr("selected_microphone"), microphone_label()},
        {tr("last_microphone_monitoring"), std::move(monitoring)},
        {tr("requested_compute"), profile.moonshine_voice() ? "CPU" : compute_label(draft_.compute)},
        {tr("effective_compute"), std::move(actual)},
        {tr("settings_storage"), writer_.failed() ?
            tr("write_failed_retrying") : "OK"},
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
    if (ImGui::Button(ui_label("copy_diagnostics"))) {
        std::string report;
        for (const auto& [label, value] : fields) report += label + ": " + value + "\n";
        if (!incidents_.empty()) {
            report += "\n";
            report += tr("recent_incidents");
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
            tr("copied") :
            tr("could_not_copy_to_the_system_clipboard");
        ImGui::TextWrapped("%s", feedback);
    }

    if (ImGui::BeginChild("diagnostic-state", {}, panel_flags)) {
        window_.section_heading(tr("current_state"));
        const float label_width = std::min(174.f, ImGui::GetContentRegionAvail().x * 0.44f);
        if (ImGui::BeginTable("diagnostic-fields", 2, ImGuiTableFlags_NoPadOuterX)) {
            ImGui::TableSetupColumn(ui_label("field"), ImGuiTableColumnFlags_WidthFixed, label_width);
            ImGui::TableSetupColumn(ui_label("value"), ImGuiTableColumnFlags_WidthStretch);
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
        window_.section_heading(tr("recent_incidents"));
        if (incidents_.empty()) {
            ImGui::TextDisabled("%s", tr("no_incidents"));
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
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    initialize_interface(false);
    copy_plugin_description(description, "provider_description");
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
