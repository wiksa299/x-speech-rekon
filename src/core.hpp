#pragma once

#include "xspeech_listen.h"
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

namespace xsl {

using Json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr size_t max_frame = 65536;
constexpr int local_client = -2;
constexpr int microphone_client = -3;
constexpr uint32_t sample_rate = 16000;
constexpr uint32_t max_samples = 30 * sample_rate;
constexpr uint32_t microphone_test_seconds = 10;
constexpr uint32_t max_result_size = XSL_MAX_TEXT_BYTES;
int default_cpu_threads(unsigned available);

std::string status_name(uint32_t status);
bool valid_utf8(std::string_view input);
bool read_frame(std::istream& stream, Json& message);
std::string wire_frame(const Json& message);

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
inline constexpr char moonshine_small_id[] = "moonshine-streaming-small";
inline constexpr char fluid_mini_id[] = "fluid-speech-mini";
inline constexpr char fluid_pro_id[] = "fluid-speech-pro";

struct Profile {
    std::string id;
    std::string quant;
    std::string revision;
    std::string path;
    std::string sha256;
    bool parakeet_v2() const { return id == parakeet_v2_id; }
    bool nemotron() const { return id == nemotron_id; }
    bool moonshine_small() const { return id == moonshine_small_id; }
    bool fluid() const { return id == fluid_mini_id || id == fluid_pro_id; }
    bool supports_beam() const { return parakeet_v2() || id == "parakeet-tdt-0.6b-v3" || fluid(); }
    bool supports_vocabulary_weight() const { return supports_beam() || moonshine_small(); }
    bool supported() const { return supports_vocabulary_weight() || nemotron(); }
    bool english_only() const { return parakeet_v2() || moonshine_small() || id == fluid_mini_id; }
    bool supports_vocabulary(bool early = false) const { return supported() && !(early && streaming()); }
    bool streaming() const { return nemotron() || moonshine_small(); }
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
    int cpu_threads = 0;
    int beam_width = 1;
    std::optional<bool> vocabulary_enabled;
    float vocabulary_weight = -1.0f;
    bool early = false;
    bool sidetone = false;
    bool low_latency_monitoring = false;
    uint32_t monitor_gain_percent = XSL_DEFAULT_MONITOR_GAIN_PERCENT;

    bool lexical_bias_enabled() const {
        return vocabulary_enabled.value_or(!profile.moonshine_small()) &&
            (!profile.supports_vocabulary_weight() || vocabulary_weight != 0.0f);
    }
};

struct Delivery {
    int client;
    uint32_t kind;
    uint32_t status;
    uint64_t session;
    uint32_t stage = XSL_STAGE_NONE;
    Json result = Json::object();
};

class Broker {
public:
    using Send = std::function<bool(const Json&)>;
    using Deliver = std::function<void(const Delivery&)>;
    Broker(Send send, Deliver deliver);
    void request(int sender, XslRequest& request, Clock::time_point now);
    void accept(const Json& event, Clock::time_point now);
    void tick(Clock::time_point now);
    void set_settings(Settings settings, Clock::time_point now);
    void engine_online(Clock::time_point now);
    void restore_last_good(Profile profile) { last_good_ = std::move(profile); }
    const Profile& last_good() const { return last_good_; }
    void engine_lost(Clock::time_point now, uint32_t unavailable = XSL_ENGINE_RESTARTING);
    void shutdown(Clock::time_point now);
    void forget(int client, Clock::time_point now);
    uint32_t begin_local(std::string wav, Clock::time_point now, uint64_t& session);
    uint32_t end_local(uint64_t session, Clock::time_point now);
    void cancel_local(Clock::time_point now);
    uint32_t begin_microphone_test(Clock::time_point now, uint64_t& session);
    uint32_t end_microphone_test(uint64_t session, Clock::time_point now);
    void cancel_microphone_test(Clock::time_point now);
    bool busy() const { return active_.has_value(); }
    bool ready() const { return ready_; }
    bool service_ready() const { return ready_ && configuration_confirmed_ && !draining_session_; }
    uint32_t service_status() const { return service_ready() ? uint32_t(XSL_OK) : engine_unavailable_; }
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

    uint32_t begin(int client, std::string wav, Clock::time_point now, uint64_t& session, uint32_t monitor_gain_percent);
    uint32_t release(int client, uint64_t session, Clock::time_point now);
    void finish(uint32_t status, uint32_t stage, Json result, Clock::time_point now);
    void configure(Clock::time_point now);
    void send_configuration(Clock::time_point now);
    void abort_session(uint32_t status, uint32_t stage, Clock::time_point now);
    void recover_profile(Clock::time_point now);
    void unload(Clock::time_point now);
    void status(XslRequest&) const;

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
    uint32_t model_state_ = XSL_MODEL_MISSING;
    uint32_t engine_unavailable_ = XSL_ENGINE_RESTARTING;
    std::string actual_compute_;
    bool cpu_fallback_ = false;
    bool ready_ = false, configuration_confirmed_ = false;
    uint64_t draining_session_ = 0;
    std::optional<Clock::time_point> configure_retry_, unload_retry_;
    Clock::time_point drain_deadline_{}, configure_deadline_{};
};

} // namespace xsl
