#pragma once

#include "soulcam.h"
#include "store/store.h"
#include "soullink/json.h"
#include "soullink/sync_engine.h"

#include <mosquitto.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sc::soullink {

struct SysCmdRequest {
    int subcmd = -1;
    JsonValue data;
};

class Module {
public:
    explicit Module(const sc::Config& cfg);
    ~Module();

    bool start();
    void stop();
    void poll();

    bool popSysCmd(SysCmdRequest& out);
    void respondSysCmd(bool success, const std::string& message,
                       const JsonValue& data = JsonValue());
    void submitAIDetections(
        const std::vector<sc::Detection>& detections,
        int frame_width,
        int frame_height,
        const std::string& tracking_mode,
        int raw_count);
    bool isStreamSubscribed() const { return stream_subscribed_.load(); }

private:
    bool init_identity();
    bool start_mdns();
    void stop_mdns();
    void maintain_mdns();

    bool start_mqtt();
    void stop_mqtt();
    void handle_downlink_payload(const std::string& payload);
    void publish_notification(const std::string& text, const std::string& type);
    void publish_sync_progress(int progress);
    void publish_sync_result(bool success, const std::string& details);

    static void mqtt_on_connect(struct mosquitto* mosq, void* userdata, int rc);
    static void mqtt_on_disconnect(struct mosquitto* mosq, void* userdata, int rc);
    static void mqtt_on_message(
        struct mosquitto* mosq,
        void* userdata,
        const struct ::mosquitto_message* msg);

    void dispatch_command(const JsonValue& msg);
    void handle_set_dp(const JsonValue& data);
    void handle_get_dp(const JsonValue& data);
    void handle_get_dp_all();
    void handle_disconnect_server();
    void handle_sub_stream(const JsonValue& data);
    void handle_unsub_stream(const JsonValue& data);
    void handle_streaming(const JsonValue& data);
    void handle_sync_files(const JsonValue& data);
    void handle_sys_cmd(const JsonValue& data);
    void publish_dp_info();
    void publish_rtsp_info();
    void publish_ai_detections_if_due();
    void publish_unsupported(const JsonValue& cmd_value);

    bool publish_json(const std::string& topic, const JsonValue& payload);
    bool publish_binary(const std::string& topic, const std::vector<uint8_t>& payload);
    void reconnect_mqtt_to_host(const std::string& host);

    bool start_control_api();
    void stop_control_api();
    void control_api_loop();

    bool start_frame_receiver();
    void stop_frame_receiver();
    void frame_receiver_loop();

    bool check_rtsp_reachable() const;
    void publish_health();
    void publish_stream_state(uint8_t old_status, uint8_t new_status, int stream_index = -1);
    std::string make_stream_topic(int stream_index) const;

    static std::string normalize_topic_prefix(const std::string& topic_prefix);
    static std::string normalize_mdns_type(const std::string& service_type);
    static std::string detect_local_ip();
    static std::string derive_service_identifier(const std::string& override_value);
    static bool command_exists(const char* name);

    const sc::Config cfg_;
    const sc::SoullinkConfig sl_cfg_;

    std::string local_ip_;
    std::string service_identifier_;
    std::string client_id_;

    std::string topic_downlink_primary_;
    std::string topic_downlink_compat_;
    std::string topic_out_;
    std::string topic_msg_;
    std::string topic_stream_;
    std::atomic<int> active_stream_index_{0};

    pid_t mdns_pid_ = -1;
    struct mosquitto* mosq_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> stream_subscribed_{false};
    std::atomic<bool> mqtt_online_{false};
    std::atomic<bool> mqtt_suspended_{false};
    std::atomic<bool> mqtt_suspend_requested_{false};
    std::atomic<bool> frame_receiver_started_{false};

    std::thread frame_receiver_thread_;
    std::thread control_api_thread_;

    std::mutex mqtt_mu_;
    std::string mqtt_host_runtime_;
    int control_api_listen_fd_ = -1;

    SyncEngine sync_engine_;
    std::chrono::steady_clock::time_point last_health_publish_{};
    std::chrono::steady_clock::time_point last_mdns_refresh_{};

    std::mutex sys_cmd_mu_;
    std::queue<SysCmdRequest> pending_sys_cmds_;

    std::mutex ai_detections_mu_;
    JsonValue pending_ai_detections_payload_;
    bool has_pending_ai_detections_ = false;
    std::chrono::steady_clock::time_point last_ai_detections_publish_{};
};

}  // namespace sc::soullink

