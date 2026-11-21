#include "core.hpp"
#include "window.hpp"

#include <XPLMMenus.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <cstring>

namespace {
using namespace xvr;

class Diagnostic {
public:
    Diagnostic() : window_([&] { draw(); }) {
        menu_index_ = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "X-Voice-Rekon Diagnostic", nullptr, 0);
        menu_ = XPLMCreateMenu("X-Voice-Rekon Diagnostic", XPLMFindPluginsMenu(), menu_index_,
            +[](void* owner, void*) { static_cast<Diagnostic*>(owner)->window_.show(); }, this);
        XPLMAppendMenuItem(menu_, localized("Show diagnostic", "Afficher le diagnostic", "Mostrar diagnóstico"), nullptr, 0);
        command_ = XPLMCreateCommand("xvoice_rekon/diagnostic/ptt",
                                     "X-Voice-Rekon Diagnostic: hold to talk");
        XPLMRegisterCommandHandler(command_, command_handler, 1, this);
        XPLMRegisterFlightLoopCallback(tick_handler, -1, this);
    }

    ~Diagnostic() {
        XPLMUnregisterFlightLoopCallback(tick_handler, this);
        XPLMUnregisterCommandHandler(command_, command_handler, 1, this);
        if (subscribed_ && provider_ != XPLM_NO_PLUGIN_ID && XPLMIsPluginEnabled(provider_))
            send(XVR_UNSUBSCRIBE);
        XPLMDestroyMenu(menu_);
        XPLMRemoveMenuItem(XPLMFindPluginsMenu(), menu_index_);
    }

    void receive(int sender, const XvrEvent& event) {
        if (sender != provider_ || event.version != XVR_ABI_VERSION || event.struct_size < sizeof(event)) return;
        if (event.kind == XVR_SERVICE_STOPPED) {
            lose();
            return;
        }
        if (!session_ || event.session != session_) return;
        if (event.kind == XVR_CAPTURE_STARTED) listening_ = held_;
        if (event.kind == XVR_RESULT) {
            session_ = 0;
            listening_ = false;
            status_ = event.status;
            vocabulary_ = event.vocabulary;
            compute_ms_ = event.compute_ms;
            if (event.status == XVR_OK) {
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
        self.discover();
        if (self.session_ && Clock::now() >= self.deadline_) {
            if (self.subscribed_) self.send(XVR_PTT_CANCEL, self.session_);
            self.session_ = 0;
            self.listening_ = false;
            self.status_ = XVR_TIMEOUT;
        }
        return self.session_ ? 0.05f : 0.5f;
    }

    XvrRequest send(uint32_t operation, uint64_t session = 0, const XvrString* terms = nullptr, uint32_t count = 0) {
        XvrRequest request{};
        request.version = XVR_ABI_VERSION;
        request.struct_size = sizeof(request);
        request.operation = operation;
        request.status = XVR_SERVICE_UNAVAILABLE;
        request.session = session;
        request.terms = terms;
        request.term_count = count;
        if (provider_ != XPLM_NO_PLUGIN_ID)
            XPLMSendMessageToPlugin(provider_, XVR_MESSAGE_REQUEST, &request);
        return request;
    }

    void lose() {
        provider_ = XPLM_NO_PLUGIN_ID;
        subscribed_ = false;
        session_ = 0;
        listening_ = false;
        status_ = XVR_SERVICE_UNAVAILABLE;
    }

    void discover() {
        const auto found = XPLMFindPluginBySignature(XVR_PROVIDER_SIGNATURE);
        if (found == XPLM_NO_PLUGIN_ID || !XPLMIsPluginEnabled(found)) {
            lose();
            return;
        }
        if (found != provider_) lose();
        provider_ = found;
        if (subscribed_) return;
        const auto subscription = send(XVR_SUBSCRIBE);
        status_ = subscription.status;
        if (subscription.status == XVR_OK) {
            subscribed_ = true;
            static const XvrString terms[] = {{"landing gear", 12}, {"flight level", 12}, {"heading", 7}};
            const auto result = send(XVR_SET_VOCABULARY, 0, terms, 3);
            if (result.status != XVR_OK) status_ = result.status;
        }
    }

    void begin() {
        if (held_ || session_) return;
        held_ = true;
        discover();
        text_.clear();
        if (!subscribed_) return;
        const auto response = send(XVR_PTT_BEGIN);
        status_ = response.status;
        session_ = response.status == XVR_OK ? response.session : 0;
        deadline_ = Clock::now() + 33s;
        if (response.status == XVR_NOT_SUBSCRIBED || response.status == XVR_SERVICE_UNAVAILABLE) lose();
    }

    void end() {
        const bool was_held = held_;
        held_ = false;
        listening_ = false;
        if (!was_held || !session_ || !subscribed_) return;
        const auto response = send(XVR_PTT_END, session_);
        if (response.status != XVR_OK) {
            status_ = response.status;
            session_ = 0;
        } else {
            deadline_ = Clock::now() + 23s;
        }
    }

    const char* state() const {
        if (session_ && held_)
            return listening_ ? localized("Listening", "Écoute", "Escuchando") :
                                localized("Starting microphone", "Démarrage du micro", "Iniciando micrófono");
        if (session_) return localized("Transcribing…", "Transcription…", "Transcribiendo…");
        switch (status_) {
        case XVR_OK: return localized("Subscribed", "Abonné", "Suscrito");
        case XVR_SERVICE_UNAVAILABLE: return localized("Provider unavailable", "Fournisseur indisponible", "Proveedor no disponible");
        case XVR_BUSY: return localized("Microphone in use", "Microphone occupé", "Micrófono ocupado");
        case XVR_MODEL_UNAVAILABLE: return localized("Model unavailable", "Modèle indisponible", "Modelo no disponible");
        case XVR_MIC_UNAVAILABLE: return localized("Microphone unavailable", "Microphone indisponible", "Micrófono no disponible");
        case XVR_TIMEOUT: return localized("Timed out", "Délai dépassé", "Tiempo agotado");
        case XVR_NO_AUDIO: return localized("No audio captured", "Aucun son capturé", "No se ha capturado audio");
        case XVR_NO_SPEECH: return localized("No speech detected", "Aucune parole détectée", "No se ha detectado habla");
        default: return localized("Request failed", "Échec de la demande", "Error de la solicitud");
        }
    }

    void draw() {
        ImGui::TextUnformatted("X-Voice-Rekon Diagnostic");
        ImGui::SameLine();
        if (ImGui::SmallButton(localized("Close", "Fermer", "Cerrar"))) window_.hide();
        ImGui::Separator();
        ImGui::TextWrapped("%s", localized("Assign xvoice_rekon/diagnostic/ptt in X-Plane and hold it to speak.",
            "Assignez xvoice_rekon/diagnostic/ptt dans X-Plane, puis maintenez la commande pour parler.",
            "Asigna xvoice_rekon/diagnostic/ptt en X-Plane y mantén pulsado para hablar."));
        ImGui::Text("%s [%s]", state(), status_name(status_).c_str());
        if (!text_.empty()) {
            ImGui::TextWrapped("%s", text_.c_str());
            ImGui::Text("%s · %s · %s · %u ms", model_.c_str(), quant_.c_str(), language_.c_str(), compute_ms_);
            ImGui::TextWrapped("%s", revision_.c_str());
            const char* vocabulary = vocabulary_ == XVR_VOCAB_APPLIED ?
                localized("Vocabulary applied", "Vocabulaire appliqué", "Vocabulario aplicado") :
                vocabulary_ == XVR_VOCAB_UNSUPPORTED ?
                localized("Vocabulary unsupported", "Vocabulaire non pris en charge", "Vocabulario no compatible") :
                localized("No vocabulary", "Sans vocabulaire", "Sin vocabulario");
            ImGui::TextUnformatted(vocabulary);
        }
    }

    Window window_;
    XPLMMenuID menu_ = nullptr;
    XPLMCommandRef command_ = nullptr;
    int menu_index_ = 0, provider_ = XPLM_NO_PLUGIN_ID;
    bool subscribed_ = false, listening_ = false, held_ = false;
    uint32_t status_ = XVR_SERVICE_UNAVAILABLE, vocabulary_ = XVR_VOCAB_NONE, compute_ms_ = 0;
    uint64_t session_ = 0;
    Clock::time_point deadline_{};
    std::string text_, model_, quant_, language_, revision_;
};

std::unique_ptr<Diagnostic> diagnostic;
}

PLUGIN_API int XPluginStart(char* name, char* signature, char* description) {
    std::strcpy(name, "X-Voice-Rekon Diagnostic");
    std::strcpy(signature, "wiksa.xvoice-rekon.diagnostic");
    std::strcpy(description, "Diagnostics for the X-Voice-Rekon plugin API");
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
    if (diagnostic && message == XVR_MESSAGE_EVENT && parameter) {
        try { diagnostic->receive(sender, *static_cast<XvrEvent*>(parameter)); }
        catch (const std::exception&) { XPLMDebugString("X-Voice-Rekon Diagnostic: invalid event\n"); }
    }
}
