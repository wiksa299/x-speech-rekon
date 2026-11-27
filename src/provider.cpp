#include "provider.hpp"

#include "localization.hpp"

#include <XPLMDataAccess.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace xsr {
namespace {
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

std::string plugin_string(int id, bool signature) {
    char name[256]{}, sig[256]{};
    XPLMGetPluginInfo(id, name, nullptr, sig, nullptr);
    return signature ? sig : name;
}
} // namespace

Provider::Provider() : root_(plugin_root()), preferences_(preferences_path()), writer_(preferences_), process_(engine_path(root_)),
    broker_([&](const Json& command) { return process_.send(command); },
            [&](const Delivery& delivery) { pending_events_.push_back(delivery); }),
    window_([&] { draw(); }, 620, 810, root_ / "NotoSans-Regular.ttf", xplm_WindowDecorationRoundRectangle, true) {
    load_catalog();
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

Provider::~Provider() {
    shutting_down_ = true;
    XPLMUnregisterFlightLoopCallback(tick_callback, this);
    if (engine_restart_.valid()) engine_restart_.wait();
    broker_.shutdown(Clock::now());
    dispatch_events();
    process_.stop();
    XPLMDestroyMenu(menu_);
    XPLMRemoveMenuItem(XPLMFindPluginsMenu(), menu_index_);
}

void Provider::request(int sender, XsrRequest& command) {
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

void Provider::menu_callback(void* owner, void*) {
    auto& self = *static_cast<Provider*>(owner);
    self.window_.show("X-Speech Rekon");
    if (self.broker_.ready()) self.process_.send({{"type", "microphones"}});
}

float Provider::tick_callback(float, float, int, void* owner) {
    auto& self = *static_cast<Provider*>(owner);
    try { return self.tick(); }
    catch (const std::exception&) {
        XPLMDebugString("X-Speech Rekon: provider update failed\n");
        self.protocol_fault_ = true;
        return 0.05f;
    }
}

void Provider::record_incident(uint32_t status, uint32_t stage, TestSource source) {
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

void Provider::engine_started(bool started, Clock::time_point now) {
    waiting_for_hello_ = started;
    if (started) {
        hello_deadline_ = now + 5s;
    } else {
        retry_at_ = now + 2s;
        if (engine_status_ != XSR_ENGINE_LOST) record_incident(XSR_ENGINE_LOST);
        engine_status_ = XSR_ENGINE_LOST;
    }
}

void Provider::restart_engine(bool recovery) {
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

void Provider::clear_file_requests() {
    if (scan_ticket_) scan_error_ = true;
    if (identify_ticket_) model_error_ = true;
    if (browse_ticket_) folder_picker_failed_ = true;
    if (wav_ticket_) wav_picker_failed_ = true;
    if (download_ticket_) download_state_ = "error";
    scan_ticket_ = identify_ticket_ = browse_ticket_ = wav_ticket_ = download_ticket_ = {};
    scan_deferred_ = false;
}

void Provider::notify_diagnostic(XPLMPluginID id) {
    if (id != XPLM_NO_PLUGIN_ID && XPLMIsPluginEnabled(id)) {
        auto preference = interface_locale().preference();
        XPLMSendMessageToPlugin(id, interface_language_message, preference.data());
    }
}

void Provider::refresh_language() {
    if (interface_locale().refresh())
        XPLMSetMenuItemName(menu_, settings_item_, tr("settings"), 0);
}

float Provider::tick() {
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

void Provider::on_engine_event(const Json& message, Clock::time_point now) {
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

void Provider::dispatch_events() {
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

void Provider::add_activity(int client, uint64_t session, uint32_t status, std::string name,
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

Activity* Provider::pending_activity(int client, uint64_t session) {
    for (auto& row : history_)
        if (row.pending && row.client == client && row.session == session) return &row;
    return nullptr;
}

void Provider::cancel_activity(int client) {
    for (auto& row : history_) {
        if (!row.pending || row.client != client) continue;
        row.pending = false;
        row.status = XSR_CANCELLED;
    }
}

bool Provider::file_request(FileRequest& pending, Json command, Clock::duration timeout) {
    const auto ticket = ++next_ticket_;
    command["ticket"] = ticket;
    if (!process_.send(command)) {
        record_incident(XSR_ENGINE_RESTARTING);
        return false;
    }
    pending = {ticket, Clock::now() + timeout};
    return true;
}

void Provider::scan() {
    scan_deferred_ = bool(scan_ticket_) || bool(browse_ticket_) || bool(wav_ticket_);
    if (scan_deferred_) return;
    scan_error_ = !file_request(scan_ticket_, {{"type", "scan"}, {"path", folder_.u8string()},
                                              {"catalog", catalog_}}, 90s);
}

void Provider::identify(const std::string& path) {
    profile_recovered_ = false;
    model_error_ = !file_request(identify_ticket_, {{"type", "identify"}, {"path", path},
                                                   {"catalog", catalog_}}, 3min);
}

std::string Provider::candidate_path() const {
    for (const auto& model : models_) {
        if (model.value("state", "") == "candidate" && model.value("id", "") == selected_id_ &&
            model.value("quant", "") == selected_quant_)
            return model.at("path").get<std::string>();
    }
    return {};
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

} // namespace xsr

namespace {
std::unique_ptr<xsr::Provider> provider;

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
    xsr::initialize_interface(false);
    xsr::copy_plugin_description(description, "provider_description");
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
        provider = std::make_unique<xsr::Provider>();
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
