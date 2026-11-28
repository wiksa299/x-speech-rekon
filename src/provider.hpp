#pragma once

#include "activity.hpp"
#include "core.hpp"
#include "window.hpp"

#include <XPLMMenus.h>
#include <XPLMPlugin.h>

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <future>
#include <initializer_list>
#include <mutex>
#include <thread>

namespace xsl {

struct MicrophoneTest {
    uint64_t session = 0;
    uint32_t status = XSL_OK, capture_ms = 0, clipped = 0;
    float level = -120, peak = -120;
    std::optional<float> rms;
    std::string device;
    bool capturing = false, ending = false, has_take = false;
    bool playing = false, playback_failed = false;
};

struct FileRequest {
    uint64_t id = 0;
    Clock::time_point deadline{};
    explicit operator bool() const { return id != 0; }
    bool matches(uint64_t ticket) const { return id != 0 && id == ticket; }
    bool expired(Clock::time_point now) const { return id != 0 && now >= deadline; }
};

class PreferencesWriter {
public:
    explicit PreferencesWriter(fs::path path);
    ~PreferencesWriter();
    bool failed() const { return failed_; }
    void save(std::string contents);

private:
    void run();
    fs::path path_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<std::string> pending_;
    bool stopping_ = false;
    std::atomic<bool> failed_{false};
    std::thread worker_;
};

enum class TestSource { microphone, reference, wav };

struct Incident {
    std::time_t when;
    uint32_t status;
    uint32_t stage;
    TestSource source;
};

struct Activity {
    uint64_t sequence;
    int client;
    uint64_t session;
    std::string name;
    uint32_t status;
    uint32_t stage = XSL_STAGE_NONE;
    uint32_t vocabulary = XSL_VOCAB_NONE;
    std::string model, quant, text;
    ActivityProgress progress;
    std::optional<uint32_t> latency_ms;
    TestSource source = TestSource::microphone;
    bool pending = false;
    bool model_was_loaded = false;
};

class Provider {
public:
    Provider();
    ~Provider();

    bool service_ready() const { return broker_.service_ready(); }
    int model_state() const { return static_cast<int>(broker_.model_state()); }
    bool ptt_busy() const { return broker_.busy(); }
    void request(int sender, XslRequest& command);

private:
    enum class EngineMode { enabled, suspended, disabled };
    static void menu_callback(void* owner, void*);
    static float tick_callback(float, float, int, void* owner);
    void record_incident(uint32_t status, uint32_t stage = XSL_STAGE_NONE,
                         TestSource source = TestSource::microphone);
    void engine_started(bool started, Clock::time_point now);
    void restart_engine(bool recovery = false);
    void change_engine_mode(EngineMode mode);
    void reset_engine(uint32_t status, Clock::time_point now);
    bool engine_controls_busy() const;
    const char* engine_state_label() const;
    void clear_file_requests();
    void notify_diagnostic(XPLMPluginID id);
    void refresh_language();
    float tick();
    void on_engine_event(const Json& message, Clock::time_point now);
    void dispatch_events();
    void add_activity(int client, uint64_t session, uint32_t status, std::string name,
                      TestSource source, bool model_was_loaded, Clock::time_point now);
    Activity* pending_activity(int client, uint64_t session);
    void cancel_activity(int client);

    void load_catalog();
    void load_preferences();
    std::string default_quantization_for(const std::string& model) const;
    static bool string_fields(const Json& value, std::initializer_list<const char*> fields);
    static bool valid_catalog_entry(const Json& entry);
    static Profile decode_profile(const Json& value);
    void save_preferences();
    bool file_request(FileRequest& pending, Json command, Clock::duration timeout);
    void scan();
    void change_models_folder(std::string path, bool require_directory);
    void identify(const std::string& path);
    std::string candidate_path(bool allow_previous = true) const;
    const Json* selected_entry() const;
    std::string compute_label(const std::string& id) const;

    static const char* status_label(uint32_t code);
    static const char* activity_status(uint32_t status, uint32_t stage, TestSource source);
    static const char* test_source_label(TestSource source);
    static void emphasized_text(const char* text, float extra_size = 3);
    static constexpr ImVec4 green_text{0.57f, 0.84f, 0.68f, 1};
    static constexpr ImVec4 amber_text{0.94f, 0.73f, 0.46f, 1};
    static constexpr ImVec4 primary_button{0.16f, 0.36f, 0.57f, 1};

    void draw();
    bool draw_engine_settings();
    bool draw_models();
    void draw_model_heading();
    void draw_models_folder();
    bool draw_model_memory();
    bool draw_compute();
    bool draw_recognition_settings();
    void draw_microphone_button();
    bool draw_microphone_selector();
    void draw_microphone_panel();
    void draw_microphone_levels();
    void request_microphone_panel();
    void close_microphone_panel();
    void clear_microphone_test();
    void draw_test_page();
    void draw_tests();
    void draw_activity();
    void draw_recent_requests();
    void draw_micro_indicator();
    void update_micro_indicator();
    void draw_micro_settings();
    void draw_settings();
    void draw_diagnostic();
    void start_test(TestSource source, std::string wav = {});
    std::string microphone_label() const;
    std::vector<std::pair<std::string, std::string>> diagnostic_fields() const;

    fs::path root_, preferences_, folder_;
    std::string folder_draft_;
    std::optional<std::string> folder_change_;
    PreferencesWriter writer_;
    Process process_;
    std::future<bool> engine_transition_;
    Broker broker_;
    Window window_;
    std::unique_ptr<Window> micro_window_;
    Window::Bounds micro_bounds_{};
    int micro_opacity_ = 65, micro_border_opacity_ = 50;
    bool show_micro_indicator_ = false, show_micro_requester_ = false;
    XPLMMenuID menu_ = nullptr;
    int menu_index_ = 0, settings_item_ = 0;
    std::map<int, std::pair<std::string, std::string>> subscribers_;
    std::deque<Activity> history_;
    std::deque<Incident> incidents_;
    std::future<bool> clipboard_copy_;
    std::optional<bool> clipboard_result_;
    int xplane_version_ = 0, sdk_version_ = 0;
    EngineMode engine_mode_ = EngineMode::enabled;
    int available_cpu_threads_ = 1, requested_cpu_threads_ = 1;
    bool manually_restarting_ = false;
    uint64_t activity_sequence_ = 0;
    std::vector<Delivery> pending_events_;
    Json catalog_ = Json::array(), models_ = Json::array(), microphones_ = Json::array(), compute_devices_ = Json::array();
    Settings draft_;
    int idle_minutes_draft_ = draft_.idle_minutes;
    std::string selected_id_ = "parakeet-tdt-0.6b-v3", selected_quant_;
    std::string startup_path_, persisted_profile_, download_state_, test_text_, engine_version_;
    std::string persisted_good_path_, persisted_good_hash_;
    uint64_t next_ticket_ = 0, downloaded_ = 0, download_total_ = 0, test_session_ = 0;
    FileRequest scan_ticket_, identify_ticket_, browse_ticket_, download_ticket_, wav_ticket_;
    uint32_t engine_status_ = XSL_ENGINE_RESTARTING, test_status_ = XSL_OK;
    const char* engine_failure_ = nullptr;
    TestSource test_source_ = TestSource::microphone;
    std::optional<uint32_t> test_latency_ms_;
    bool wav_picker_failed_ = false, folder_picker_failed_ = false, scan_error_ = false, catalog_error_ = false;
    float test_level_ = 0;
    MicrophoneTest microphone_test_;
    bool microphone_panel_open_ = false, microphone_panel_requested_ = false;
    Json last_monitoring_;
    bool model_error_ = false, boot_selection_ = true, waiting_for_hello_ = false, shutting_down_ = false;
    bool model_picker_edited_ = false;
    bool local_held_ = false, test_pending_ = false, listening_ = false;
    bool scan_deferred_ = false, protocol_fault_ = false, profile_recovered_ = false;
    Clock::time_point retry_at_{}, hello_deadline_{}, preferences_retry_at_{};
};

} // namespace xsl
