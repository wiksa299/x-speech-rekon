#include "core.hpp"
#include "window.hpp"
#include "localization.hpp"

#include <XPLMMenus.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <cstring>

namespace {
using namespace xsr;

class Diagnostic {
public:
    Diagnostic() : window_([&] { draw(); }, 690, 680, interface_directory(true).parent_path() / "NotoSans-Regular.ttf") {
        initialize_interface(true);
        menu_index_ = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "X-Speech Rekon Diagnostic", nullptr, 0);
        menu_ = XPLMCreateMenu("X-Speech Rekon Diagnostic", XPLMFindPluginsMenu(), menu_index_,
            +[](void* owner, void*) { static_cast<Diagnostic*>(owner)->window_.show("X-Speech Rekon Diagnostic"); }, this);
        show_item_ = XPLMAppendMenuItem(menu_, tr("show_diagnostic"), nullptr, 0);
        command_ = XPLMCreateCommand("xspeech_rekon/diagnostic/ptt",
                                     tr("diagnostic_command"));
        XPLMRegisterCommandHandler(command_, command_handler, 1, this);
        XPLMRegisterFlightLoopCallback(tick_handler, -1, this);
    }

    ~Diagnostic() {
        XPLMUnregisterFlightLoopCallback(tick_handler, this);
        XPLMUnregisterCommandHandler(command_, command_handler, 1, this);
        if (subscribed_ && provider_ != XPLM_NO_PLUGIN_ID && XPLMIsPluginEnabled(provider_))
            send(XSR_UNSUBSCRIBE);
        XPLMDestroyMenu(menu_);
        XPLMRemoveMenuItem(XPLMFindPluginsMenu(), menu_index_);
    }

    void select_language(int sender, const char* preference) {
        if (sender == provider_ && sender != XPLM_NO_PLUGIN_ID && interface_locale().select(preference))
            XPLMSetMenuItemName(menu_, show_item_, tr("show_diagnostic"), 0);
    }

    void refresh_language() {
        if (interface_locale().refresh())
            XPLMSetMenuItemName(menu_, show_item_, tr("show_diagnostic"), 0);
    }

    void receive(int sender, const XsrEvent& event) {
        if (sender != provider_ || event.version != XSR_ABI_VERSION || event.struct_size < sizeof(event)) return;
        if (event.kind == XSR_SERVICE_STOPPED) {
            lose();
            return;
        }
        if (!session_ || event.session != session_) return;
        if (event.kind == XSR_CAPTURE_STARTED) listening_ = held_;
        if (event.kind == XSR_RESULT) {
            session_ = 0;
            listening_ = false;
            status_ = event.status;
            vocabulary_ = event.vocabulary;
            compute_ms_ = event.compute_ms;
            if (event.status == XSR_OK) {
                text_ = event.text ? event.text : "";
                model_ = event.model_id ? event.model_id : "";
                quant_ = event.quantization ? event.quantization : "";
                language_ = event.language ? event.language : "";
                revision_ = event.revision ? event.revision : "";
            }
        }
    }

private:
    static int command_handler(XPLMCommandRef, XPLMCommandPhase phase, void* owner) {
        auto& self = *static_cast<Diagnostic*>(owner);
        if (phase == xplm_CommandBegin) self.begin();
        if (phase == xplm_CommandEnd) self.end();
        return 1;
    }

    static float tick_handler(float, float, int, void* owner) {
        auto& self = *static_cast<Diagnostic*>(owner);
        self.refresh_language();
        self.discover();
        if (self.session_ && Clock::now() >= self.deadline_) {
            if (self.subscribed_) self.send(XSR_PTT_CANCEL, self.session_);
            self.session_ = 0;
            self.listening_ = false;
            self.status_ = XSR_TIMEOUT;
        }
        return self.session_ ? 0.05f : 0.5f;
    }

    XsrRequest send(uint32_t operation, uint64_t session = 0, const XsrString* terms = nullptr, uint32_t count = 0) {
        XsrRequest request{};
        request.version = XSR_ABI_VERSION;
        request.struct_size = sizeof(request);
        request.operation = operation;
        request.status = XSR_SERVICE_UNAVAILABLE;
        request.session = session;
        request.terms = terms;
        request.term_count = count;
        if (provider_ != XPLM_NO_PLUGIN_ID)
            XPLMSendMessageToPlugin(provider_, XSR_MESSAGE_REQUEST, &request);
        return request;
    }

    void lose() {
        provider_ = XPLM_NO_PLUGIN_ID;
        subscribed_ = false;
        session_ = 0;
        listening_ = false;
        status_ = XSR_SERVICE_UNAVAILABLE;
    }

    void discover() {
        const auto found = XPLMFindPluginBySignature(XSR_PROVIDER_SIGNATURE);
        if (found == XPLM_NO_PLUGIN_ID || !XPLMIsPluginEnabled(found)) {
            lose();
            return;
        }
        if (found != provider_) {
            lose();
            provider_ = found;
            initialize_interface(true);
            XPLMSetMenuItemName(menu_, show_item_, tr("show_diagnostic"), 0);
        }
        if (subscribed_) return;
        const auto subscription = send(XSR_SUBSCRIBE);
        status_ = subscription.status;
        if (subscription.status == XSR_OK) {
            subscribed_ = true;
            static const XsrString terms[] = {{"landing gear", 12}, {"flight level", 12}, {"heading", 7}};
            const auto result = send(XSR_SET_VOCABULARY, 0, terms, 3);
            if (result.status != XSR_OK) status_ = result.status;
        }
    }

    void begin() {
        if (held_ || session_) return;
        held_ = true;
        discover();
        text_.clear();
        if (!subscribed_) return;
        const auto response = send(XSR_PTT_BEGIN);
        status_ = response.status;
        session_ = response.status == XSR_OK ? response.session : 0;
        deadline_ = Clock::now() + 33s;
        if (response.status == XSR_NOT_SUBSCRIBED || response.status == XSR_SERVICE_UNAVAILABLE) lose();
    }

    void end() {
        const bool was_held = held_;
        held_ = false;
        listening_ = false;
        if (!was_held || !session_ || !subscribed_) return;
        const auto response = send(XSR_PTT_END, session_);
        if (response.status != XSR_OK) {
            status_ = response.status;
            session_ = 0;
        } else {
            deadline_ = Clock::now() + 23s;
        }
    }

    const char* state() const {
        if (session_ && held_)
            return listening_ ? tr("diagnostic_listening") :
                                tr("diagnostic_starting_microphone");
        if (session_) return tr("diagnostic_transcribing");
        switch (status_) {
        case XSR_OK: return tr("subscribed");
        case XSR_SERVICE_UNAVAILABLE: return tr("provider_unavailable");
        case XSR_BUSY: return tr("microphone_in_use");
        case XSR_MODEL_UNAVAILABLE: return tr("model_unavailable");
        case XSR_MIC_UNAVAILABLE: return tr("microphone_unavailable");
        case XSR_TIMEOUT: return tr("timed_out");
        case XSR_NO_AUDIO: return tr("no_audio_captured");
        case XSR_NO_SPEECH: return tr("no_speech_detected");
        default: return tr("request_failed");
        }
    }

    void draw() {
        refresh_language();
        ImGui::TextUnformatted("X-Speech Rekon Diagnostic");
        ImGui::SameLine();
        if (ImGui::SmallButton(ui_label("close"))) window_.hide();
        ImGui::Separator();
        ImGui::TextWrapped("%s", tr("diagnostic_ptt_instructions"));
        ImGui::Text("%s [%s]", state(), status_name(status_).c_str());
        if (!text_.empty()) {
            ImGui::TextWrapped("%s", text_.c_str());
            ImGui::Text("%s · %s · %s · %u ms", model_.c_str(), quant_.c_str(), language_.c_str(), compute_ms_);
            ImGui::TextWrapped("%s", revision_.c_str());
            const char* vocabulary = vocabulary_ == XSR_VOCAB_APPLIED ?
                tr("vocabulary_applied") :
                vocabulary_ == XSR_VOCAB_UNSUPPORTED ?
                tr("vocabulary_unsupported") :
                tr("no_vocabulary");
            ImGui::TextUnformatted(vocabulary);
        }
    }

    Window window_;
    XPLMMenuID menu_ = nullptr;
    XPLMCommandRef command_ = nullptr;
    int menu_index_ = 0, show_item_ = 0, provider_ = XPLM_NO_PLUGIN_ID;
    bool subscribed_ = false, listening_ = false, held_ = false;
    uint32_t status_ = XSR_SERVICE_UNAVAILABLE, vocabulary_ = XSR_VOCAB_NONE, compute_ms_ = 0;
    uint64_t session_ = 0;
    Clock::time_point deadline_{};
    std::string text_, model_, quant_, language_, revision_;
};

std::unique_ptr<Diagnostic> diagnostic;
}

PLUGIN_API int XPluginStart(char* name, char* signature, char* description) {
    std::strcpy(name, "X-Speech Rekon Diagnostic");
    std::strcpy(signature, diagnostic_signature);
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    initialize_interface(true);
    copy_plugin_description(description, "diagnostic_description");
    return 1;
}
PLUGIN_API void XPluginStop() { diagnostic.reset(); }
PLUGIN_API int XPluginEnable() {
    try {
        diagnostic = std::make_unique<Diagnostic>();
        return 1;
    } catch (const std::exception&) { return 0; }
}
PLUGIN_API void XPluginDisable() { diagnostic.reset(); }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID sender, int message, void* parameter) {
    if (diagnostic && message == interface_language_message && parameter)
        diagnostic->select_language(sender, static_cast<const char*>(parameter));
    if (diagnostic && message == XSR_MESSAGE_EVENT && parameter) {
        try { diagnostic->receive(sender, *static_cast<XsrEvent*>(parameter)); }
        catch (const std::exception&) { XPLMDebugString("X-Speech Rekon Diagnostic: invalid event\n"); }
    }
}
