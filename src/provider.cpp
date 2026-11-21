#include "core.hpp"
#include "window.hpp"

#include <XPLMMenus.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
using namespace xvr;
using xvr::localized;

struct ModelName { const char* id; const char* label; };
constexpr ModelName names[] = {
    {"parakeet-tdt-0.6b-v3", "Parakeet v3"},
    {nemotron_id, "Nemotron 3.5 Streaming"},
    {"moonshine-streaming-tiny", "Moonshine Tiny"},
    {"moonshine-streaming-small", "Moonshine Small"},
    {"moonshine-streaming-medium", "Moonshine Medium"},
};

const char* model_label(const std::string& id) {
    for (const auto& model : names) if (id == model.id) return model.label;
    return "—";
}

const char* status_label(uint32_t code) {
    switch (code) {
    case XVR_OK: return localized("Ready", "Prêt", "Listo");
    case XVR_BUSY: return localized("Microphone in use", "Microphone occupé", "Micrófono ocupado");
    case XVR_MODEL_UNAVAILABLE: return localized("Model unavailable", "Modèle indisponible", "Modelo no disponible");
    case XVR_MIC_UNAVAILABLE: return localized("Microphone unavailable", "Microphone indisponible", "Micrófono no disponible");
    case XVR_ENGINE_LOST: return localized("Engine stopped", "Moteur arrêté", "Motor detenido");
    case XVR_ENGINE_RESTARTING: return localized("Engine starting", "Moteur en démarrage", "Iniciando el motor");
    case XVR_TIMEOUT: return localized("Timed out", "Délai dépassé", "Tiempo agotado");
    case XVR_CANCELLED: return localized("Cancelled", "Annulé", "Cancelado");
    case XVR_NO_AUDIO: return localized("No audio captured", "Aucun son capturé", "No se ha capturado audio");
    case XVR_NO_SPEECH: return localized("No speech detected", "Aucune parole détectée", "No se ha detectado habla");
    case XVR_OUTPUT_TRUNCATED: return localized("Transcript too long", "Transcription trop longue", "Transcripción demasiado larga");
    default: return localized("Transcription failed", "Échec de la transcription", "Error de transcripción");
    }
}

void section_heading(const char* title) {
    const auto pos = ImGui::GetCursorScreenPos();
    const float size = ImGui::GetFontSize() + 3;
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), size, pos, ImGui::GetColorU32(ImGuiCol_Text), title);
    ImGui::Dummy({0, size});
    ImGui::Separator();
}

fs::path plugin_root() {
    char file[1024]{};
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, file, nullptr, nullptr);
    return fs::u8path(file).parent_path().parent_path();
}

fs::path preferences() {
    char path[1024]{};
    XPLMGetPrefsPath(path);
    return fs::u8path(path).parent_path() / "x-voice-rekon.json";
}

fs::path engine_path(const fs::path& root) {
#ifdef _WIN32
    return root / "bin" / "xvr-engine.exe";
#else
    return root / "bin" / "xvr-engine";
#endif
}

Json json_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return file ? Json::parse(file, nullptr, false) : Json();
}

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
            if (output.fail()) continue;
#ifdef _WIN32
            MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
            std::error_code error;
            fs::rename(temporary, path_, error);
#endif
        }
    }
    fs::path path_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<std::string> pending_;
    bool stopping_ = false;
    std::thread worker_;
};

std::string plugin_string(int id, bool signature) {
    char name[256]{}, sig[256]{};
    XPLMGetPluginInfo(id, name, nullptr, sig, nullptr);
    return signature ? sig : name;
}

struct Activity {
    std::string name;
    uint64_t id;
    uint32_t status;
    uint32_t vocabulary = XVR_VOCAB_NONE;
    std::string model;
    bool pending = false;
};

class Provider {
public:
    Provider() : root_(plugin_root()), preferences_(preferences()), writer_(preferences_), process_(engine_path(root_)),
        broker_([&](const Json& command) { return process_.send(command); },
                [&](const Delivery& delivery) { pending_events_.push_back(delivery); }),
        window_([&] { draw(); }, 890, 760, root_ / "NotoSans-Regular.ttf") {
        catalog_ = json_file(root_ / "catalog.json");
        if (!catalog_.is_array()) catalog_ = Json::array();
        load_preferences();
        menu_index_ = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "X-Voice-Rekon", nullptr, 0);
        menu_ = XPLMCreateMenu("X-Voice-Rekon", XPLMFindPluginsMenu(), menu_index_, menu_callback, this);
        XPLMAppendMenuItem(menu_, localized("Settings", "Réglages", "Ajustes"), nullptr, 0);
        XPLMRegisterFlightLoopCallback(tick_callback, -1, this);
    }

    ~Provider() {
        shutting_down_ = true;
        XPLMUnregisterFlightLoopCallback(tick_callback, this);
        broker_.shutdown(Clock::now());
        dispatch_events();
        process_.stop();
        XPLMDestroyMenu(menu_);
        XPLMRemoveMenuItem(XPLMFindPluginsMenu(), menu_index_);
    }

    void request(int sender, XvrRequest& command) {
        if (shutting_down_ || sender < 0 || !XPLMIsPluginEnabled(sender)) return;
        broker_.request(sender, command, Clock::now());
        if (command.struct_size < sizeof(command) || command.version != XVR_ABI_VERSION) return;
        if (command.operation == XVR_SUBSCRIBE && command.status == XVR_OK) {
            subscribers_[sender] = {plugin_string(sender, false), plugin_string(sender, true)};
        } else if (command.operation == XVR_UNSUBSCRIBE && command.status == XVR_OK) {
            subscribers_.erase(sender);
        } else if (command.operation == XVR_PTT_BEGIN) {
            history_.push_front({plugin_string(sender, false), command.session, command.status,
                                 XVR_VOCAB_NONE, "", command.status == XVR_OK});
            if (history_.size() > 12) history_.pop_back();
        }
    }

private:
    static void menu_callback(void* owner, void*) {
        auto& self = *static_cast<Provider*>(owner);
        self.window_.show();
        if (self.broker_.ready()) self.process_.send({{"type", "microphones"}});
    }

    static float tick_callback(float, float, int, void* owner) {
        return static_cast<Provider*>(owner)->tick();
    }

    float tick() {
        const auto now = Clock::now();
        for (auto it = subscribers_.begin(); it != subscribers_.end();) {
            if (!XPLMIsPluginEnabled(it->first) || plugin_string(it->first, true) != it->second.second) {
                broker_.forget(it->first, now);
                it = subscribers_.erase(it);
            } else {
                ++it;
            }
        }
        broker_.tick(now);
        if (process_.failed() || broker_.restart_due(now) ||
            (waiting_for_hello_ && now > hello_deadline_)) {
            if (broker_.ready() || waiting_for_hello_) {
                broker_.engine_lost(now);
                process_.stop();
                scan_ticket_ = identify_ticket_ = browse_ticket_ = download_ticket_ = 0;
                waiting_for_hello_ = false;
                engine_status_ = XVR_ENGINE_LOST;
                retry_at_ = now + 500ms;
            }
            if (now >= retry_at_) {
                if (process_.start()) {
                    waiting_for_hello_ = true;
                    hello_deadline_ = now + 5s;
                } else {
                    retry_at_ = now + 2s;
                }
            }
        }
        for (const auto& message : process_.receive()) on_engine_event(message, now);
        if (local_held_ && !window_.visible()) {
            broker_.cancel_local(now);
            local_held_ = false;
        }
        dispatch_events();
        const auto& profile = broker_.settings().profile;
        if (profile.path != persisted_profile_) save_preferences();
        return broker_.busy() ? -1.0f : 0.05f;
    }

    void on_engine_event(const Json& message, Clock::time_point now) {
        const auto type = message.value("type", "");
        const auto ticket = message.value("ticket", uint64_t(0));
        if (type == "hello") {
            if (message.value("protocol", 0) != 1) return;
            waiting_for_hello_ = false;
            engine_status_ = XVR_OK;
            compute_devices_ = message.value("devices", Json::array());
            broker_.engine_online(now);
            scan();
            if (window_.visible()) process_.send({{"type", "microphones"}});
        } else if (type == "capture" || type == "level" || type == "result" || type == "state") {
            if (message.value("session", uint64_t(0)) == test_session_ && (type == "level" || type == "capture"))
                test_level_ = message.value("level", 0.0f);
            broker_.accept(message, now);
        } else if (type == "microphones") {
            microphones_ = message.at("items");
        } else if (type == "scan" && ticket && ticket == scan_ticket_) {
            scan_ticket_ = 0;
            models_ = message.value("items", Json::array());
            if (boot_selection_) {
                boot_selection_ = false;
                std::string candidate;
                if (!startup_path_.empty()) {
                    for (const auto& file : models_) {
                        if (file.value("state", "") == "candidate" && file.value("path", "") == startup_path_)
                            candidate = startup_path_;
                    }
                }
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
        } else if (type == "identify" && ticket && ticket == identify_ticket_) {
            identify_ticket_ = 0;
            auto file = message.value("model", Json{});
            if (file.is_object()) {
                draft_.profile = {file.at("id"), file.at("quant"), file.at("revision"),
                                  file.at("path"), file.at("sha256")};
                selected_id_ = draft_.profile.id;
                selected_quant_ = draft_.profile.quant;
                broker_.set_settings(draft_, now);
                save_preferences();
                model_error_ = false;
            } else {
                model_error_ = true;
            }
        } else if (type == "browse" && ticket && ticket == browse_ticket_) {
            browse_ticket_ = 0;
            const auto path = message.value("path", "");
            if (!path.empty() && path != folder_.u8string()) {
                folder_ = fs::u8path(path);
                models_ = Json::array();
                save_preferences();
                scan();
            }
        } else if (type == "download_progress" && ticket && ticket == download_ticket_) {
            downloaded_ = message.value("bytes", uint64_t(0));
        } else if (type == "download" && ticket && ticket == download_ticket_) {
            download_ticket_ = 0;
            download_state_ = message.value("status", "error");
            scan();
        }
    }

    void dispatch_events() {
        std::vector<Delivery> events;
        events.swap(pending_events_);
        for (const auto& event : events) {
            if (event.client == local_client) {
                if (event.kind == XVR_CAPTURE_STARTED) listening_ = true;
                if (event.kind == XVR_RESULT) {
                    listening_ = false;
                    test_pending_ = false;
                    test_level_ = 0;
                    test_status_ = event.status;
                    test_text_ = event.result.is_object() ? event.result.value("text", "") : "";
                    compute_ms_ = event.result.is_object() ? event.result.value("compute_ms", 0u) : 0u;
                }
                continue;
            }
            std::string text, model, revision, quant, language;
            uint32_t vocab = XVR_VOCAB_NONE, capture_ms = 0, compute_ms = 0;
            if (event.status == XVR_OK && event.kind == XVR_RESULT) {
                text = event.result.value("text", "");
                model = event.result.value("id", "");
                revision = event.result.value("revision", "");
                quant = event.result.value("quant", "");
                language = event.result.value("language", "");
                vocab = event.result.value("vocabulary", uint32_t(XVR_VOCAB_NONE));
                capture_ms = event.result.value("capture_ms", 0u);
                compute_ms = event.result.value("compute_ms", 0u);
            }
            if (event.kind == XVR_RESULT) {
                for (auto& row : history_) {
                    if (row.id == event.session) {
                        row.status = event.status;
                        row.pending = false;
                        row.model = model;
                        row.vocabulary = vocab;
                        break;
                    }
                }
                const auto log = "X-Voice-Rekon PTT " + status_name(event.status) + " " + model + " " +
                    std::to_string(capture_ms) + "ms audio " + std::to_string(compute_ms) + "ms compute\n";
                XPLMDebugString(log.c_str());
            }
            if (!subscribers_.count(event.client) || !XPLMIsPluginEnabled(event.client)) continue;
            XvrEvent outgoing{XVR_ABI_VERSION, sizeof(XvrEvent), event.kind, event.status, event.session,
                vocab, event.stage, capture_ms, compute_ms, text.c_str(), model.c_str(), revision.c_str(),
                quant.c_str(), language.c_str()};
            XPLMSendMessageToPlugin(event.client, XVR_MESSAGE_EVENT, &outgoing);
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
            draft_.microphone = saved.value("microphone", "");
            draft_.compute = saved.value("compute", "cpu");
            draft_.language = saved.value("language", "auto");
            if (draft_.language != "en-US" && draft_.language != "fr-FR" && draft_.language != "es-ES")
                draft_.language = "auto";
            draft_.early = saved.value("early", false);
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
        const Json saved = {{"folder", folder_.u8string()}, {"path", path}, {"id", id}, {"quant", quant},
            {"microphone", settings.microphone}, {"compute", settings.compute}, {"language", settings.language},
            {"early", settings.early}, {"load", int(settings.load)},
            {"unload", int(settings.unload)}, {"idle_minutes", settings.idle_minutes}};
        writer_.save(saved.dump(2, ' ', false, Json::error_handler_t::replace));
    }

    void scan() {
        const auto ticket = ++next_ticket_;
        if (process_.send({{"type", "scan"}, {"ticket", ticket}, {"path", folder_.u8string()}, {"catalog", catalog_}}))
            scan_ticket_ = ticket;
    }

    void identify(const std::string& path) {
        const auto ticket = ++next_ticket_;
        if (process_.send({{"type", "identify"}, {"ticket", ticket}, {"path", path}, {"catalog", catalog_}}))
            identify_ticket_ = ticket;
    }

    std::string candidate_path() const {
        for (const auto& model : models_) {
            if (model.value("id", "") == selected_id_ && model.value("quant", "") == selected_quant_)
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
    void draw_policies();
    void draw_tests();
    void draw_activity();
    void start_test(bool sample);

    fs::path root_, preferences_, folder_;
    PreferencesWriter writer_;
    Process process_;
    Broker broker_;
    Window window_;
    XPLMMenuID menu_ = nullptr;
    int menu_index_ = 0;
    std::map<int, std::pair<std::string, std::string>> subscribers_;
    std::deque<Activity> history_;
    std::vector<Delivery> pending_events_;
    Json catalog_ = Json::array(), models_ = Json::array(), microphones_ = Json::array(), compute_devices_ = Json::array();
    Settings draft_;
    std::string selected_id_ = "parakeet-tdt-0.6b-v3", selected_quant_ = "Q8_0";
    std::string startup_path_, persisted_profile_, download_state_, test_text_;
    uint64_t next_ticket_ = 0, scan_ticket_ = 0, identify_ticket_ = 0, browse_ticket_ = 0, download_ticket_ = 0;
    uint64_t downloaded_ = 0, download_total_ = 0, test_session_ = 0;
    uint32_t engine_status_ = XVR_ENGINE_RESTARTING, test_status_ = XVR_OK, compute_ms_ = 0;
    float test_level_ = 0;
    bool model_error_ = false, boot_selection_ = true, waiting_for_hello_ = false, shutting_down_ = false;
    bool local_held_ = false, test_pending_ = false, listening_ = false, test_is_sample_ = false;
    Clock::time_point retry_at_{}, hello_deadline_{};
};

void Provider::draw() {
    const auto& active = broker_.settings().profile;
    const auto state = broker_.model_state();
    const char* summary = status_label(broker_.ready() ? XVR_MODEL_UNAVAILABLE : engine_status_);
    ImVec4 color = {0.94f, 0.71f, 0.42f, 1};
    if (broker_.ready()) {
        switch (state) {
        case XVR_MODEL_READY:
            summary = localized("Model loaded", "Modèle chargé", "Modelo cargado");
            color = {0.58f, 0.87f, 0.68f, 1};
            break;
        case XVR_MODEL_LOADING:
            summary = localized("Loading model", "Chargement du modèle", "Cargando el modelo");
            break;
        case XVR_MODEL_UNLOADED:
            summary = localized("Model ready to load", "Modèle prêt à charger", "Modelo listo para cargar");
            color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            break;
        }
    }
    const auto& actual = broker_.actual_compute();
    const auto compute = broker_.cpu_fallback() ? localized("CPU (fallback)", "CPU (repli)", "CPU (respaldo)") :
        actual.empty() ? compute_label(draft_.compute) : actual;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    if (active.id.empty()) {
        ImGui::TextWrapped("%s · %s", summary, compute.c_str());
    } else {
        ImGui::TextWrapped("%s · %s %s · %s", summary, model_label(active.id), active.quant.c_str(), compute.c_str());
    }
    ImGui::PopStyleColor();
    ImGui::Separator();

    const bool table = ImGui::GetContentRegionAvail().x >= 714 &&
        ImGui::BeginTable("settings-and-test", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV);
    if (table) ImGui::TableNextColumn();
    section_heading(localized("Settings", "Réglages", "Ajustes"));
    draw_models();
    draw_policies();
    if (table) ImGui::TableNextColumn();
    section_heading(localized("Recognition test", "Test de reconnaissance", "Prueba de reconocimiento"));
    draw_tests();
    if (table) ImGui::EndTable();
    ImGui::Spacing();
    draw_activity();
}

void Provider::draw_models() {
    ImGui::SeparatorText(localized("Model", "Modèle", "Modelo"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##model", model_label(selected_id_))) {
        for (const auto& model : names) {
            if (ImGui::Selectable(model.label, selected_id_ == model.id)) {
                selected_id_ = model.id;
                selected_quant_ = "Q8_0";
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextUnformatted(localized("Quantization", "Quantification", "Cuantización"));
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
    const auto* artifact = selected_entry();
    const auto candidate = candidate_path();
    if (artifact) {
        ImGui::TextWrapped("%.1f %s · %s", artifact->at("bytes").get<double>() / 1000000.0,
            localized("MB", "Mo", "MB"),
            candidate.empty() ? localized("Not installed", "Absent du dossier", "Sin instalar") :
                                localized("Found locally", "Présent dans le dossier", "Encontrado en la carpeta"));
    }
    ImGui::BeginDisabled(!broker_.ready() || identify_ticket_ || candidate.empty() || broker_.busy());
    if (ImGui::Button(localized("Apply model", "Appliquer le modèle", "Aplicar modelo"))) identify(candidate);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!broker_.ready() || !artifact || !candidate.empty() || download_ticket_ || browse_ticket_);
    if (ImGui::Button(localized("Download", "Télécharger", "Descargar"))) {
        const auto ticket = ++next_ticket_;
        if (process_.send({{"type", "download"}, {"ticket", ticket}, {"path", folder_.u8string()}, {"entry", *artifact}})) {
            download_ticket_ = ticket;
            download_total_ = artifact->at("bytes").get<uint64_t>();
            downloaded_ = 0;
            download_state_.clear();
        }
    }
    ImGui::EndDisabled();
    if (download_ticket_) {
        ImGui::ProgressBar(download_total_ ? float(double(downloaded_) / double(download_total_)) : 0.f);
        if (ImGui::Button(localized("Cancel transfer", "Annuler le transfert", "Cancelar descarga")))
            process_.send({{"type", "cancel_download"}});
    } else if (download_state_ == "ok") {
        ImGui::TextUnformatted(localized("Download verified", "Téléchargement vérifié", "Descarga verificada"));
    } else if (download_state_ == "cancelled") {
        ImGui::TextUnformatted(status_label(XVR_CANCELLED));
    } else if (!download_state_.empty()) {
        ImGui::TextWrapped("%s", localized("Could not install the download", "Installation du téléchargement impossible",
                                           "No se pudo instalar la descarga"));
    }
    if (model_error_) ImGui::TextWrapped("%s", status_label(XVR_MODEL_UNAVAILABLE));
    if (!ImGui::TreeNode(localized("Models folder", "Dossier des modèles", "Carpeta de modelos"))) return;
    ImGui::TextWrapped("%s", folder_.u8string().c_str());
    ImGui::BeginDisabled(!broker_.ready() || browse_ticket_ || download_ticket_ || scan_ticket_ || identify_ticket_);
#ifdef _WIN32
    if (ImGui::Button(localized("Browse…", "Parcourir…", "Examinar…"))) {
        const auto ticket = ++next_ticket_;
        if (process_.send({{"type", "browse"}, {"ticket", ticket}})) browse_ticket_ = ticket;
    }
    ImGui::SameLine();
#endif
    if (ImGui::Button(localized("Rescan", "Actualiser", "Volver a buscar"))) scan();
    ImGui::EndDisabled();
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

void Provider::draw_policies() {
    const char* default_input = localized("System default", "Par défaut du système", "Predeterminado del sistema");
    std::string current = draft_.microphone.empty() ? default_input :
        localized("Selected microphone missing", "Microphone sélectionné absent", "Falta el micrófono seleccionado");
    for (const auto& mic : microphones_) if (mic.at("id") == draft_.microphone) current = mic.at("name");
    ImGui::SeparatorText(localized("Microphone", "Microphone", "Micrófono"));
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
    if (active.nemotron()) {
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

    ImGui::SeparatorText(localized("Compute device", "Périphérique de calcul", "Dispositivo de cálculo"));
    const auto compute = compute_label(draft_.compute);
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(!broker_.ready() || (compute_devices_.empty() && draft_.compute == "cpu"));
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

    ImGui::SeparatorText(localized("Model memory", "Mémoire du modèle", "Memoria del modelo"));
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
    if (changed) {
        draft_.profile = broker_.settings().profile;
        broker_.set_settings(draft_, Clock::now());
        save_preferences();
    }
}

void Provider::start_test(bool sample) {
    std::string wav;
    if (sample) {
        const auto& settings = broker_.settings();
        const char* file = localized("test-en.wav", "test-fr.wav", "test-es.wav");
        if (settings.profile.nemotron()) {
            if (settings.language == "en-US") file = "test-en.wav";
            if (settings.language == "fr-FR") file = "test-fr.wav";
            if (settings.language == "es-ES") file = "test-es.wav";
        } else if (settings.profile.streaming()) {
            file = "test-en.wav";
        }
        wav = (root_ / file).u8string();
    }
    test_text_.clear();
    test_is_sample_ = sample;
    compute_ms_ = 0;
    test_level_ = 0;
    test_status_ = broker_.begin_local(std::move(wav), Clock::now(), test_session_);
    test_pending_ = test_status_ == XVR_OK;
    local_held_ = test_pending_ && !sample;
    listening_ = false;
}

void Provider::draw_tests() {
    const bool unavailable = !broker_.ready() || broker_.model_state() == XVR_MODEL_MISSING;
    ImGui::TextUnformatted(localized("Microphone level", "Niveau du microphone", "Nivel del micrófono"));
    ImGui::ProgressBar(local_held_ ? test_level_ : 0.0f, {-1, ImGui::GetFrameHeight()}, "");
    ImGui::BeginDisabled(unavailable || (broker_.busy() && !local_held_));
    ImGui::Button(localized("Hold to talk", "Maintenir pour parler", "Mantén pulsado para hablar"), {-1, 0});
    if (ImGui::IsItemActivated() && !local_held_) start_test(false);
    ImGui::EndDisabled();
    if (local_held_ && !window_.pointer_pressed()) {
        const auto status = broker_.end_local(test_session_, Clock::now());
        if (status != XVR_OK && test_pending_) test_status_ = status;
        local_held_ = false;
        listening_ = false;
    }
    ImGui::SeparatorText(localized("Transcript", "Transcription", "Transcripción"));
    const float transcript_height = ImGui::GetContentRegionAvail().x >= 315 ? 220.0f : 148.0f;
    if (ImGui::BeginChild("transcript", {0, transcript_height}, ImGuiChildFlags_Borders)) {
        if (local_held_) {
            ImGui::TextWrapped("%s", listening_ ? localized("Listening…", "Écoute…", "Escuchando…") :
                localized("Starting microphone…", "Démarrage du micro…", "Iniciando micrófono…"));
        } else if (test_pending_) {
            ImGui::TextUnformatted(localized("Transcribing…", "Transcription en cours…", "Transcribiendo…"));
        } else if (!test_text_.empty()) {
            const char* source = test_is_sample_ ? localized("Recording", "Enregistrement", "Grabación") :
                localized("Microphone", "Microphone", "Micrófono");
            ImGui::TextDisabled("%s · %u ms", source, compute_ms_);
            ImGui::TextWrapped("%s", test_text_.c_str());
        } else if (test_status_ != XVR_OK) {
            ImGui::TextWrapped("%s", status_label(test_status_));
        } else if (unavailable) {
            ImGui::TextWrapped("%s", broker_.ready() ?
                localized("Apply an installed model to start a test.", "Applique un modèle installé pour lancer un test.",
                          "Aplica un modelo instalado para iniciar una prueba.") : status_label(engine_status_));
        } else {
            ImGui::TextWrapped("%s", localized("Hold the button, speak, then release to read the result.",
                "Maintiens le bouton, parle, puis relâche pour lire le résultat.",
                "Mantén pulsado el botón, habla y suéltalo para leer el resultado."));
        }
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(unavailable || broker_.busy());
    if (ImGui::Button(localized("Test with a recording", "Tester avec un enregistrement", "Probar con una grabación")))
        start_test(true);
    ImGui::EndDisabled();
}

void Provider::draw_activity() {
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(1, 1, 1, 0.13f));
    const bool open = ImGui::TreeNodeEx("plugin-activity",
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
        "%s", localized("Plugin activity", "Activité des plugins", "Actividad de plugins"));
    ImGui::PopStyleColor(2);
    ImGui::Separator();
    if (!open) return;
    const bool table = ImGui::GetContentRegionAvail().x >= 714 &&
        ImGui::BeginTable("activity", 2, ImGuiTableFlags_BordersInnerV);
    if (table) {
        ImGui::TableSetupColumn("##subscriptions", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn("##requests", ImGuiTableColumnFlags_WidthStretch, 2);
        ImGui::TableNextColumn();
    }
    const auto subscriptions = std::string(localized("Subscribers", "Abonnés", "Suscriptores")) +
        " (" + std::to_string(subscribers_.size()) + ")";
    ImGui::SeparatorText(subscriptions.c_str());
    if (subscribers_.empty())
        ImGui::TextDisabled("%s", localized("No subscribed plugins", "Aucun plugin abonné", "Ningún plugin suscrito"));
    for (const auto& item : subscribers_) ImGui::TextWrapped("%s", item.second.first.c_str());
    if (table) ImGui::TableNextColumn();
    ImGui::SeparatorText(localized("Recent requests", "Requêtes récentes", "Solicitudes recientes"));
    if (history_.empty())
        ImGui::TextDisabled("%s", localized("No requests", "Aucune requête", "No hay solicitudes"));
    for (const auto& row : history_) {
        ImGui::TextWrapped("%s · %s", row.name.c_str(),
            row.pending ? localized("In progress", "En cours", "En curso") : status_label(row.status));
        if (!row.model.empty()) {
            const char* vocab = row.vocabulary == XVR_VOCAB_APPLIED ?
                localized("Vocabulary applied", "Vocabulaire appliqué", "Vocabulario aplicado") :
                row.vocabulary == XVR_VOCAB_UNSUPPORTED ?
                localized("Vocabulary unsupported", "Vocabulaire non pris en charge", "Vocabulario no compatible") :
                localized("No vocabulary", "Sans vocabulaire", "Sin vocabulario");
            ImGui::Indent();
            ImGui::TextWrapped("%s · %s", model_label(row.model), vocab);
            ImGui::Unindent();
        }
    }
    if (table) ImGui::EndTable();
}

std::unique_ptr<Provider> provider;
}

PLUGIN_API int XPluginStart(char* name, char* signature, char* description) {
    std::strcpy(name, "X-Voice-Rekon");
    std::strcpy(signature, XVR_PROVIDER_SIGNATURE);
    std::strcpy(description, "Local push-to-talk transcription service");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    return 1;
}

PLUGIN_API void XPluginStop() { provider.reset(); }
PLUGIN_API int XPluginEnable() {
    try {
        provider = std::make_unique<Provider>();
        return 1;
    } catch (const std::exception&) {
        XPLMDebugString("X-Voice-Rekon: enable failed\n");
        return 0;
    }
}
PLUGIN_API void XPluginDisable() { provider.reset(); }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID sender, int message, void* parameter) {
    if (provider && message == XVR_MESSAGE_REQUEST && parameter) {
        try { provider->request(sender, *static_cast<XvrRequest*>(parameter)); }
        catch (const std::exception&) { XPLMDebugString("X-Voice-Rekon: invalid request\n"); }
    }
}
