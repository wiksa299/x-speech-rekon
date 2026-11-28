#include "provider.hpp"

#include "localization.hpp"
#include "model_catalog.hpp"

#include <algorithm>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace xsl {
namespace {
Json json_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return file ? Json::parse(file, nullptr, false) : Json();
}

bool valid_digest(std::string_view digest) {
    return digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
} // namespace

bool Provider::string_fields(const Json& value, std::initializer_list<const char*> fields) {
    return value.is_object() && std::all_of(fields.begin(), fields.end(), [&](const char* key) {
        const auto field = value.find(key);
        return field != value.end() && field->is_string();
    });
}

bool Provider::valid_catalog_entry(const Json& entry) {
    if (!string_fields(entry, {"id", "quant", "revision", "file", "sha256"}) ||
        !entry.contains("bytes") || !entry["bytes"].is_number_unsigned() || entry["bytes"] == 0 ||
        !entry.contains("urls") || !entry["urls"].is_array() || entry["urls"].empty()) return false;
    if (const auto previous = entry.find("previous"); previous != entry.end() &&
        (entry["id"] != fluid_pro_id || !string_fields(*previous, {"revision", "sha256"}) ||
         !previous->contains("bytes") || !previous->at("bytes").is_number_unsigned() ||
         previous->at("bytes") == 0 || previous->at("bytes") == entry["bytes"] ||
         !valid_digest(previous->at("sha256").get_ref<const std::string&>()))) return false;
    const Profile profile{entry.at("id"), {}, {}, {}, {}};
    return profile.supported() && valid_digest(entry["sha256"].get_ref<const std::string&>()) &&
        std::all_of(entry["urls"].begin(), entry["urls"].end(), [](const Json& url) {
            return url.is_string() && url.get_ref<const std::string&>().rfind("https://", 0) == 0;
        });
}

Profile Provider::decode_profile(const Json& value) {
    if (!string_fields(value, {"id", "quant", "revision", "path", "sha256"}) ||
        !valid_digest(value["sha256"].get_ref<const std::string&>())) return {};
    Profile profile{value["id"], value["quant"], value["revision"], value["path"], value["sha256"]};
    return profile.supported() ? profile : Profile{};
}

PreferencesWriter::PreferencesWriter(fs::path path) : path_(std::move(path)), worker_([this] { run(); }) {}

PreferencesWriter::~PreferencesWriter() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_one();
    worker_.join();
}

void PreferencesWriter::save(std::string contents) {
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(contents);
    }
    changed_.notify_one();
}

void PreferencesWriter::run() {
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

void Provider::load_catalog() {
    catalog_ = json_file(root_ / "catalog.json");
    catalog_error_ = !catalog_.is_array() || catalog_.empty() ||
        !std::all_of(catalog_.begin(), catalog_.end(), valid_catalog_entry);
    if (catalog_error_) catalog_ = Json::parse(built_in_model_catalog);
}

std::string Provider::default_quantization_for(const std::string& model) const {
    std::string fallback;
    for (const auto& entry : catalog_) {
        if (entry.at("id") != model) continue;
        const auto quantization = entry.at("quant").get<std::string>();
        if (quantization == "Q6_K") return quantization;
        if (fallback.empty() || quantization == "Q8_0") fallback = quantization;
    }
    return fallback;
}

void Provider::load_preferences() {
    folder_ = root_ / "models";
    selected_quant_ = default_quantization_for(selected_id_);
    const auto saved = json_file(preferences_);
    if (!saved.is_object()) return;
    try {
        folder_ = fs::u8path(saved.value("folder", folder_.u8string()));
        const auto model = saved.value("id", selected_id_);
        const Profile preferred{model, saved.value("quant", default_quantization_for(model)), {},
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
        draft_.beam_width = std::clamp(saved.value("beam_width", 1), 1, int(XSL_MAX_CANDIDATES));
        const auto bias = saved.value("vocabulary_enabled", Json());
        if (bias.is_boolean()) draft_.vocabulary_enabled = bias.get<bool>();
        draft_.vocabulary_weight = std::clamp(saved.value("vocabulary_weight", -1.0f), -1.0f, 4.0f);
        draft_.sidetone = saved.value("sidetone", false);
        draft_.low_latency_monitoring = saved.value("low_latency_monitoring", false);
        draft_.monitor_gain_percent = std::clamp(saved.value("monitor_gain_percent", int(XSL_DEFAULT_MONITOR_GAIN_PERCENT)),
                                                0, int(XSL_MAX_MONITOR_GAIN_PERCENT));
        show_micro_indicator_ = saved.value("show_micro_indicator", false);
        show_micro_requester_ = saved.value("show_micro_requester", false);
        micro_opacity_ = std::clamp(saved.value("micro_opacity", 65), 0, 100);
        micro_border_opacity_ = std::clamp(saved.value("micro_border_opacity", 50), 0, 100);
        micro_bounds_ = saved.value("micro_bounds", Window::Bounds{});
        draft_.load = saved.value("load", 0) == 1 ? LoadPolicy::on_activate : LoadPolicy::on_use;
        draft_.unload = static_cast<UnloadPolicy>(std::clamp(saved.value("unload", 2), 0, 2));
        draft_.idle_minutes = std::clamp(saved.value("idle_minutes", 5), 1, 60);
    } catch (const Json::exception&) {}
    idle_minutes_draft_ = draft_.idle_minutes;
    if (saved.value("engine_enabled", Json(true)) == false) engine_mode_ = EngineMode::disabled;
    const auto threads = saved.value("cpu_threads", Json());
    if (threads.is_number_integer() && threads >= 1 && threads <= available_cpu_threads_)
        requested_cpu_threads_ = threads.get<int>();
    broker_.set_settings(draft_, Clock::now());
}

void Provider::save_preferences() {
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
        {"engine_enabled", engine_mode_ != EngineMode::disabled}, {"cpu_threads", requested_cpu_threads_},
        {"microphone", settings.microphone}, {"compute", settings.compute}, {"language", settings.language},
        {"early", settings.early}, {"beam_width", settings.beam_width},
        {"vocabulary_enabled", settings.vocabulary_enabled ? Json(*settings.vocabulary_enabled) : Json()},
        {"vocabulary_weight", settings.vocabulary_weight}, {"sidetone", settings.sidetone},
        {"low_latency_monitoring", settings.low_latency_monitoring}, {"monitor_gain_percent", settings.monitor_gain_percent},
        {"load", int(settings.load)},
        {"unload", int(settings.unload)}, {"idle_minutes", settings.idle_minutes},
        {"show_micro_indicator", show_micro_indicator_}, {"show_micro_requester", show_micro_requester_},
        {"micro_opacity", micro_opacity_}, {"micro_border_opacity", micro_border_opacity_}, {"micro_bounds", micro_bounds_}};
    if (last_good.available()) saved["last_good"] = {{"id", last_good.id}, {"quant", last_good.quant},
        {"revision", last_good.revision}, {"path", last_good.path}, {"sha256", last_good.sha256}};
    writer_.save(saved.dump(2, ' ', false, Json::error_handler_t::replace));
}

} // namespace xsl
