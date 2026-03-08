#include "soullink/module.h"

#include "util/logger.h"

#include <mosquitto.h>

#include <arpa/inet.h>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sc::soullink {

namespace {

constexpr int kCmdSetDp = 0;
constexpr int kCmdGetDp = 1;
constexpr int kCmdGetDpAll = 2;
constexpr int kCmdSubStream = 4;
constexpr int kCmdUnsubStream = 5;
constexpr int kCmdDisconnectServer = 12;
constexpr int kCmdSyncFiles = 13;
constexpr int kCmdSoulReload = 14;
constexpr int kCmdStreaming = 18;
constexpr int kCmdDirectorPlay = 20;
constexpr int kCmdSysCmd = 21;

static std::string trim_copy(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) begin++;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(begin, end - begin);
}

static bool fork_exec_detached(const std::vector<std::string>& args, pid_t* child_pid) {
    if (args.empty() || child_pid == nullptr) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    *child_pid = pid;
    return true;
}

static void kill_child(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 20; ++i) {
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) return;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

static bool g_mosq_initialized = false;

}  // namespace

Module::Module(const sc::Config& cfg)
    : cfg_(cfg), sl_cfg_(cfg.soullink), sync_engine_(cfg.soullink) {}

Module::~Module() { stop(); }

bool Module::start() {
    if (!sl_cfg_.enable) {
        SC_LOG_INFO("Soullink module disabled by configuration");
        return false;
    }
    if (running_) return true;
    if (!init_identity()) return false;

    running_ = true;
    mqtt_suspended_ = false;
    mqtt_suspend_requested_ = false;
    last_health_publish_ = std::chrono::steady_clock::now();
    last_mdns_refresh_ = std::chrono::steady_clock::now();

    start_mdns();
    start_control_api();
    start_mqtt();
    start_frame_receiver();

    SC_LOG_INFO("Soullink module started: serviceId=%s clientId=%s",
                service_identifier_.c_str(),
                client_id_.c_str());
    return true;
}

void Module::stop() {
    if (!running_) return;
    running_ = false;
    stop_frame_receiver();
    stop_control_api();
    stop_mqtt();
    stop_mdns();
    SC_LOG_INFO("Soullink module stopped");
}

void Module::poll() {
    if (!running_) return;

    if (mqtt_suspend_requested_.exchange(false)) {
        stop_mqtt();
        SC_LOG_INFO("Soullink MQTT suspended by disconnectServer");
    }

    // Ivy-aligned mDNS lifecycle: advertise only while MQTT is not connected.
    // SoulFlow cleanup now only clears `found` (not `connected`), so stopping
    // mDNS while connected is safe.
    if (!mqtt_online_ || mqtt_suspended_) {
        maintain_mdns();
    } else if (mdns_pid_ > 0) {
        SC_LOG_INFO("Soullink mDNS stopped (MQTT connected)");
        stop_mdns();
    }

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_health_publish_).count() >=
        sl_cfg_.status_interval_sec) {
        publish_health();
        last_health_publish_ = now;
    }
}

bool Module::init_identity() {
    local_ip_ = detect_local_ip();
    service_identifier_ = derive_service_identifier(sl_cfg_.service_identifier);
    if (service_identifier_.empty()) service_identifier_ = "soulcam";

    client_id_ = sl_cfg_.use_composite_client_id
        ? (sl_cfg_.device_type + ":" + service_identifier_)
        : service_identifier_;

    const std::string prefix = normalize_topic_prefix(sl_cfg_.mqtt_topic_prefix);
    topic_downlink_primary_ = prefix + "in/" + service_identifier_;
    topic_downlink_compat_ = prefix + "in/" + client_id_;
    topic_out_ = prefix + "out/" + client_id_;
    topic_msg_ = prefix + "m/" + client_id_;
    topic_stream_ = prefix + "s/" + client_id_ + "/" + std::to_string(sl_cfg_.stream_index);
    active_stream_index_ = sl_cfg_.stream_index;
    mqtt_host_runtime_ = sl_cfg_.mqtt_host;
    return true;
}

bool Module::start_mdns() {
    if (!command_exists("avahi-publish-service")) {
        SC_LOG_WARN("Soullink mDNS: avahi-publish-service not found");
        return false;
    }
    std::vector<std::string> args{
        "avahi-publish-service",
        service_identifier_,
        normalize_mdns_type(sl_cfg_.mdns_service_type),
        std::to_string(sl_cfg_.api_port),
        ("deviceType=" + sl_cfg_.device_type),
        ("serviceIdentifier=" + service_identifier_),
        ("ip=" + local_ip_),
    };
    if (!fork_exec_detached(args, &mdns_pid_)) {
        SC_LOG_WARN("Soullink mDNS: failed to launch avahi publisher");
        mdns_pid_ = -1;
        return false;
    }
    SC_LOG_INFO("Soullink mDNS advertised as %s.%s",
                service_identifier_.c_str(),
                normalize_mdns_type(sl_cfg_.mdns_service_type).c_str());
    return true;
}

void Module::stop_mdns() {
    if (mdns_pid_ > 0) {
        kill_child(mdns_pid_);
        mdns_pid_ = -1;
    }
}

void Module::maintain_mdns() {
    bool need_restart = false;

    if (mdns_pid_ > 0) {
        int status = 0;
        pid_t rc = waitpid(mdns_pid_, &status, WNOHANG);
        if (rc == mdns_pid_) {
            SC_LOG_WARN("Soullink mDNS publisher exited unexpectedly; restarting");
            mdns_pid_ = -1;
            need_restart = true;
        }
    } else {
        need_restart = true;
    }

    // Periodic re-announcement: avahi-publish-service only sends a gratuitous
    // announcement at launch.  Ivy calls mdns_announce(true) every 5 s to stay
    // visible; we achieve the same by cycling the publisher process.
    if (!need_restart && mdns_pid_ > 0) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_mdns_refresh_).count();
        if (elapsed >= sl_cfg_.mdns_refresh_sec) {
            need_restart = true;
        }
    }

    if (need_restart) {
        stop_mdns();
        start_mdns();
        last_mdns_refresh_ = std::chrono::steady_clock::now();
    }
}

bool Module::start_mqtt() {
    std::lock_guard<std::mutex> lk(mqtt_mu_);
    if (mqtt_suspended_) {
        SC_LOG_INFO("Soullink MQTT start skipped (suspended)");
        return false;
    }
    if (!g_mosq_initialized) {
        if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
            SC_LOG_WARN("Soullink MQTT: mosquitto_lib_init failed");
            return false;
        }
        g_mosq_initialized = true;
    }
    mosq_ = mosquitto_new(client_id_.c_str(), true, this);
    if (!mosq_) {
        SC_LOG_WARN("Soullink MQTT: mosquitto_new failed");
        return false;
    }

    if (!sl_cfg_.mqtt_username.empty()) {
        int rc = mosquitto_username_pw_set(
            mosq_,
            sl_cfg_.mqtt_username.c_str(),
            sl_cfg_.mqtt_password.empty() ? nullptr : sl_cfg_.mqtt_password.c_str());
        if (rc != MOSQ_ERR_SUCCESS) {
            SC_LOG_WARN("Soullink MQTT: failed to set username/password (rc=%d)", rc);
        }
    }

    mosquitto_connect_callback_set(mosq_, &Module::mqtt_on_connect);
    mosquitto_disconnect_callback_set(mosq_, &Module::mqtt_on_disconnect);
    mosquitto_message_callback_set(mosq_, &Module::mqtt_on_message);

    int rc = mosquitto_connect_async(mosq_, mqtt_host_runtime_.c_str(), sl_cfg_.mqtt_port, 30);
    if (rc != MOSQ_ERR_SUCCESS) {
        SC_LOG_WARN("Soullink MQTT: connect failed rc=%d (%s)", rc, mosquitto_strerror(rc));
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        return false;
    }

    rc = mosquitto_loop_start(mosq_);
    if (rc != MOSQ_ERR_SUCCESS) {
        SC_LOG_WARN("Soullink MQTT: loop_start failed rc=%d (%s)", rc, mosquitto_strerror(rc));
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        return false;
    }

    SC_LOG_INFO("Soullink MQTT connecting to %s:%d", mqtt_host_runtime_.c_str(), sl_cfg_.mqtt_port);
    return true;
}

void Module::stop_mqtt() {
    std::lock_guard<std::mutex> lk(mqtt_mu_);
    mqtt_online_ = false;
    if (!mosq_) return;
    mosquitto_disconnect(mosq_);
    mosquitto_loop_stop(mosq_, true);
    mosquitto_destroy(mosq_);
    mosq_ = nullptr;
}

void Module::reconnect_mqtt_to_host(const std::string& host) {
    const std::string trimmed = trim_copy(host);
    if (trimmed.empty()) return;
    const bool host_changed = trimmed != mqtt_host_runtime_;
    if (host_changed) {
        SC_LOG_INFO("Soullink MQTT broker updated: %s -> %s", mqtt_host_runtime_.c_str(), trimmed.c_str());
        mqtt_host_runtime_ = trimmed;
    }
    if (mqtt_suspended_.exchange(false)) {
        SC_LOG_INFO("Soullink MQTT resume requested via /debug handshake");
    }
    if (host_changed || !mosq_ || !mqtt_online_) {
        stop_mqtt();
        start_mqtt();
    }
}

void Module::mqtt_on_connect(struct mosquitto* mosq, void* userdata, int rc) {
    auto* self = static_cast<Module*>(userdata);
    if (!self) return;
    if (rc != 0) {
        self->mqtt_online_ = false;
        SC_LOG_WARN("Soullink MQTT connect failed rc=%d", rc);
        return;
    }
    self->mqtt_online_ = true;
    mosquitto_subscribe(mosq, nullptr, self->topic_downlink_primary_.c_str(), 0);
    if (self->topic_downlink_compat_ != self->topic_downlink_primary_) {
        mosquitto_subscribe(mosq, nullptr, self->topic_downlink_compat_.c_str(), 0);
    }
    SC_LOG_INFO("Soullink MQTT subscribed to %s%s%s",
                self->topic_downlink_primary_.c_str(),
                self->topic_downlink_compat_ != self->topic_downlink_primary_ ? ", " : "",
                self->topic_downlink_compat_ != self->topic_downlink_primary_
                    ? self->topic_downlink_compat_.c_str()
                    : "");
}

void Module::mqtt_on_disconnect(struct mosquitto* /*mosq*/, void* userdata, int rc) {
    auto* self = static_cast<Module*>(userdata);
    if (!self) return;
    self->mqtt_online_ = false;
    SC_LOG_WARN("Soullink MQTT disconnected rc=%d", rc);
}

void Module::mqtt_on_message(
    struct mosquitto* /*mosq*/,
    void* userdata,
    const struct ::mosquitto_message* msg) {
    auto* self = static_cast<Module*>(userdata);
    if (!self || !msg || !msg->payload) return;
    std::string payload(static_cast<const char*>(msg->payload), msg->payloadlen);
    self->handle_downlink_payload(payload);
}

void Module::handle_downlink_payload(const std::string& payload) {
    JsonValue msg;
    std::string err;
    if (!json_parse(payload, &msg, &err)) {
        SC_LOG_WARN("Soullink MQTT: invalid downlink JSON (%s)", err.c_str());
        return;
    }
    dispatch_command(msg);
}

void Module::dispatch_command(const JsonValue& msg) {
    if (!msg.is_object()) {
        publish_unsupported(JsonValue("invalid"));
        return;
    }
    const JsonValue* cmd_v = msg.get("cmd");
    if (!cmd_v) {
        publish_unsupported(JsonValue("missing_cmd"));
        return;
    }

    int cmd = -1;
    if (cmd_v->is_number()) {
        cmd = cmd_v->as_int(-1);
    } else if (cmd_v->is_string()) {
        const std::string& s = cmd_v->as_string();
        if (s == "setDp") cmd = kCmdSetDp;
        else if (s == "getDp") cmd = kCmdGetDp;
        else if (s == "getDpAll") cmd = kCmdGetDpAll;
        else if (s == "subStream") cmd = kCmdSubStream;
        else if (s == "unsubStream") cmd = kCmdUnsubStream;
        else if (s == "streaming") cmd = kCmdStreaming;
        else if (s == "syncFiles") cmd = kCmdSyncFiles;
        else if (s == "disconnectServer") cmd = kCmdDisconnectServer;
        else if (s == "soulReload") cmd = kCmdSoulReload;
        else if (s == "directorPlay") cmd = kCmdDirectorPlay;
        else if (s == "sysCmd") cmd = kCmdSysCmd;
    }

    const JsonValue* data = msg.get("data");
    switch (cmd) {
        case kCmdSetDp: handle_set_dp(data ? *data : JsonValue(JsonValue::Array{})); break;
        case kCmdGetDp: handle_get_dp(data ? *data : JsonValue(JsonValue::Array{})); break;
        case kCmdGetDpAll: handle_get_dp_all(); break;
        case kCmdDisconnectServer: handle_disconnect_server(); break;
        case kCmdSubStream: handle_sub_stream(data ? *data : JsonValue(0.0)); break;
        case kCmdUnsubStream: handle_unsub_stream(data ? *data : JsonValue(0.0)); break;
        case kCmdStreaming: handle_streaming(data ? *data : JsonValue(0.0)); break;
        case kCmdSyncFiles: handle_sync_files(data ? *data : JsonValue(JsonValue::Object{})); break;
        case kCmdSoulReload:
        case kCmdDirectorPlay:
        case kCmdSysCmd:
            publish_notification("Command received but not implemented", "warning");
            break;
        default: publish_unsupported(*cmd_v); break;
    }
}

void Module::handle_set_dp(const JsonValue& data) {
    JsonValue::Array updates;
    if (data.is_array()) {
        std::lock_guard<std::mutex> lk(dp_mu_);
        for (const auto& item : data.as_array()) {
            if (!item.is_object()) continue;
            const auto* dp = item.get("dp");
            const auto* value = item.get("value");
            if (!dp || !dp->is_number() || !value) continue;
            int dp_id = dp->as_int();
            dp_values_[dp_id] = *value;

            JsonValue::Object row;
            row["dp"] = JsonValue(static_cast<int64_t>(dp_id));
            row["value"] = *value;
            updates.emplace_back(JsonValue(std::move(row)));
        }
    }

    JsonValue::Object out;
    out["cmd"] = JsonValue(static_cast<int64_t>(kCmdGetDp));  // SoulFlow parser expects cmd=1
    out["data"] = JsonValue(std::move(updates));
    publish_json(topic_out_, JsonValue(std::move(out)));
}

void Module::handle_get_dp(const JsonValue& data) {
    JsonValue::Array rows;
    if (data.is_array()) {
        std::lock_guard<std::mutex> lk(dp_mu_);
        for (const auto& item : data.as_array()) {
            int dp_id = -1;
            if (item.is_number()) dp_id = item.as_int(-1);
            else if (item.is_object()) {
                if (const auto* dp = item.get("dp"); dp && dp->is_number()) dp_id = dp->as_int(-1);
            }
            if (dp_id < 0) continue;
            JsonValue::Object row;
            row["dp"] = JsonValue(static_cast<int64_t>(dp_id));
            auto it = dp_values_.find(dp_id);
            row["value"] = (it != dp_values_.end()) ? it->second : JsonValue(0.0);
            rows.emplace_back(JsonValue(std::move(row)));
        }
    }
    JsonValue::Object out;
    out["cmd"] = JsonValue(static_cast<int64_t>(kCmdGetDp));
    out["data"] = JsonValue(std::move(rows));
    publish_json(topic_out_, JsonValue(std::move(out)));
}

void Module::handle_get_dp_all() {
    JsonValue::Array rows;
    {
        std::lock_guard<std::mutex> lk(dp_mu_);
        rows.reserve(dp_values_.size());
        for (const auto& [dp_id, value] : dp_values_) {
            JsonValue::Object row;
            row["dp"] = JsonValue(static_cast<int64_t>(dp_id));
            row["value"] = value;
            rows.emplace_back(JsonValue(std::move(row)));
        }
    }
    JsonValue::Object out;
    out["cmd"] = JsonValue(static_cast<int64_t>(kCmdGetDp));
    out["data"] = JsonValue(std::move(rows));
    publish_json(topic_out_, JsonValue(std::move(out)));
}

void Module::handle_disconnect_server() {
    if (mqtt_suspended_) {
        SC_LOG_INFO("Soullink disconnectServer ignored: MQTT already suspended");
        return;
    }
    publish_notification("Disconnected from host broker by command", "warning");
    mqtt_suspended_ = true;
    mqtt_suspend_requested_ = true;
    SC_LOG_INFO("Soullink disconnectServer accepted; suspend queued");
}

static int extract_stream_index(const JsonValue& data, int fallback) {
    if (data.is_number()) return data.as_int(fallback);
    if (data.is_object()) {
        if (const auto* idx = data.get("index"); idx && idx->is_number()) return idx->as_int(fallback);
        if (const auto* idx = data.get("streamIndex"); idx && idx->is_number()) return idx->as_int(fallback);
    }
    return fallback;
}

void Module::handle_sub_stream(const JsonValue& data) {
    int stream_index = extract_stream_index(data, active_stream_index_);
    if (stream_index >= 0) active_stream_index_ = stream_index;
    bool was = stream_subscribed_.exchange(true);
    if (!was) publish_stream_state(0, 1, stream_index);
}

void Module::handle_unsub_stream(const JsonValue& data) {
    int stream_index = extract_stream_index(data, active_stream_index_);
    if (stream_index >= 0) active_stream_index_ = stream_index;
    bool was = stream_subscribed_.exchange(false);
    if (was) publish_stream_state(1, 0, stream_index);
}

void Module::handle_streaming(const JsonValue& data) {
    int stream_index = extract_stream_index(data, active_stream_index_);
    if (stream_index >= 0) active_stream_index_ = stream_index;
    publish_stream_state(stream_subscribed_ ? 1 : 0, stream_subscribed_ ? 1 : 0, stream_index);
}

void Module::handle_sync_files(const JsonValue& data) {
    publish_sync_progress(0);
    auto result = sync_engine_.execute(
        data,
        [this](const std::string& stage, const std::string& /*details*/) {
            if (stage.find("fetch") != std::string::npos) publish_sync_progress(20);
            else if (stage.find("download") != std::string::npos) publish_sync_progress(60);
            else if (stage.find("apply") != std::string::npos) publish_sync_progress(90);
        });

    publish_sync_progress(100);
    publish_sync_result(result.ok, result.message);
}

void Module::publish_unsupported(const JsonValue& cmd_value) {
    JsonValue::Object data;
    data["cmd"] = cmd_value;
    JsonValue::Object message;
    message["text"] = JsonValue("Unsupported command");
    message["type"] = JsonValue("warning");
    message["data"] = JsonValue(std::move(data));
    JsonValue::Object payload;
    payload["id"] = JsonValue(static_cast<int64_t>(0));
    payload["message"] = JsonValue(std::move(message));
    publish_json(topic_msg_, JsonValue(std::move(payload)));
}

void Module::publish_notification(const std::string& text, const std::string& type) {
    JsonValue::Object message;
    message["text"] = JsonValue(text);
    message["type"] = JsonValue(type);
    JsonValue::Object payload;
    payload["id"] = JsonValue(static_cast<int64_t>(0));
    payload["message"] = JsonValue(std::move(message));
    publish_json(topic_msg_, JsonValue(std::move(payload)));
}

void Module::publish_sync_progress(int progress) {
    JsonValue::Object message;
    message["progress"] = JsonValue(static_cast<double>(progress));
    JsonValue::Object payload;
    payload["id"] = JsonValue(static_cast<int64_t>(1));
    payload["message"] = JsonValue(std::move(message));
    publish_json(topic_msg_, JsonValue(std::move(payload)));
}

void Module::publish_sync_result(bool success, const std::string& details) {
    JsonValue::Object message;
    message["success"] = JsonValue(success);
    message["detail"] = JsonValue(details);
    JsonValue::Object payload;
    payload["id"] = JsonValue(static_cast<int64_t>(1));
    payload["message"] = JsonValue(std::move(message));
    publish_json(topic_msg_, JsonValue(std::move(payload)));
}

bool Module::publish_json(const std::string& topic, const JsonValue& payload) {
    if (!mqtt_online_ || !mosq_) return false;
    const std::string data = json_dump(payload);
    int rc = mosquitto_publish(
        mosq_, nullptr, topic.c_str(), static_cast<int>(data.size()), data.data(), 0, false);
    return rc == MOSQ_ERR_SUCCESS;
}

bool Module::publish_binary(const std::string& topic, const std::vector<uint8_t>& payload) {
    if (!mqtt_online_ || !mosq_) return false;
    int rc = mosquitto_publish(
        mosq_, nullptr, topic.c_str(), static_cast<int>(payload.size()), payload.data(), 0, false);
    return rc == MOSQ_ERR_SUCCESS;
}

bool Module::start_control_api() {
    if (control_api_thread_.joinable()) return true;
    control_api_thread_ = std::thread(&Module::control_api_loop, this);
    return true;
}

void Module::stop_control_api() {
    if (control_api_listen_fd_ >= 0) {
        close(control_api_listen_fd_);
        control_api_listen_fd_ = -1;
    }
    if (control_api_thread_.joinable()) {
        control_api_thread_.join();
    }
}

void Module::control_api_loop() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        SC_LOG_WARN("Soullink API: socket failed");
        return;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(sl_cfg_.api_port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(fd, 4) != 0) {
        SC_LOG_WARN("Soullink API: bind/listen failed on %d", sl_cfg_.api_port);
        close(fd);
        return;
    }
    control_api_listen_fd_ = fd;
    SC_LOG_INFO("Soullink API listening on :%d", sl_cfg_.api_port);

    auto send_text = [](int conn, int code, const char* body) {
        const char* status = code == 200 ? "200 OK" : "400 Bad Request";
        std::ostringstream resp;
        resp << "HTTP/1.1 " << status << "\r\n"
             << "Content-Type: text/plain\r\n"
             << "Connection: close\r\n"
             << "Content-Length: " << strlen(body) << "\r\n\r\n"
             << body;
        std::string s = resp.str();
        send(conn, s.data(), s.size(), 0);
    };

    while (running_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{1, 0};
        int sel = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in peer_addr{};
        socklen_t peer_len = sizeof(peer_addr);
        int conn = accept(fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (conn < 0) continue;

        char req[2048] = {0};
        ssize_t n = recv(conn, req, sizeof(req) - 1, 0);
        if (n <= 0) {
            close(conn);
            continue;
        }
        std::string request(req, static_cast<size_t>(n));
        std::string first_line = request.substr(0, request.find("\r\n"));

        // Expected:
        //   GET /debug HTTP/1.1
        //   GET /debug?ip=<host-ip> HTTP/1.1
        // If ip is omitted, use requester source IP.
        if (first_line.rfind("GET /debug", 0) == 0) {
            std::string host;
            size_t q = first_line.find('?');
            size_t sp = first_line.find(' ');
            std::string qs = (q != std::string::npos && sp != std::string::npos && q < sp)
                ? first_line.substr(q + 1, sp - q - 1)
                : "";
            std::stringstream qss(qs);
            std::string kv;
            while (std::getline(qss, kv, '&')) {
                if (kv.rfind("ip=", 0) == 0) {
                    host = kv.substr(3);
                    break;
                }
            }
            host = trim_copy(host);
            if (host.empty()) {
                char ipbuf[INET_ADDRSTRLEN] = {0};
                if (inet_ntop(AF_INET, &peer_addr.sin_addr, ipbuf, sizeof(ipbuf))) {
                    host = ipbuf;
                }
            }
            if (!host.empty()) {
                reconnect_mqtt_to_host(host);
                send_text(conn, 200, "ok");
            } else {
                send_text(conn, 400, "invalid");
            }
        } else {
            send_text(conn, 400, "invalid");
        }
        close(conn);
    }
}

bool Module::start_frame_receiver() {
    if (frame_receiver_started_) return true;
    frame_receiver_started_ = true;
    frame_receiver_thread_ = std::thread(&Module::frame_receiver_loop, this);
    return true;
}

void Module::stop_frame_receiver() {
    frame_receiver_started_ = false;
    if (frame_receiver_thread_.joinable()) frame_receiver_thread_.join();
}

void Module::frame_receiver_loop() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        SC_LOG_WARN("Soullink TCP receiver: socket() failed");
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(sl_cfg_.stream_tcp_port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0) {
        close(listen_fd);
        SC_LOG_WARN("Soullink TCP receiver: bind/listen failed on %d", sl_cfg_.stream_tcp_port);
        return;
    }
    SC_LOG_INFO("Soullink TCP receiver listening on %d", sl_cfg_.stream_tcp_port);

    std::vector<uint8_t> buffer;
    buffer.reserve(1024 * 1024);

    while (running_ && frame_receiver_started_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        timeval tv{1, 0};
        int sel = select(listen_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) continue;

        while (running_ && frame_receiver_started_) {
            uint8_t chunk[65536];
            ssize_t n = recv(conn_fd, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buffer.insert(buffer.end(), chunk, chunk + n);

            while (buffer.size() >= 4) {
                uint32_t frame_size = 0;
                std::memcpy(&frame_size, buffer.data(), 4);
                if (frame_size == 0 || frame_size > static_cast<uint32_t>(sl_cfg_.max_frame_bytes)) {
                    SC_LOG_WARN("Soullink TCP receiver: invalid frame size=%u", frame_size);
                    buffer.clear();
                    break;
                }
                size_t total = 4u + frame_size;
                if (buffer.size() < total) break;

                std::vector<uint8_t> frame(buffer.begin() + 4, buffer.begin() + total);
                buffer.erase(buffer.begin(), buffer.begin() + total);

                std::string tmp = sl_cfg_.frame_sink_path + ".tmp";
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (out.good()) {
                    out.write(reinterpret_cast<const char*>(frame.data()),
                              static_cast<std::streamsize>(frame.size()));
                    out.close();
                    rename(tmp.c_str(), sl_cfg_.frame_sink_path.c_str());
                }
            }
        }
        close(conn_fd);
    }
    close(listen_fd);
}

bool Module::check_rtsp_reachable() const {
    std::string host = sl_cfg_.rtsp_host_override.empty() ? local_ip_ : sl_cfg_.rtsp_host_override;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    timeval tv{0, 800000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.rtsp.port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return false;
    }
    bool ok = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    close(fd);
    return ok;
}

void Module::publish_health() {
    bool rtsp_ok = check_rtsp_reachable();
    bool ready = rtsp_ok && frame_receiver_started_;

    JsonValue::Array data;
    JsonValue::Object dp_rtsp;
    dp_rtsp["dp"] = JsonValue(static_cast<int64_t>(1001));
    dp_rtsp["value"] = JsonValue(rtsp_ok ? 1.0 : 0.0);
    data.emplace_back(JsonValue(std::move(dp_rtsp)));

    JsonValue::Object dp_stream;
    dp_stream["dp"] = JsonValue(static_cast<int64_t>(1002));
    dp_stream["value"] = JsonValue(stream_subscribed_ ? 1.0 : 0.0);
    data.emplace_back(JsonValue(std::move(dp_stream)));

    JsonValue::Object dp_ready;
    dp_ready["dp"] = JsonValue(static_cast<int64_t>(1003));
    dp_ready["value"] = JsonValue(ready ? 1.0 : 0.0);
    data.emplace_back(JsonValue(std::move(dp_ready)));

    JsonValue::Object payload;
    payload["cmd"] = JsonValue(static_cast<int64_t>(kCmdGetDp));
    payload["data"] = JsonValue(std::move(data));
    publish_json(topic_out_, JsonValue(std::move(payload)));
}

std::string Module::make_stream_topic(int stream_index) const {
    if (stream_index < 0) stream_index = active_stream_index_;
    std::string prefix = normalize_topic_prefix(sl_cfg_.mqtt_topic_prefix);
    return prefix + "s/" + client_id_ + "/" + std::to_string(stream_index);
}

void Module::publish_stream_state(uint8_t old_status, uint8_t new_status, int stream_index) {
    std::vector<uint8_t> rec(8, 0);
    uint16_t uid = 0;
    uint32_t ts = static_cast<uint32_t>(time(nullptr));
    std::memcpy(rec.data(), &uid, sizeof(uid));
    rec[2] = old_status;
    rec[3] = new_status;
    std::memcpy(rec.data() + 4, &ts, sizeof(ts));
    publish_binary(make_stream_topic(stream_index), rec);
}

std::string Module::normalize_topic_prefix(const std::string& topic_prefix) {
    if (topic_prefix.empty()) return "soulcam/debug/";
    return topic_prefix.back() == '/' ? topic_prefix : (topic_prefix + "/");
}

std::string Module::normalize_mdns_type(const std::string& service_type) {
    std::string out = service_type;
    if (out.size() >= 6 && out.substr(out.size() - 6) == ".local") {
        out = out.substr(0, out.size() - 6);
    } else if (out.size() >= 7 && out.substr(out.size() - 7) == ".local.") {
        out = out.substr(0, out.size() - 7);
    }
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

std::string Module::detect_local_ip() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "127.0.0.1";
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));

    sockaddr_in src{};
    socklen_t len = sizeof(src);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&src), &len) != 0) {
        close(fd);
        return "127.0.0.1";
    }
    char buf[64];
    const char* p = inet_ntop(AF_INET, &src.sin_addr, buf, sizeof(buf));
    close(fd);
    return p ? std::string(p) : "127.0.0.1";
}

std::string Module::derive_service_identifier(const std::string& override_value) {
    if (!override_value.empty()) return override_value;

    std::string machine_id;
    {
        std::ifstream in("/etc/machine-id");
        if (in.good()) std::getline(in, machine_id);
    }
    if (machine_id.empty()) {
        std::ifstream in("/var/lib/dbus/machine-id");
        if (in.good()) std::getline(in, machine_id);
    }
    if (machine_id.empty()) machine_id = "unknown";

    char host[128] = {};
    gethostname(host, sizeof(host) - 1);
    std::string hostname(host);
    for (char& c : hostname) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '-';
    }

    std::hash<std::string> hasher;
    size_t digest = hasher(machine_id + ":" + hostname);
    std::ostringstream out;
    out << hostname << "-" << std::hex << (digest & 0xffffffffu);
    std::string id = out.str();
    if (id.size() > 48) id.resize(48);
    return id;
}

bool Module::command_exists(const char* name) {
    const char* path = getenv("PATH");
    if (!path || !name || !*name) return false;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        std::string full = dir + "/" + name;
        if (access(full.c_str(), X_OK) == 0) return true;
    }
    return false;
}

}  // namespace sc::soullink

