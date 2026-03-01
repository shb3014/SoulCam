// ============================================================================
// SoulCam -- IP Camera with AI (RK3566)
//
// Main entry point.  Configures the ISP, starts RTSP streaming on mainpath,
// and optionally starts AI inference on selfpath.
//
// Architecture:
//
//   ┌──────────────────────────────────────────────┐
//   │            ISP (rkisp v21)                   │
//   │  OV5647 -> CSI -> ISP Processing (3A, CCM)   │
//   ├────────────────┬─────────────────────────────┤
//   │  mainpath      │  selfpath                   │
//   │  /dev/video8   │  /dev/video9                │
//   │  1280x960 NV12 │  640x480 NV12               │
//   └────────┬───────┴────────────┬────────────────┘
//            │                    │
//            ▼                    ▼
//   ┌────────────────┐   ┌─────────────────┐
//   │ MPP (HW)       │   │ RGA (HW)        │
//   │ NV12 -> H.264  │   │ NV12 -> RGB     │
//   └────────┬───────┘   └────────┬────────┘
//            │                    │
//            ▼                    ▼
//   ┌────────────────┐   ┌─────────────────┐
//   │ RTSP Server    │   │ RKNN NPU        │
//   │ :8554/cam      │   │ YOLOv8 infer    │
//   └────────────────┘   └────────┬────────┘
//                                 │
//                                 ▼
//                        ┌─────────────────┐
//                        │ Scene Hub       │
//                        │ (detections)    │
//                        └─────────────────┘
//
// Usage:
//   soulcam                          # RTSP only (default)
//   soulcam --ai --model yolov8n.rknn  # RTSP + AI
//   soulcam -v                       # verbose logging
//
// Test:
//   ffplay rtsp://<device-ip>:8554/cam
//   ffmpeg -rtsp_transport tcp -i rtsp://<device-ip>:8554/cam -t 10 -f null -
// ============================================================================

#include "soulcam.h"
#include "pipeline/isp_config.h"
#include "pipeline/rtsp_server.h"
#include "pipeline/ai_capture.h"
#include "ai/model_pipeline.h"         // Multi-model pipeline info
#include "pipeline/overlay.h"          // overlay_update()
#include "pipeline/onvif_metadata.h"   // ONVIF metadata stream
#include "pipeline/onvif_device.h"     // ONVIF device service (WS-Discovery + SOAP)
#include "pipeline/snapshot.h"         // JPEG snapshot endpoint
#include "pipeline/tuya_ipc.h"         // Tuya IPC SDK adapter
#include "util/logger.h"

#include <gst/gst.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <unistd.h>
#include <atomic>
#include <sstream>
#include <iomanip>
#include <sys/socket.h>
#include <sys/un.h>

// ---------------------------------------------------------------------------
// Global shutdown flag
// ---------------------------------------------------------------------------
static std::atomic<bool> g_shutdown{false};

static void signal_handler(int sig) {
    (void)sig;
    SC_LOG_INFO("Signal %d received -- shutting down", sig);
    g_shutdown = true;
}

// ---------------------------------------------------------------------------
// CLI argument parsing
// ---------------------------------------------------------------------------

static void print_usage(const char* prog) {
    fprintf(stderr,
        "SoulCam -- IP Camera with AI (RK3566)\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Stream options:\n"
        "  --width W          Stream width       (default: 1280)\n"
        "  --height H         Stream height      (default: 960)\n"
        "  --fps F            Framerate           (default: 30)\n"
        "  --bitrate K        H.264 bitrate kbps  (default: 4000)\n"
        "  --port P           RTSP port           (default: 8554)\n"
        "  --mount M          RTSP mount point    (default: /cam)\n"
        "  --encoder E        mpp|x264            (default: mpp)\n"
        "\n"
        "Sensor options:\n"
        "  --sensor-width W   Sensor mode width   (default: 1296)\n"
        "  --sensor-height H  Sensor mode height  (default: 972)\n"
        "\n"
        "AI options:\n"
        "  --ai               Enable AI pipeline on selfpath\n"
        "  --overlay          Draw detection boxes on RTSP stream (requires --ai)\n"
        "  --onvif            Enable ONVIF metadata stream on port+1 (requires --ai)\n"
        "\n"
        "ONVIF device service:\n"
        "  --onvif-device     Enable ONVIF device service (WS-Discovery + SOAP)\n"
        "  --onvif-port P     ONVIF HTTP port (default: 8080)\n"
        "\n"
        "Snapshot:\n"
        "  --snapshot         Enable JPEG snapshot HTTP server\n"
        "  --snapshot-port P  Snapshot HTTP port (default: 8088)\n"
        "\n"
        "Performance options:\n"
        "  --dmabuf           Use DMA-BUF io-mode for zero-copy ISP→RGA→MPP\n"
        "  --model PATH       Primary RKNN model (slot 0)\n"
        "  --ai-width W       AI capture width    (default: 640)\n"
        "  --ai-height H      AI capture height   (default: 480)\n"
        "  --ai-fps F         AI capture FPS      (default: 30)\n"
        "  --conf-thresh F    Detection confidence (default: 0.25)\n"
        "  --nms-thresh F     NMS threshold       (default: 0.45)\n"
        "  --labels L         Comma-separated class labels (e.g. \"hand\")\n"
        "\n"
        "Multi-model pipeline:\n"
        "  --model2 PATH      Second model (slot 1)\n"
        "  --model2-skip N    Run model 2 every N+1 frames (default: 0)\n"
        "  --model2-conf F    Model 2 confidence threshold\n"
        "  --model3 PATH      Third model (slot 2)\n"
        "  --model3-skip N    Run model 3 every N+1 frames (default: 0)\n"
        "  --model3-conf F    Model 3 confidence threshold\n"
        "\n"
        "Device options:\n"
        "  --mainpath DEV     Mainpath device     (default: /dev/video8)\n"
        "  --selfpath DEV     Selfpath device     (default: /dev/video9)\n"
        "  --media DEV        Media device        (default: /dev/media1)\n"
        "\n"
        "Runtime control:\n"
        "  --ctrl-sock PATH   Control socket      (default: /tmp/soulcam_ctrl.sock)\n"
        "\n"
        "General:\n"
        "  -v, --verbose      Verbose logging\n"
        "  -h, --help         Show this help\n"
        "\n"
        "Model management (at runtime):\n"
        "  echo '{\"cmd\":\"swap_model\",\"path\":\"/path/to/model.rknn\"}' | \\\n"
        "      socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock\n"
        "  echo '{\"cmd\":\"swap_model\",\"slot\":1,\"path\":\"model.rknn\"}' | \\\n"
        "      socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock\n"
        "  echo '{\"cmd\":\"add_model\",\"path\":\"model.rknn\",\"skip\":2}' | \\\n"
        "      socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock\n"
        "  echo '{\"cmd\":\"remove_model\",\"slot\":1}' | \\\n"
        "      socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock\n"
        "  echo '{\"cmd\":\"list_models\"}' | \\\n"
        "      socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock\n"
        "  echo '{\"cmd\":\"enable_model\",\"slot\":1,\"enable\":false}' | \\\n"
        "      socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock\n"
        "\n", prog);
}

static sc::Config parse_args(int argc, char** argv) {
    sc::Config cfg;

    // Temp holders for multi-model CLI args
    std::string model2_path, model3_path;
    int model2_skip = 0, model3_skip = 0;
    float model2_conf = 0.25f, model3_conf = 0.25f;

    static struct option long_opts[] = {
        {"width",         required_argument, nullptr, 'W'},
        {"height",        required_argument, nullptr, 'H'},
        {"fps",           required_argument, nullptr, 'f'},
        {"bitrate",       required_argument, nullptr, 'b'},
        {"port",          required_argument, nullptr, 'p'},
        {"mount",         required_argument, nullptr, 'm'},
        {"encoder",       required_argument, nullptr, 'e'},
        {"sensor-width",  required_argument, nullptr, 'S'},
        {"sensor-height", required_argument, nullptr, 'T'},
        {"ai",            no_argument,       nullptr, 'A'},
        {"overlay",       no_argument,       nullptr, 'O'},
        {"onvif",         no_argument,       nullptr, 'N'},
        {"onvif-device",  no_argument,       nullptr, 'Q'},
        {"onvif-port",    required_argument, nullptr, 'R'},
        {"snapshot",      no_argument,       nullptr, 'J'},
        {"snapshot-port", required_argument, nullptr, 'K'},
        {"dmabuf",        no_argument,       nullptr, 'D'},
        {"model",         required_argument, nullptr, 'M'},
        {"ai-width",      required_argument, nullptr, 'w'},
        {"ai-height",     required_argument, nullptr, 'x'},
        {"ai-fps",        required_argument, nullptr, 'F'},
        {"conf-thresh",   required_argument, nullptr, 'c'},
        {"nms-thresh",    required_argument, nullptr, 'n'},
        {"labels",        required_argument, nullptr, 'L'},
        {"mainpath",      required_argument, nullptr, 1},
        {"selfpath",      required_argument, nullptr, 2},
        {"media",         required_argument, nullptr, 3},
        {"scene-sock",    required_argument, nullptr, 4},
        {"ctrl-sock",     required_argument, nullptr, 5},
        // Multi-model options
        {"model2",        required_argument, nullptr, 10},
        {"model2-skip",   required_argument, nullptr, 11},
        {"model2-conf",   required_argument, nullptr, 12},
        {"model3",        required_argument, nullptr, 13},
        {"model3-skip",   required_argument, nullptr, 14},
        {"model3-conf",   required_argument, nullptr, 15},
        {"verbose",       no_argument,       nullptr, 'v'},
        {"help",          no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "vhf:b:p:m:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'W': cfg.stream.width       = atoi(optarg); break;
            case 'H': cfg.stream.height      = atoi(optarg); break;
            case 'f': cfg.stream.fps         = atoi(optarg); break;
            case 'b': cfg.rtsp.bitrate_kbps  = atoi(optarg); break;
            case 'p': cfg.rtsp.port          = atoi(optarg); break;
            case 'm': cfg.rtsp.mount         = optarg;       break;
            case 'e': cfg.rtsp.encoder       = optarg;       break;
            case 'S': cfg.sensor.width       = atoi(optarg); break;
            case 'T': cfg.sensor.height      = atoi(optarg); break;
            case 'A': cfg.enable_ai          = true;         break;
            case 'O': cfg.enable_overlay    = true;         break;
            case 'N': cfg.enable_onvif     = true;         break;
            case 'Q': cfg.enable_onvif_device = true;       break;
            case 'R': cfg.onvif_port       = atoi(optarg);  break;
            case 'J': cfg.enable_snapshot   = true;         break;
            case 'K': cfg.snapshot_port     = atoi(optarg);  break;
            case 'D': cfg.use_dmabuf        = true;         break;
            case 'M': cfg.rknn.model_path    = optarg;       break;
            case 'w': cfg.ai.width           = atoi(optarg); break;
            case 'x': cfg.ai.height          = atoi(optarg); break;
            case 'F': cfg.ai.fps             = atoi(optarg); break;
            case 'c': cfg.rknn.conf_threshold = atof(optarg); break;
            case 'n': cfg.rknn.nms_threshold  = atof(optarg); break;
            case 'L': cfg.rknn.labels         = optarg;       break;
            case 1:   cfg.isp.mainpath       = optarg;       break;
            case 2:   cfg.isp.selfpath       = optarg;       break;
            case 3:   cfg.isp.media_dev      = optarg;       break;
            case 4:   cfg.scene_sock         = optarg;       break;
            case 5:   cfg.ctrl_sock          = optarg;       break;
            // Multi-model
            case 10:  model2_path = optarg;                  break;
            case 11:  model2_skip = atoi(optarg);            break;
            case 12:  model2_conf = static_cast<float>(atof(optarg)); break;
            case 13:  model3_path = optarg;                  break;
            case 14:  model3_skip = atoi(optarg);            break;
            case 15:  model3_conf = static_cast<float>(atof(optarg)); break;
            case 'v': cfg.verbose            = true;         break;
            case 'h': print_usage(argv[0]); exit(0);
            default:  print_usage(argv[0]); exit(1);
        }
    }

    // Sync GOP with FPS (1 keyframe per second)
    cfg.rtsp.gop = cfg.stream.fps;

    // Build extra model slots from CLI args
    if (!model2_path.empty()) {
        sc::ModelSlotConfig slot;
        slot.rknn.model_path     = model2_path;
        slot.rknn.conf_threshold = model2_conf;
        slot.rknn.nms_threshold  = cfg.rknn.nms_threshold;  // inherit from primary
        slot.skip_frames         = model2_skip;
        cfg.extra_models.push_back(std::move(slot));
    }
    if (!model3_path.empty()) {
        sc::ModelSlotConfig slot;
        slot.rknn.model_path     = model3_path;
        slot.rknn.conf_threshold = model3_conf;
        slot.rknn.nms_threshold  = cfg.rknn.nms_threshold;
        slot.skip_frames         = model3_skip;
        cfg.extra_models.push_back(std::move(slot));
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// Scene hub socket (publish detection JSON over Unix datagram)
// ---------------------------------------------------------------------------
static int         g_scene_fd = -1;
static sockaddr_un g_scene_addr{};

static void scene_hub_init(const std::string& sock_path) {
    if (sock_path.empty()) return;
    g_scene_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (g_scene_fd < 0) {
        SC_LOG_WARN("Failed to create scene hub socket");
        return;
    }
    memset(&g_scene_addr, 0, sizeof(g_scene_addr));
    g_scene_addr.sun_family = AF_UNIX;
    strncpy(g_scene_addr.sun_path, sock_path.c_str(),
            sizeof(g_scene_addr.sun_path) - 1);
    SC_LOG_INFO("Scene hub target: %s", sock_path.c_str());
}

static void scene_hub_close() {
    if (g_scene_fd >= 0) { close(g_scene_fd); g_scene_fd = -1; }
}

// ---------------------------------------------------------------------------
// ONVIF metadata stream
// ---------------------------------------------------------------------------
static sc::OnvifStream* g_onvif = nullptr;

// ---------------------------------------------------------------------------
// AI detection callback -- logs + publishes to scene hub + ONVIF
// ---------------------------------------------------------------------------

static void on_detections(const std::vector<sc::Detection>& dets,
                           int frame_w, int frame_h) {
    SC_LOG_DEBUG("Detections: %zu in %dx%d", dets.size(), frame_w, frame_h);
    for (const auto& d : dets) {
        SC_LOG_DEBUG("  [%s] %.2f @ (%d,%d)-(%d,%d)",
                     d.label ? d.label : "?", d.confidence,
                     d.left, d.top, d.right, d.bottom);
    }

    // Update shared overlay state (thread-safe)
    sc::overlay_update(dets, frame_w, frame_h);

    // Push to ONVIF metadata stream
    if (g_onvif) {
        sc::onvif_stream_push(g_onvif, dets, frame_w, frame_h);
    }

    // Publish JSON to scene hub (extended format with model_id)
    if (g_scene_fd < 0) return;

    std::ostringstream msg;
    msg << "{\"source\":\"soulcam\",\"type\":\"detections\",\"count\":"
        << dets.size() << ",\"objects\":[";
    bool first = true;
    for (const auto& d : dets) {
        if (!first) msg << ",";
        first = false;
        msg << "{\"model\":" << d.model_id
            << ",\"cls_id\":" << d.cls_id
            << ",\"label\":\"" << (d.label ? d.label : "?") << "\""
            << ",\"conf\":" << std::fixed << std::setprecision(3) << d.confidence
            << ",\"box\":{\"left\":" << d.left
            << ",\"top\":" << d.top
            << ",\"right\":" << d.right
            << ",\"bottom\":" << d.bottom
            << "}}";
    }
    msg << "]}";

    std::string s = msg.str();
    sendto(g_scene_fd, s.c_str(), s.size(), MSG_DONTWAIT,
           reinterpret_cast<const sockaddr*>(&g_scene_addr), sizeof(g_scene_addr));
}

// ---------------------------------------------------------------------------
// Control socket -- accepts JSON commands at runtime
//
// Commands:
//   {"cmd":"swap_model","path":"/path/to/model.rknn"}
//   {"cmd":"swap_model","path":"/path/to/model.rknn","conf":0.3,"nms":0.5}
//   {"cmd":"status"}
//   {"cmd":"ping"}
//
// Default socket: /tmp/soulcam_ctrl.sock
// Usage:
//   echo '{"cmd":"swap_model","path":"rk3566/yolov8s.rknn"}' | \
//       socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock
// ---------------------------------------------------------------------------

static int         g_ctrl_fd = -1;
static std::string g_ctrl_path;

static void ctrl_init(const std::string& sock_path) {
    if (sock_path.empty()) return;

    g_ctrl_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (g_ctrl_fd < 0) {
        SC_LOG_WARN("Control socket: failed to create");
        return;
    }

    // Bind to the path (remove stale socket first)
    unlink(sock_path.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(g_ctrl_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        SC_LOG_WARN("Control socket: failed to bind %s", sock_path.c_str());
        close(g_ctrl_fd);
        g_ctrl_fd = -1;
        return;
    }

    g_ctrl_path = sock_path;
    SC_LOG_INFO("Control socket: listening on %s", sock_path.c_str());
}

static void ctrl_close() {
    if (g_ctrl_fd >= 0) {
        close(g_ctrl_fd);
        g_ctrl_fd = -1;
    }
    if (!g_ctrl_path.empty()) {
        unlink(g_ctrl_path.c_str());
        g_ctrl_path.clear();
    }
}

// Simple JSON field extraction (no dependency on a JSON library)
static std::string json_str(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return json.substr(pos + 1, end - pos - 1);
}

static float json_float(const std::string& json, const std::string& key, float def) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    try { return std::stof(json.substr(pos)); }
    catch (...) { return def; }
}

static int json_int(const std::string& json, const std::string& key, int def) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    try { return std::stoi(json.substr(pos)); }
    catch (...) { return def; }
}

static bool json_bool(const std::string& json, const std::string& key, bool def) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos < json.size() && json[pos] == 't') return true;
    if (pos < json.size() && json[pos] == 'f') return false;
    return def;
}

static void ctrl_poll(sc::AiCapture* ai, sc::Config& cfg) {
    if (g_ctrl_fd < 0) return;

    char buf[2048];
    ssize_t n = recv(g_ctrl_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;  // EAGAIN or error
    buf[n] = '\0';

    std::string msg(buf, n);
    std::string cmd = json_str(msg, "cmd");

    if (cmd == "swap_model") {
        // Swap model in a specific slot (default: slot 0 for backwards compat)
        std::string path = json_str(msg, "path");
        if (path.empty()) {
            SC_LOG_WARN("Control: swap_model missing 'path'");
            return;
        }
        int slot  = json_int(msg, "slot", 0);
        float conf = json_float(msg, "conf", cfg.rknn.conf_threshold);
        float nms  = json_float(msg, "nms",  cfg.rknn.nms_threshold);

        sc::RknnConfig new_cfg;
        new_cfg.model_path     = path;
        new_cfg.conf_threshold = conf;
        new_cfg.nms_threshold  = nms;

        if (ai) {
            int rc = sc::ai_capture_swap_model(ai, new_cfg, slot);
            if (rc == 0) {
                if (slot == 0) {
                    cfg.rknn.model_path     = path;
                    cfg.rknn.conf_threshold = conf;
                    cfg.rknn.nms_threshold  = nms;
                }
                SC_LOG_INFO("Control: slot %d swapped to %s (conf=%.2f, nms=%.2f)",
                            slot, path.c_str(), conf, nms);
            }
        } else {
            SC_LOG_WARN("Control: swap_model but AI pipeline is not running");
        }
    } else if (cmd == "add_model") {
        // Add a new model slot at runtime
        std::string path = json_str(msg, "path");
        if (path.empty()) {
            SC_LOG_WARN("Control: add_model missing 'path'");
            return;
        }
        if (!ai) {
            SC_LOG_WARN("Control: add_model but AI pipeline is not running");
            return;
        }
        sc::ModelSlotConfig slot_cfg;
        slot_cfg.rknn.model_path     = path;
        slot_cfg.rknn.conf_threshold = json_float(msg, "conf", cfg.rknn.conf_threshold);
        slot_cfg.rknn.nms_threshold  = json_float(msg, "nms", cfg.rknn.nms_threshold);
        slot_cfg.skip_frames         = json_int(msg, "skip", 0);
        slot_cfg.name                = json_str(msg, "name");

        int idx = sc::ai_capture_add_model(ai, slot_cfg);
        if (idx >= 0) {
            SC_LOG_INFO("Control: added model slot %d (%s, skip=%d)",
                        idx, path.c_str(), slot_cfg.skip_frames);
        }
    } else if (cmd == "remove_model") {
        int slot = json_int(msg, "slot", -1);
        if (slot <= 0) {
            SC_LOG_WARN("Control: remove_model requires 'slot' > 0");
            return;
        }
        if (ai) {
            int rc = sc::ai_capture_remove_model(ai, slot);
            if (rc == 0) SC_LOG_INFO("Control: removed model slot %d", slot);
        }
    } else if (cmd == "enable_model") {
        int slot = json_int(msg, "slot", -1);
        bool enable = json_bool(msg, "enable", true);
        if (slot < 0) {
            SC_LOG_WARN("Control: enable_model requires 'slot'");
            return;
        }
        if (ai) {
            sc::ai_capture_enable_model(ai, slot, enable);
            SC_LOG_INFO("Control: slot %d %s", slot, enable ? "enabled" : "disabled");
        }
    } else if (cmd == "list_models") {
        if (!ai) {
            SC_LOG_INFO("Control: list_models -- AI pipeline not running");
            return;
        }
        int count = sc::ai_capture_model_count(ai);
        SC_LOG_INFO("Control: %d model slot(s):", count);
        for (int i = 0; i < count; i++) {
            auto info = sc::ai_capture_get_model_info(ai, i);
            SC_LOG_INFO("  slot %d: [%s] %s (conf=%.2f, skip=%d, %s)",
                        i, info.name.c_str(), info.rknn.model_path.c_str(),
                        info.rknn.conf_threshold, info.skip_frames,
                        info.enabled ? "enabled" : "disabled");
        }
    } else if (cmd == "status") {
        int model_count = ai ? sc::ai_capture_model_count(ai) : 0;
        SC_LOG_INFO("Control: status -- SoulCam running, %d model slot(s), primary=%s",
                    model_count, cfg.rknn.model_path.c_str());
    } else if (cmd == "ping") {
        SC_LOG_INFO("Control: pong");
    } else {
        SC_LOG_WARN("Control: unknown command '%s'", cmd.c_str());
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    sc::Config cfg = parse_args(argc, argv);

    if (cfg.verbose) {
        sc::log_set_level(sc::LogLevel::DEBUG);
    }

    SC_LOG_INFO("SoulCam v%s starting", "0.2.0");

    // --- Set GStreamer plugin path BEFORE gst_init() ---
    // rgaconvert lives in a custom path; GStreamer scans plugins at init time.
    const char* existing_pp = g_getenv("GST_PLUGIN_PATH");
    std::string plugin_path = SOULCAM_RGA_PLUGIN_PATH;
    if (existing_pp && existing_pp[0] != '\0') {
        plugin_path = plugin_path + ":" + existing_pp;
    }
    g_setenv("GST_PLUGIN_PATH", plugin_path.c_str(), TRUE);

    // --- Initialize GStreamer ---
    gst_init(&argc, &argv);

    // --- Signal handling ---
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    // --- Configure ISP ---
    // Dual-path mode if AI is enabled or snapshot needs selfpath
    int rc;
    if (cfg.enable_ai || cfg.enable_snapshot) {
        rc = sc::isp_configure(cfg);
    } else {
        rc = sc::isp_configure_mainpath(cfg);
    }
    if (rc != 0) {
        SC_LOG_ERROR("ISP configuration failed -- continuing anyway (may use existing config)");
    }

    // --- Start RTSP server ---
    SC_LOG_INFO("Starting RTSP server (HW-accelerated pipeline)...");
    auto* rtsp = sc::rtsp_server_start(cfg);
    if (!rtsp) {
        SC_LOG_ERROR("Failed to start RTSP server");
        return 1;
    }

    if (cfg.enable_overlay && !cfg.enable_ai) {
        SC_LOG_WARN("--overlay requires --ai; overlay disabled");
    }

    // --- Start AI pipeline (optional) ---
    sc::AiCapture* ai = nullptr;
    if (cfg.enable_ai) {
        // Init scene hub socket for detection output
        scene_hub_init(cfg.scene_sock);

        SC_LOG_INFO("Starting AI pipeline on selfpath...");
        ai = sc::ai_capture_start(cfg, on_detections);
        if (!ai) {
            SC_LOG_WARN("AI pipeline failed to start -- RTSP continues without AI");
        }
    }

    // --- ONVIF metadata stream (optional) ---
    if (cfg.enable_onvif && cfg.enable_ai && ai) {
        SC_LOG_INFO("Starting ONVIF metadata stream...");
        g_onvif = sc::onvif_stream_start(cfg);
        if (!g_onvif) {
            SC_LOG_WARN("ONVIF metadata stream failed to start -- detections still via scene hub");
        }
    } else if (cfg.enable_onvif && !cfg.enable_ai) {
        SC_LOG_WARN("--onvif requires --ai; ONVIF metadata disabled");
    }

    // --- ONVIF device service (WS-Discovery + SOAP) ---
    sc::OnvifDevice* onvif_dev = nullptr;
    if (cfg.enable_onvif_device) {
        SC_LOG_INFO("Starting ONVIF device service...");
        sc::OnvifDeviceConfig dev_cfg;
        dev_cfg.http_port = cfg.onvif_port;
        onvif_dev = sc::onvif_device_start(cfg, dev_cfg);
        if (!onvif_dev) {
            SC_LOG_WARN("ONVIF device service failed to start -- continuing without it");
        }
    }

    // --- JPEG snapshot server (optional) ---
    sc::SnapshotServer* snap_srv = nullptr;
    if (cfg.enable_snapshot) {
        SC_LOG_INFO("Starting snapshot HTTP server...");
        snap_srv = sc::snapshot_server_start(cfg, cfg.snapshot_port);
        if (!snap_srv) {
            SC_LOG_WARN("Snapshot server failed to start -- continuing without it");
        }
    }

    // --- Control socket ---
    ctrl_init(cfg.ctrl_sock);

    // --- Print summary ---
    fprintf(stderr, "\n");
    SC_LOG_INFO("=== SoulCam running ===");
    SC_LOG_INFO("  RTSP: rtsp://0.0.0.0:%d%s", cfg.rtsp.port, cfg.rtsp.mount.c_str());
    if (cfg.stream.src_fmt == "NV12") {
        SC_LOG_INFO("  Stream: %dx%d@%d NV12 -> MPP(H.264) -> RTSP",
                    cfg.stream.width, cfg.stream.height, cfg.stream.fps);
    } else {
        SC_LOG_INFO("  Stream: %dx%d@%d %s -> RGA(NV12) -> MPP(H.264) -> RTSP",
                    cfg.stream.width, cfg.stream.height, cfg.stream.fps,
                    cfg.stream.src_fmt.c_str());
    }
    SC_LOG_INFO("  I/O mode: %s", cfg.use_dmabuf ? "dmabuf (zero-copy)" : "userptr");
    if (ai) {
        int model_count = sc::ai_capture_model_count(ai);
        SC_LOG_INFO("  AI: selfpath %dx%d@%d %s -> %d model slot(s)",
                    cfg.ai.width, cfg.ai.height, cfg.ai.fps,
                    cfg.ai.src_fmt.c_str(), model_count);
        for (int i = 0; i < model_count; i++) {
            auto info = sc::ai_capture_get_model_info(ai, i);
            SC_LOG_INFO("    slot %d: [%s] %s (conf=%.2f, skip=%d)",
                        i, info.name.c_str(), info.rknn.model_path.c_str(),
                        info.rknn.conf_threshold, info.skip_frames);
        }
        if (cfg.enable_overlay) {
            SC_LOG_INFO("  Overlay: detection boxes drawn on RTSP stream");
        }
        if (g_onvif) {
            SC_LOG_INFO("  ONVIF: rtsp://0.0.0.0:%d/cam/meta (metadata stream)",
                        cfg.rtsp.port + 1);
        }
    }
    if (onvif_dev) {
        SC_LOG_INFO("  ONVIF Device: http://0.0.0.0:%d/onvif/device_service",
                    cfg.onvif_port);
        SC_LOG_INFO("  WS-Discovery: active (multicast 239.255.255.250:3702)");
    }
    if (snap_srv) {
        SC_LOG_INFO("  Snapshot: http://0.0.0.0:%d/snapshot (JPEG)",
                    cfg.snapshot_port);
    }
    if (g_ctrl_fd >= 0) {
        SC_LOG_INFO("  Control: %s (model hot-swap, status)", cfg.ctrl_sock.c_str());
    }
    SC_LOG_INFO("  Press Ctrl+C to stop");
    fprintf(stderr, "\n");

    // --- Main loop: poll control socket ---
    while (!g_shutdown) {
        ctrl_poll(ai, cfg);
        usleep(100000);  // 100ms polling interval
    }

    // --- Cleanup ---
    SC_LOG_INFO("Shutting down...");
    ctrl_close();
    sc::snapshot_server_stop(snap_srv);
    sc::onvif_device_stop(onvif_dev);
    sc::onvif_stream_stop(g_onvif);
    g_onvif = nullptr;
    sc::ai_capture_stop(ai);
    scene_hub_close();
    sc::rtsp_server_stop(rtsp);

    SC_LOG_INFO("SoulCam stopped");
    return 0;
}
