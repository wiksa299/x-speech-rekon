#include "provider.hpp"

#include "desktop.hpp"
#include "localization.hpp"

#include <XPLMUtilities.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace xsr {
namespace {
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
} // namespace

const char* Provider::status_label(uint32_t code) {
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

void Provider::emphasized_text(const char* text, float extra_size) {
    const auto pos = ImGui::GetCursorScreenPos();
    auto* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() + extra_size;
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImGui::GetWindowDrawList()->AddText(font, size, pos, ImGui::GetColorU32(ImGuiCol_Text), text, nullptr, width);
    const auto bounds = font->CalcTextSizeA(size, std::numeric_limits<float>::max(), width, text);
    ImGui::Dummy({0, bounds.y + 2});
}

const char* Provider::test_source_label(TestSource type) {
    switch (type) {
    case TestSource::reference:
        return tr("built_in_recording");
    case TestSource::wav:
        return tr("wav_file");
    default:
        return tr("microphone");
    }
}

const char* Provider::activity_status(uint32_t status, uint32_t stage, TestSource source) {
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

const Json* Provider::selected_entry() const {
    for (const auto& entry : catalog_) {
        if (entry.at("id") == selected_id_ && entry.at("quant") == selected_quant_) return &entry;
    }
    return nullptr;
}

std::string Provider::compute_label(const std::string& id) const {
    if (id == "cpu") return "CPU";
    for (const auto& device : compute_devices_) if (device.at("id") == id) return device.at("label");
    return tr("vulkan_unavailable");
}

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
    if (ImGui::BeginChild("model", {}, settings_panel_flags)) draw_models();
    ImGui::EndChild();
    bool changed = false;
    if (ImGui::BeginChild("compute-and-memory", {}, settings_panel_flags)) changed = draw_compute_and_memory();
    ImGui::EndChild();
    if (ImGui::BeginChild("recognition", {}, settings_panel_flags)) changed |= draw_recognition_settings();
    ImGui::EndChild();
    if (ImGui::BeginChild("microphone-settings", {}, settings_panel_flags)) {
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
    if (ImGui::BeginChild("micro-indicator-settings", {}, settings_panel_flags)) draw_micro_settings();
    ImGui::EndChild();
    if (ImGui::BeginChild("interface-settings", {}, settings_panel_flags)) {
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

void Provider::draw_test_page() {
    if (ImGui::BeginChild("test-scroll", {}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) {
        if (ImGui::BeginChild("recognition-test", {}, settings_panel_flags)) draw_tests();
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
        const bool visible = ImGui::BeginChild("request", {}, settings_panel_flags);
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

    if (ImGui::BeginChild("diagnostic-state", {}, settings_panel_flags)) {
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

    if (ImGui::BeginChild("diagnostic-incidents", {}, settings_panel_flags)) {
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

} // namespace xsr
