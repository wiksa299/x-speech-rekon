#include "provider.hpp"

#include "localization.hpp"

#include <algorithm>

namespace xsr {
namespace {
constexpr ImVec4 red_text{0.96f, 0.60f, 0.60f, 1};
} // namespace

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
            if (ImGui::BeginChild("mic-input", {}, settings_panel_flags)) {
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
            if (ImGui::BeginChild("mic-levels", {}, settings_panel_flags)) draw_microphone_levels();
            ImGui::EndChild();
            if (ImGui::BeginChild("mic-review", {}, settings_panel_flags)) {
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

} // namespace xsr
