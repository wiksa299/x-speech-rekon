#pragma once

#include "xspeech_rekon.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xsr {

using Json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr size_t max_frame = 65536;
constexpr int local_client = -2;
constexpr uint32_t sample_rate = 16000;
constexpr uint32_t max_samples = 30 * sample_rate;
constexpr uint32_t max_result_size = XSR_MAX_TEXT_BYTES;

std::string status_name(uint32_t status);
bool valid_utf8(std::string_view input);
bool read_frame(std::istream& stream, Json& message);
std::string wire_frame(const Json& message);
fs::path executable_path(const char* argv0);

class Process {
public:
    explicit Process(fs::path executable);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    bool start();
    bool send(const Json& message);
    std::vector<Json> receive();
    bool failed() const;
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

inline constexpr char parakeet_v2_id[] = "parakeet-tdt-0.6b-v2";
inline constexpr char nemotron_id[] = "nemotron-3.5-asr-streaming-0.6b";
inline constexpr char whisper_cpp_id[] = "whisper-small.en-whisper-cpp";
inline constexpr char whisper_transcribe_id[] = "whisper-small.en-transcribe";
inline constexpr size_t whisper_chunk_samples = 8000;

struct Profile {
    std::string id;
    std::string quant;
    std::string revision;
    std::string path;
    std::string sha256;
    bool parakeet_v2() const { return id == parakeet_v2_id; }
    bool nemotron() const { return id == nemotron_id; }
    bool moonshine_voice() const { return id == "moonshine-voice-small" || id == "moonshine-voice-medium"; }
    bool whisper_cpp() const { return id == whisper_cpp_id; }
    bool whisper() const { return whisper_cpp() || id == whisper_transcribe_id; }
    bool qwen() const { return id == XSR_MODEL_QWEN3_ASR_06B; }
    bool english_only() const { return parakeet_v2() || whisper() || moonshine_voice() || (streaming() && !nemotron()); }
    bool supports_vocabulary(bool early = false) const {
        return qwen() || whisper() || moonshine_voice() || parakeet_v2() || id == "parakeet-tdt-0.6b-v3" || (nemotron() && !early);
    }
    bool streaming() const { return nemotron() || id.rfind("moonshine-streaming-", 0) == 0; }
    bool available() const { return !path.empty(); }
};

enum class LoadPolicy { on_use, on_activate };
enum class UnloadPolicy { after_use, on_deactivate, after_idle };

struct Settings {
    Profile profile;
    std::string microphone;
    std::string compute = "cpu";
    std::string language = "auto";
    LoadPolicy load = LoadPolicy::on_use;
    UnloadPolicy unload = UnloadPolicy::after_idle;
    int idle_minutes = 5;
    bool early = false;
    bool sidetone = false;
};

struct Delivery {
    int client;
    uint32_t kind;
    uint32_t status;
    uint64_t session;
    uint32_t stage = XSR_STAGE_NONE;
    Json result = Json::object();
};

class Broker {
public:
    using Send = std::function<bool(const Json&)>;
    using Deliver = std::function<void(const Delivery&)>;
    Broker(Send send, Deliver deliver);
    void request(int sender, XsrRequest& request, Clock::time_point now);
    void accept(const Json& event, Clock::time_point now);
    void tick(Clock::time_point now);
    void set_settings(Settings settings, Clock::time_point now);
    void engine_online(Clock::time_point now);
    void restore_last_good(Profile profile) { last_good_ = std::move(profile); }
    const Profile& last_good() const { return last_good_; }
    void engine_lost(Clock::time_point now);
    void shutdown(Clock::time_point now);
    void forget(int client, Clock::time_point now);
    uint32_t begin_local(std::string wav, Clock::time_point now, uint64_t& session);
    uint32_t end_local(uint64_t session, Clock::time_point now);
    void cancel_local(Clock::time_point now);
    bool busy() const { return active_.has_value(); }
    bool ready() const { return ready_; }
    bool service_ready() const { return ready_ && configuration_confirmed_ && !draining_session_; }
    bool restart_due(Clock::time_point now) const { return (draining_session_ && now >= drain_deadline_) ||
        (ready_ && !configuration_confirmed_ && now >= configure_deadline_); }
    uint32_t model_state() const { return model_state_; }
    const std::string& actual_compute() const { return actual_compute_; }
    bool cpu_fallback() const { return cpu_fallback_; }
    const Settings& settings() const { return settings_; }
    const std::map<int, std::vector<std::string>>& subscribers() const { return subscribers_; }

private:
    struct Session {
        int client;
        uint64_t id;
        Clock::time_point accepted;
        Clock::time_point deadline;
        Profile profile;
        bool captured = false;
        bool released = false;
    };

    struct PttHints {
        std::vector<std::string> terms;
        std::string context;
    };
    uint32_t begin(int client, std::string wav, Clock::time_point now, uint64_t& session,
                   uint32_t monitor_gain_percent = XSR_DEFAULT_MONITOR_GAIN_PERCENT, const PttHints* hints = nullptr);
    uint32_t release(int client, uint64_t session, Clock::time_point now);
    void finish(uint32_t status, uint32_t stage, Json result, Clock::time_point now);
    void configure(Clock::time_point now);
    void send_configuration(Clock::time_point now);
    void abort_session(uint32_t status, uint32_t stage, Clock::time_point now);
    void recover_profile(Clock::time_point now);
    void unload(Clock::time_point now);
    void status(XsrRequest&) const;

    Send send_;
    Deliver deliver_;
    Settings settings_;
    std::optional<Settings> pending_;
    Profile last_good_;
    std::optional<Session> active_;
    std::map<int, std::vector<std::string>> subscribers_;
    Clock::time_point last_use_{};
    uint64_t next_session_ = 0;
    uint64_t generation_ = 0;
    uint32_t model_state_ = XSR_MODEL_MISSING;
    std::string actual_compute_;
    bool cpu_fallback_ = false;
    bool ready_ = false, configuration_confirmed_ = false;
    uint64_t draining_session_ = 0;
    std::optional<Clock::time_point> configure_retry_, unload_retry_;
    Clock::time_point drain_deadline_{}, configure_deadline_{};
};

} // namespace xsr
