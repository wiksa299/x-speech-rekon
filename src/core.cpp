#include "core.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace xvr {

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

void Broker::status(XvrRequest& out) const {
    out.model_state = model_state_;
    out.service_status = ready_ && !draining_ ? XVR_OK : XVR_ENGINE_RESTARTING;
    out.supports_vocabulary = settings_.profile.supports_vocabulary();
    std::memset(out.model_id, 0, sizeof(out.model_id));
    std::memset(out.quantization, 0, sizeof(out.quantization));
    settings_.profile.id.copy(out.model_id, sizeof(out.model_id) - 1);
    settings_.profile.quant.copy(out.quantization, sizeof(out.quantization) - 1);
}

void Broker::request(int sender, XvrRequest& out, Clock::time_point now) {
    if (out.struct_size < offsetof(XvrRequest, status) + sizeof(out.status)) return;
    if (out.version != XVR_ABI_VERSION || out.struct_size < sizeof(XvrRequest)) {
        out.status = XVR_BAD_VERSION;
        return;
    }
    tick(now);
    out.status = XVR_OK;
    if (out.operation == XVR_SUBSCRIBE) {
        subscribers_.try_emplace(sender);
        status(out);
        return;
    }
    if (out.operation == XVR_GET_STATUS) {
        status(out);
        return;
    }
    auto subscriber = subscribers_.find(sender);
    if (subscriber == subscribers_.end()) {
        out.status = XVR_NOT_SUBSCRIBED;
        return;
    }
    switch (out.operation) {
    case XVR_UNSUBSCRIBE:
        forget(sender, now);
        return;
    case XVR_SET_VOCABULARY: {
        if (out.term_count > XVR_MAX_TERMS) {
            out.status = XVR_VOCABULARY_LIMIT;
            return;
        }
        if (out.term_count && !out.terms) {
            out.status = XVR_BAD_REQUEST;
            return;
        }
        std::set<std::string> seen;
        std::vector<std::string> terms;
        size_t bytes = 0;
        for (uint32_t i = 0; i < out.term_count; ++i) {
            const auto term = out.terms[i];
            if (term.bytes > XVR_MAX_TERM_BYTES || bytes + term.bytes > XVR_MAX_VOCABULARY_BYTES) {
                out.status = XVR_VOCABULARY_LIMIT;
                return;
            }
            if (!term.data || !term.bytes || !valid_utf8({term.data, term.bytes})) {
                out.status = XVR_BAD_REQUEST;
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
    case XVR_PTT_BEGIN:
        out.session = 0;
        out.status = begin(sender, {}, now, out.session);
        return;
    case XVR_PTT_END:
        out.status = release(sender, out.session, now);
        return;
    case XVR_PTT_CANCEL:
        if (!active_ || active_->client != sender || active_->id != out.session) {
            out.status = XVR_STALE_SESSION;
            return;
        }
        send_({{"type", "cancel"}, {"session", out.session}});
        draining_ = true;
        drain_deadline_ = now + 1s;
        finish(XVR_CANCELLED, XVR_STAGE_NONE, {}, now);
        return;
    default:
        out.status = XVR_BAD_REQUEST;
    }
}

uint32_t Broker::begin(int client, std::string wav, Clock::time_point now, uint64_t& session) {
    if (active_) return XVR_BUSY;
    if (!settings_.profile.available() || model_state_ == XVR_MODEL_MISSING) return XVR_MODEL_UNAVAILABLE;
    if (!ready_ || draining_) return XVR_ENGINE_RESTARTING;
    const auto id = ++next_session_;
    const bool sample = !wav.empty();
    Json request = {
        {"type", "begin"}, {"session", id}, {"generation", generation_},
        {"wav", std::move(wav)}, {"microphone", settings_.microphone},
        {"early", settings_.early && settings_.profile.streaming()},
        {"terms", client == local_client ? std::vector<std::string>{} : subscribers_.at(client)}
    };
    if (!send_(request)) return XVR_ENGINE_RESTARTING;
    session = id;
    const auto limit = model_state_ == XVR_MODEL_READY ? 10s : 20s;
    active_ = Session{client, id, now, now + (sample ? limit : 2s), settings_.profile,
                      false, sample};
    if (model_state_ == XVR_MODEL_UNLOADED) model_state_ = XVR_MODEL_LOADING;
    return XVR_OK;
}

uint32_t Broker::release(int client, uint64_t session, Clock::time_point now) {
    if (!active_ || active_->client != client || active_->id != session || active_->released) {
        return XVR_STALE_SESSION;
    }
    if (!send_({{"type", "end"}, {"session", session}})) return XVR_ENGINE_RESTARTING;
    active_->released = true;
    active_->deadline = now + (model_state_ == XVR_MODEL_READY ? 10s : 20s);
    return XVR_OK;
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
    if (active_ && active_->client == client) {
        send_({{"type", "cancel"}, {"session", active_->id}});
        draining_ = true;
        drain_deadline_ = now + 1s;
        finish(XVR_CANCELLED, XVR_STAGE_NONE, {}, now);
    }
}

void Broker::finish(uint32_t status_code, uint32_t stage, Json result, Clock::time_point now) {
    if (!active_) return;
    auto session = std::move(*active_);
    active_.reset();
    last_use_ = now;
    if (pending_) {
        auto updated = std::move(*pending_);
        pending_.reset();
        set_settings(std::move(updated), now);
    } else if (ready_ && settings_.unload == UnloadPolicy::after_use) {
        send_({{"type", "unload"}, {"generation", generation_}});
        model_state_ = XVR_MODEL_UNLOADED;
    }
    if (session.client == local_client || subscribers_.count(session.client)) {
        if (status_code == XVR_OK) {
            result["id"] = session.profile.id;
            result["quant"] = session.profile.quant;
            result["revision"] = session.profile.revision;
        }
        deliver_({session.client, XVR_RESULT, status_code, session.id, stage, std::move(result)});
    }
}

void Broker::accept(const Json& event, Clock::time_point now) {
    const auto type = event.value("type", "");
    if (type == "state" && event.value("generation", uint64_t(0)) == generation_) {
        model_state_ = event.at("state").get<uint32_t>();
        if (model_state_ == XVR_MODEL_READY) previous_.reset();
        if (model_state_ == XVR_MODEL_MISSING && previous_ && !active_) {
            settings_.profile = std::move(*previous_);
            previous_.reset();
            configure(now);
        }
    } else if (type == "result") {
        draining_ = false;
        if (!active_ || event.value("session", uint64_t(0)) != active_->id) return;
        const auto code = event.at("status").get<uint32_t>();
        const bool revert = code == XVR_MODEL_UNAVAILABLE && !pending_;
        finish(code, XVR_STAGE_NONE, code == XVR_OK ? event : Json::object(), now);
        if (revert && previous_ && !active_) {
            settings_.profile = std::move(*previous_);
            previous_.reset();
            configure(now);
        }
    } else if (type == "capture" && active_ && !active_->released && !active_->captured &&
               event.value("session", uint64_t(0)) == active_->id) {
        active_->captured = true;
        active_->deadline = active_->accepted + 30s;
        deliver_({active_->client, XVR_CAPTURE_STARTED, XVR_OK, active_->id});
    }
}

void Broker::tick(Clock::time_point now) {
    if (active_ && now >= active_->deadline) {
        const auto stage = active_->released ? XVR_STAGE_TRANSCRIPTION :
            active_->captured ? XVR_STAGE_CAPTURE : XVR_STAGE_CAPTURE_START;
        send_({{"type", "cancel"}, {"session", active_->id}});
        draining_ = true;
        drain_deadline_ = now + 1s;
        finish(XVR_TIMEOUT, stage, {}, now);
    }
    if (!active_ && !draining_ && model_state_ == XVR_MODEL_READY &&
        settings_.unload == UnloadPolicy::after_idle &&
        now - last_use_ >= std::chrono::minutes(settings_.idle_minutes)) {
        send_({{"type", "unload"}, {"generation", generation_}});
        model_state_ = XVR_MODEL_UNLOADED;
    }
}

void Broker::configure(Clock::time_point now) {
    ++generation_;
    last_use_ = now;
    const bool load = settings_.profile.available() && settings_.load == LoadPolicy::on_activate;
    model_state_ = !settings_.profile.available() ? XVR_MODEL_MISSING :
        load ? XVR_MODEL_LOADING : XVR_MODEL_UNLOADED;
    if (ready_) {
        const auto& profile = settings_.profile;
        send_({{"type", "configure"}, {"generation", generation_}, {"path", profile.path},
               {"sha256", profile.sha256}, {"preload", load}});
    }
}

void Broker::set_settings(Settings settings, Clock::time_point now) {
    if (active_) {
        pending_ = std::move(settings);
        return;
    }
    if (settings_.profile.path != settings.profile.path && settings_.profile.available() && !previous_) {
        previous_ = settings_.profile;
    }
    settings_ = std::move(settings);
    configure(now);
}

void Broker::engine_online(Clock::time_point now) {
    ready_ = true;
    draining_ = false;
    configure(now);
}

void Broker::engine_lost(Clock::time_point now) {
    ready_ = false;
    draining_ = false;
    model_state_ = settings_.profile.available() ? XVR_MODEL_UNLOADED : XVR_MODEL_MISSING;
    finish(XVR_ENGINE_LOST, XVR_STAGE_NONE, {}, now);
}

void Broker::shutdown(Clock::time_point now) {
    ready_ = false;
    draining_ = false;
    pending_.reset();
    if (active_) send_({{"type", "cancel"}, {"session", active_->id}});
    finish(XVR_CANCELLED, XVR_STAGE_NONE, {}, now);
    auto subscribers = std::move(subscribers_);
    subscribers_.clear();
    for (const auto& subscriber : subscribers) {
        deliver_({subscriber.first, XVR_SERVICE_STOPPED, XVR_SERVICE_UNAVAILABLE, 0});
    }
}

} // namespace xvr
