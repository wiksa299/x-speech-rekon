#include "core.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <stdexcept>

namespace xsr {

std::string status_name(uint32_t status) {
    static constexpr const char* names[] = {
        "OK", "SERVICE_UNAVAILABLE", "BAD_VERSION", "BAD_REQUEST", "NOT_SUBSCRIBED",
        "BUSY", "MODEL_UNAVAILABLE", "MIC_UNAVAILABLE", "ENGINE_LOST",
        "ENGINE_RESTARTING", "TIMEOUT", "CANCELLED", "NO_AUDIO", "NO_SPEECH",
        "STALE_SESSION", "OUTPUT_TRUNCATED", "ENGINE_ERROR", "VOCABULARY_LIMIT"
    };
    return status < sizeof(names) / sizeof(*names) ? names[status] : "UNKNOWN";
}

bool valid_utf8(std::string_view input) {
    for (size_t index = 0; index < input.size();) {
        auto first = static_cast<unsigned char>(input[index++]);
        if (first < 0x80) {
            if (!first) return false;
            continue;
        }
        const int length = first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
        if (first < 0xc2 || first > 0xf4 || input.size() - index < size_t(length - 1)) return false;
        uint32_t code = first & (0x7f >> length);
        for (int i = 1; i < length; ++i) {
            auto next = static_cast<unsigned char>(input[index++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if ((length == 3 && code < 0x800) || (length == 4 && code < 0x10000) ||
            (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff) return false;
    }
    return true;
}

bool read_frame(std::istream& stream, Json& message) {
    std::string line;
    for (int byte; (byte = stream.get()) != EOF;) {
        if (byte == '\n') {
            message = Json::parse(line, nullptr, false);
            return message.is_object() && message.contains("type");
        }
        if (line.size() == max_frame) return false;
        line.push_back(static_cast<char>(byte));
    }
    return false;
}

std::string wire_frame(const Json& message) {
    auto line = message.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (line.size() > max_frame) return {};
    line.push_back('\n');
    return line;
}

Broker::Broker(Send send, Deliver deliver) : send_(std::move(send)), deliver_(std::move(deliver)) {}

void Broker::status(XsrRequest& out) const {
    out.model_state = model_state_;
    out.service_status = service_ready() ? XSR_OK : XSR_ENGINE_RESTARTING;
    out.supports_vocabulary = settings_.profile.supports_vocabulary(settings_.early);
    std::memset(out.model_id, 0, sizeof(out.model_id));
    std::memset(out.quantization, 0, sizeof(out.quantization));
    settings_.profile.id.copy(out.model_id, sizeof(out.model_id) - 1);
    settings_.profile.quant.copy(out.quantization, sizeof(out.quantization) - 1);
}

void Broker::request(int sender, XsrRequest& out, Clock::time_point now) {
    if (out.struct_size < offsetof(XsrRequest, status) + sizeof(out.status)) return;
    if (out.version != XSR_ABI_VERSION || out.struct_size < sizeof(XsrRequest)) {
        out.status = XSR_BAD_VERSION;
        return;
    }
    tick(now);
    out.status = XSR_OK;
    if (out.operation == XSR_SUBSCRIBE) {
        subscribers_.try_emplace(sender);
        status(out);
        return;
    }
    if (out.operation == XSR_GET_STATUS) {
        status(out);
        return;
    }
    auto subscriber = subscribers_.find(sender);
    if (subscriber == subscribers_.end()) {
        out.status = XSR_NOT_SUBSCRIBED;
        return;
    }
    switch (out.operation) {
    case XSR_UNSUBSCRIBE:
        forget(sender, now);
        return;
    case XSR_SET_VOCABULARY: {
        if (out.term_count > XSR_MAX_TERMS) {
            out.status = XSR_VOCABULARY_LIMIT;
            return;
        }
        if (out.term_count && !out.terms) {
            out.status = XSR_BAD_REQUEST;
            return;
        }
        std::set<std::string> seen;
        std::vector<std::string> terms;
        size_t bytes = 0;
        for (uint32_t i = 0; i < out.term_count; ++i) {
            const auto term = out.terms[i];
            if (term.bytes > XSR_MAX_TERM_BYTES || bytes + term.bytes > XSR_MAX_VOCABULARY_BYTES) {
                out.status = XSR_VOCABULARY_LIMIT;
                return;
            }
            if (!term.data || !term.bytes || !valid_utf8({term.data, term.bytes})) {
                out.status = XSR_BAD_REQUEST;
                return;
            }
            std::string word(term.data, term.bytes);
            if (seen.insert(word).second) {
                bytes += term.bytes;
                terms.push_back(std::move(word));
            }
        }
        subscriber->second = std::move(terms);
        return;
    }
    case XSR_PTT_BEGIN:
        out.session = 0;
        out.status = begin(sender, {}, now, out.session);
        return;
    case XSR_PTT_END:
        out.status = release(sender, out.session, now);
        return;
    case XSR_PTT_CANCEL:
        if (!active_ || active_->client != sender || active_->id != out.session) {
            out.status = XSR_STALE_SESSION;
            return;
        }
        abort_session(XSR_CANCELLED, XSR_STAGE_NONE, now);
        return;
    default:
        out.status = XSR_BAD_REQUEST;
    }
}

uint32_t Broker::begin(int client, std::string wav, Clock::time_point now, uint64_t& session) {
    if (active_) return XSR_BUSY;
    if (!settings_.profile.available() || model_state_ == XSR_MODEL_MISSING) return XSR_MODEL_UNAVAILABLE;
    if (!service_ready()) return XSR_ENGINE_RESTARTING;
    const auto id = ++next_session_;
    const bool sample = !wav.empty();
    Json request = {
        {"type", "begin"}, {"session", id}, {"generation", generation_},
        {"wav", std::move(wav)}, {"microphone", settings_.microphone},
        {"early", settings_.early && settings_.profile.streaming()},
        {"language", settings_.profile.nemotron() ? settings_.language : ""},
        {"terms", client == local_client ? std::vector<std::string>{} : subscribers_.at(client)}
    };
    if (!send_(request)) return XSR_ENGINE_RESTARTING;
    session = id;
    const auto limit = model_state_ == XSR_MODEL_READY ? 10s : 20s;
    active_ = Session{client, id, now, now + (sample ? limit : 2s), settings_.profile,
                      false, sample};
    return XSR_OK;
}

uint32_t Broker::release(int client, uint64_t session, Clock::time_point now) {
    if (!active_ || active_->client != client || active_->id != session || active_->released) {
        return XSR_STALE_SESSION;
    }
    if (!send_({{"type", "end"}, {"session", session}})) {
        abort_session(XSR_ENGINE_RESTARTING, XSR_STAGE_NONE, now);
        return XSR_ENGINE_RESTARTING;
    }
    active_->released = true;
    active_->deadline = now + (model_state_ == XSR_MODEL_READY ? 10s : 20s);
    return XSR_OK;
}

uint32_t Broker::begin_local(std::string wav, Clock::time_point now, uint64_t& session) {
    tick(now);
    return begin(local_client, std::move(wav), now, session);
}

uint32_t Broker::end_local(uint64_t session, Clock::time_point now) {
    tick(now);
    return release(local_client, session, now);
}

void Broker::cancel_local(Clock::time_point now) {
    forget(local_client, now);
}

void Broker::forget(int client, Clock::time_point now) {
    if (client != local_client) subscribers_.erase(client);
    if (active_ && active_->client == client) abort_session(XSR_CANCELLED, XSR_STAGE_NONE, now);
}

void Broker::abort_session(uint32_t status, uint32_t stage, Clock::time_point now) {
    draining_session_ = active_->id;
    drain_deadline_ = now + 1s;
    send_({{"type", "cancel"}, {"session", draining_session_}});
    finish(status, stage, {}, now);
}

void Broker::unload(Clock::time_point now) {
    if (send_({{"type", "unload"}, {"generation", generation_}})) {
        unload_retry_.reset();
        actual_compute_.clear();
        cpu_fallback_ = false;
        model_state_ = XSR_MODEL_UNLOADED;
    } else {
        unload_retry_ = now + 100ms;
    }
}

void Broker::finish(uint32_t status_code, uint32_t stage, Json result, Clock::time_point now) {
    if (!active_) return;
    auto session = std::move(*active_);
    active_.reset();
    last_use_ = now;
    const auto settled_generation = generation_;
    if (pending_) {
        auto updated = std::move(*pending_);
        pending_.reset();
        set_settings(std::move(updated), now);
    }
    if (ready_ && settled_generation == generation_ && settings_.unload == UnloadPolicy::after_use) {
        if (draining_session_) unload_retry_ = now;
        else unload(now);
    }
    if (session.client == local_client || subscribers_.count(session.client)) {
        if (status_code == XSR_OK) {
            result["id"] = session.profile.id;
            result["quant"] = session.profile.quant;
            result["revision"] = session.profile.revision;
        }
        deliver_({session.client, XSR_RESULT, status_code, session.id, stage, std::move(result)});
    }
}

void Broker::accept(const Json& event, Clock::time_point now) {
    const auto type = event.value("type", "");
    if (type == "inference_rejected" && event.value("generation", uint64_t(0)) == generation_) {
        if (event.value("operation", "") == "configure") {
            configuration_confirmed_ = false;
            configure_retry_ = now + 100ms;
        }
        else if (event.value("operation", "") == "unload") unload_retry_ = now + 100ms;
    } else if (type == "state" && event.value("generation", uint64_t(0)) == generation_) {
        configure_retry_.reset();
        const auto state = event.at("state").get<uint32_t>();
        if (state > XSR_MODEL_READY) throw std::runtime_error("Invalid engine model state");
        configuration_confirmed_ = true;
        model_state_ = state;
        if (model_state_ == XSR_MODEL_READY) {
            actual_compute_ = event.value("compute", "");
            cpu_fallback_ = event.value("cpu_fallback", false);
            last_good_ = settings_.profile;
        }
        if (model_state_ == XSR_MODEL_MISSING && !active_) recover_profile(now);
    } else if (type == "result") {
        const auto session = event.value("session", uint64_t(0));
        if (session == draining_session_) draining_session_ = 0;
        if (!active_ || session != active_->id) return;
        const auto code = event.at("status").get<uint32_t>();
        if (code > XSR_VOCABULARY_LIMIT) throw std::runtime_error("Invalid engine result status");
        const bool revert = code == XSR_MODEL_UNAVAILABLE && !pending_;
        finish(code, XSR_STAGE_NONE, code == XSR_OK ? event : Json::object(), now);
        if (revert) recover_profile(now);
    } else if (type == "capture" && active_ && !active_->released && !active_->captured &&
               event.value("session", uint64_t(0)) == active_->id) {
        active_->captured = true;
        active_->deadline = active_->accepted + 30s;
        deliver_({active_->client, XSR_CAPTURE_STARTED, XSR_OK, active_->id});
    }
}

void Broker::tick(Clock::time_point now) {
    if (active_ && now >= active_->deadline) {
        const auto stage = active_->released ? XSR_STAGE_TRANSCRIPTION :
            active_->captured ? XSR_STAGE_CAPTURE : XSR_STAGE_CAPTURE_START;
        abort_session(XSR_TIMEOUT, stage, now);
    }
    if (!ready_ || active_ || draining_session_) return;
    if (configure_retry_) {
        if (now >= *configure_retry_) send_configuration(now);
        return;
    }
    if (unload_retry_ ? now >= *unload_retry_ : model_state_ == XSR_MODEL_READY &&
        settings_.unload == UnloadPolicy::after_idle &&
        now - last_use_ >= std::chrono::minutes(settings_.idle_minutes)) unload(now);
}

void Broker::recover_profile(Clock::time_point now) {
    if (last_good_.available() && (last_good_.path != settings_.profile.path ||
                                  last_good_.sha256 != settings_.profile.sha256)) {
        settings_.profile = last_good_;
        configure(now);
    }
}

void Broker::configure(Clock::time_point now) {
    ++generation_;
    configuration_confirmed_ = false;
    configure_deadline_ = now + 20s;
    actual_compute_.clear();
    cpu_fallback_ = false;
    last_use_ = now;
    const bool load = settings_.profile.available() && settings_.load == LoadPolicy::on_activate;
    model_state_ = !settings_.profile.available() ? XSR_MODEL_MISSING :
        load ? XSR_MODEL_LOADING : XSR_MODEL_UNLOADED;
    configure_retry_.reset();
    unload_retry_.reset();
    if (ready_) send_configuration(now);
}

void Broker::send_configuration(Clock::time_point now) {
    const auto& profile = settings_.profile;
    const bool preload = profile.available() && settings_.load == LoadPolicy::on_activate;
    if (send_({{"type", "configure"}, {"generation", generation_}, {"path", profile.path},
               {"sha256", profile.sha256}, {"preload", preload}, {"compute", settings_.compute}}))
        configure_retry_.reset();
    else configure_retry_ = now + 100ms;
}

void Broker::set_settings(Settings settings, Clock::time_point now) {
    if (active_) {
        pending_ = std::move(settings);
        return;
    }
    const bool model_changed = settings_.profile.path != settings.profile.path ||
        settings_.profile.sha256 != settings.profile.sha256 || settings_.compute != settings.compute ||
        settings_.load != settings.load;
    if (settings_.unload != settings.unload) unload_retry_.reset();
    settings_ = std::move(settings);
    last_use_ = now;
    if (model_changed) configure(now);
}

void Broker::engine_online(Clock::time_point now) {
    ready_ = true;
    draining_session_ = 0;
    configure(now);
}

void Broker::engine_lost(Clock::time_point now) {
    ready_ = false;
    actual_compute_.clear();
    cpu_fallback_ = false;
    draining_session_ = 0;
    configure_retry_.reset();
    unload_retry_.reset();
    model_state_ = settings_.profile.available() ? XSR_MODEL_UNLOADED : XSR_MODEL_MISSING;
    finish(XSR_ENGINE_LOST, XSR_STAGE_NONE, {}, now);
}

void Broker::shutdown(Clock::time_point now) {
    ready_ = false;
    draining_session_ = 0;
    pending_.reset();
    if (active_) send_({{"type", "cancel"}, {"session", active_->id}});
    finish(XSR_CANCELLED, XSR_STAGE_NONE, {}, now);
    auto subscribers = std::move(subscribers_);
    subscribers_.clear();
    for (const auto& subscriber : subscribers) {
        deliver_({subscriber.first, XSR_SERVICE_STOPPED, XSR_SERVICE_UNAVAILABLE, 0});
    }
}

} // namespace xsr
