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
#include "store/store.h"
#include "pipeline/isp_config.h"
#include "pipeline/rtsp_server.h"
#include "pipeline/ai_capture.h"
#include "ai/model_pipeline.h"         // Multi-model pipeline info
#include "ai/hand_target_tracker.h"    // Single-target hand tracking policy
#include "pipeline/overlay.h"          // overlay_update()
#include "pipeline/rive_renderer.h"    // Rive GPU renderer
#include "pipeline/onvif_metadata.h"   // ONVIF metadata stream
#include "pipeline/onvif_device.h"     // ONVIF device service (WS-Discovery + SOAP)
#include "pipeline/snapshot.h"         // JPEG snapshot endpoint
#include "pipeline/tuya_ipc.h"         // Tuya IPC SDK adapter
#include "soullink/module.h"           // Native Soullink transport module
#include "util/logger.h"

#include <gst/gst.h>

#include <csignal>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <sstream>
#include <string_view>
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

static bool iequal_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static std::string trim_copy(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) begin++;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    return s.substr(begin, end - begin);
}

static bool should_enable_hand_target_tracker(const sc::Config& cfg) {
    if (cfg.rknn.labels.empty()) return false;
    std::stringstream ss(cfg.rknn.labels);
    std::string token;
    int non_empty_count = 0;
    bool has_hand = false;
    while (std::getline(ss, token, ',')) {
        token = trim_copy(token);
        if (token.empty()) continue;
        non_empty_count++;
        if (iequal_ascii(token, "hand")) has_hand = true;
    }
    return has_hand && (non_empty_count == 1);
}

static sc::HandTargetTrackerConfig make_hand_tracker_config(const sc::Config& cfg) {
    sc::HandTargetTrackerConfig tc;
    tc.enable_fast_approach_switch = cfg.hand_fast_switch;
    tc.fast_approach_min_growth = cfg.hand_fast_growth;
    tc.fast_approach_area_ratio = cfg.hand_fast_area_ratio;
    tc.fast_approach_hold_frames = std::max(1, cfg.hand_fast_hold_frames);
    return tc;
}

static sc::HandTargetTrackerConfig make_label_only_tracker_config(const std::string& label) {
    sc::HandTargetTrackerConfig tc;
    tc.preferred_label = label;
    // In test policy we pre-filter detections by model slot, so label fallback
    // should stay enabled to avoid dependence on model label strings.
    tc.fallback_to_all_labels = true;
    return tc;
}

struct AdaptiveHandPersonTestConfig {
    bool enabled = false;
    int hand_slot = 1;
    int person_slot = 0;
    int high_weight = 10;
    int low_weight = 1;
    int no_hand_frames_to_fallback = 8;
};

enum class AdaptiveTestMode {
    Neutral,
    HandPreferred,
    PersonPreferred,
};

static AdaptiveHandPersonTestConfig g_adaptive_test_cfg{};
static AdaptiveTestMode g_adaptive_test_mode = AdaptiveTestMode::Neutral;
static int g_adaptive_no_hand_frames = 0;
static sc::AiCapture* g_ai_for_policy = nullptr;
static sc::HandTargetTracker g_hand_policy_tracker(make_label_only_tracker_config("hand"));
static sc::HandTargetTracker g_person_policy_tracker(make_label_only_tracker_config("person"));

static std::vector<sc::Detection> dets_for_slot(const std::vector<sc::Detection>& dets,
                                                int slot_id) {
    std::vector<sc::Detection> out;
    if (slot_id < 0) return out;
    out.reserve(dets.size());
    for (const auto& d : dets) {
        if (d.model_id == slot_id) out.push_back(d);
    }
    return out;
}

static void apply_adaptive_test_weights_if_needed(AdaptiveTestMode mode) {
    if (!g_adaptive_test_cfg.enabled || !g_ai_for_policy) return;

    int hand_w = g_adaptive_test_cfg.low_weight;
    int person_w = g_adaptive_test_cfg.low_weight;
    bool hand_enable = true;
    bool person_enable = true;
    if (mode == AdaptiveTestMode::HandPreferred) {
        hand_w = g_adaptive_test_cfg.high_weight;
        // When hand is active, stop human inference to avoid periodic jump.
        person_enable = false;
    } else if (mode == AdaptiveTestMode::PersonPreferred) {
        person_w = g_adaptive_test_cfg.high_weight;
    } else {
        hand_w = 1;
        person_w = 1;
    }

    sc::ai_capture_enable_model(g_ai_for_policy, g_adaptive_test_cfg.hand_slot, hand_enable);
    sc::ai_capture_enable_model(g_ai_for_policy, g_adaptive_test_cfg.person_slot, person_enable);
    sc::ai_capture_set_model_weight(g_ai_for_policy, g_adaptive_test_cfg.hand_slot, hand_w);
    sc::ai_capture_set_model_weight(g_ai_for_policy, g_adaptive_test_cfg.person_slot, person_w);

    const char* mode_name = "neutral";
    if (mode == AdaptiveTestMode::HandPreferred) mode_name = "hand_preferred";
    if (mode == AdaptiveTestMode::PersonPreferred) mode_name = "person_preferred";
    SC_LOG_INFO("Adaptive test policy: mode=%s hand(slot=%d,en=%d,w=%d) person(slot=%d,en=%d,w=%d)",
                mode_name,
                g_adaptive_test_cfg.hand_slot, hand_enable ? 1 : 0, hand_w,
                g_adaptive_test_cfg.person_slot, person_enable ? 1 : 0, person_w);
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
        "\n"
        "Sensor options:\n"
        "  --sensor-width W   Sensor mode width   (default: 1296)\n"
        "  --sensor-height H  Sensor mode height  (default: 972)\n"
        "\n"
        "AI options:\n"
        "  --ai               Enable AI pipeline on selfpath\n"
        "  --overlay          Draw detection boxes on RTSP stream (requires --ai)\n"
        "  --onvif            Enable ONVIF (metadata stream + device service)\n"
        "  --onvif-port P     ONVIF HTTP port (default: 8080)\n"
        "  --snapshot         Enable JPEG snapshot HTTP server\n"
        "  --snapshot-port P  Snapshot HTTP port (default: 8088)\n"
        "\n"
        "Model options:\n"
        "  --model PATH       Primary RKNN model (slot 0)\n"
        "  --model-weight W   Primary model weighted-scheduler share (default: 1)\n"
        "  --conf-thresh F    Detection confidence (default: 0.25)\n"
        "  --nms-thresh F     NMS threshold       (default: 0.45)\n"
        "  --labels L         Comma-separated class labels (e.g. \"hand\")\n"
        "  --hand-fast-switch Enable advanced fast-approach target switching\n"
        "  --hand-fast-growth F   Min relative area growth per frame (default: 0.18)\n"
        "  --hand-fast-area-ratio F  Min challenger/current area ratio (default: 0.90)\n"
        "  --hand-fast-hold N   Consecutive fast-growth frames to switch (default: 3)\n"
        "\n"
        "Multi-model pipeline:\n"
        "  --model2 PATH      Second model (slot 1)\n"
        "  --model2-skip N    Run model 2 every N+1 frames (default: 0)\n"
        "  --model2-weight W  Model 2 weighted-scheduler share (default: 1)\n"
        "  --model2-conf F    Model 2 confidence threshold\n"
        "  --model3 PATH      Third model (slot 2)\n"
        "  --model3-skip N    Run model 3 every N+1 frames (default: 0)\n"
        "  --model3-weight W  Model 3 weighted-scheduler share (default: 1)\n"
        "  --model3-conf F    Model 3 confidence threshold\n"
        "  --weighted-scheduler   Enable weighted model scheduler\n"
        "  --max-models-per-frame N  Max models to run per frame in weighted mode\n"
        "  --test-adaptive-hand-person  Test policy: prefer hand, fallback person\n"
        "  --test-hand-slot N    Hand model slot index for test policy (default: 1)\n"
        "  --test-person-slot N  Person model slot index for test policy (default: 0)\n"
        "  --test-weight-high W  High weight in test policy (default: 10)\n"
        "  --test-weight-low W   Low weight in test policy (default: 1)\n"
        "  --test-no-hand-frames N  Frames without hand before person fallback (default: 8)\n"
        "\n"
        "Device options:\n"
        "  --mainpath DEV     Mainpath device     (default: /dev/video8)\n"
        "  --selfpath DEV     Selfpath device     (default: /dev/video9)\n"
        "  --media DEV        Media device        (default: /dev/media1)\n"
        "\n"
        "Runtime control:\n"
        "  --ctrl-sock PATH   Control socket      (default: /tmp/soulcam_ctrl.sock)\n"
        "\n"
        "Soullink (native module):\n"
        "  --soullink               Enable Soullink transport module (default: enabled)\n"
        "  --no-soullink            Disable Soullink module\n"
        "  --soullink-service-id ID Override service identifier\n"
        "  --soullink-mdns TYPE     mDNS service type (default: _soulcamDebug._tcp.local)\n"
        "  --soullink-mqtt-host H   MQTT broker host (default: 127.0.0.1)\n"
        "  --soullink-mqtt-port P   MQTT broker port (default: 1883)\n"
        "  --soullink-mqtt-prefix T MQTT topic prefix (default: soulcam/debug/)\n"
        "  --soullink-mqtt-user U   MQTT username\n"
        "  --soullink-mqtt-pass P   MQTT password\n"
        "  --soullink-mdns-refresh N periodic mDNS refresh seconds (default: 5)\n"
        "  --soullink-sync-root PATH syncFiles managed root\n"
        "  --soullink-sync-state PATH persisted commitID state file\n"
        "  --soullink-compat-id     Use compatibility clientId (<serviceIdentifier>, default)\n"
        "  --soullink-composite-id  Use composite clientId (<deviceType>:<serviceIdentifier>)\n"
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
        "  echo '{\"cmd\":\"debug_models\"}' | \\\n"
        "      socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock\n"
        "\n", prog);
}

static sc::Config parse_args(int argc, char** argv) {
    sc::Config cfg;

    // Temp holders for multi-model CLI args
    std::string model2_path, model3_path;
    int model2_skip = 0, model3_skip = 0;
    int model2_weight = 1, model3_weight = 1;
    float model2_conf = 0.25f, model3_conf = 0.25f;

    static struct option long_opts[] = {
        {"width",         required_argument, nullptr, 'W'},
        {"height",        required_argument, nullptr, 'H'},
        {"fps",           required_argument, nullptr, 'f'},
        {"bitrate",       required_argument, nullptr, 'b'},
        {"port",          required_argument, nullptr, 'p'},
        {"mount",         required_argument, nullptr, 'm'},
        {"sensor-width",  required_argument, nullptr, 'S'},
        {"sensor-height", required_argument, nullptr, 'T'},
        {"ai",            no_argument,       nullptr, 'A'},
        {"overlay",       no_argument,       nullptr, 'O'},
        {"onvif",         no_argument,       nullptr, 'N'},
        {"onvif-port",    required_argument, nullptr, 'R'},
        {"snapshot",      no_argument,       nullptr, 'J'},
        {"snapshot-port", required_argument, nullptr, 'K'},
        {"model",         required_argument, nullptr, 'M'},
        {"model-weight",  required_argument, nullptr, 20},
        {"conf-thresh",   required_argument, nullptr, 'c'},
        {"nms-thresh",    required_argument, nullptr, 'n'},
        {"labels",        required_argument, nullptr, 'L'},
        {"hand-fast-switch", no_argument,    nullptr, 16},
        {"hand-fast-growth", required_argument, nullptr, 17},
        {"hand-fast-area-ratio", required_argument, nullptr, 18},
        {"hand-fast-hold", required_argument, nullptr, 19},
        {"mainpath",      required_argument, nullptr, 1},
        {"selfpath",      required_argument, nullptr, 2},
        {"media",         required_argument, nullptr, 3},
        {"scene-sock",    required_argument, nullptr, 4},
        {"ctrl-sock",     required_argument, nullptr, 5},
        // Soullink module options
        {"soullink",      no_argument,       nullptr, 40},
        {"no-soullink",   no_argument,       nullptr, 41},
        {"soullink-service-id", required_argument, nullptr, 42},
        {"soullink-mdns", required_argument, nullptr, 43},
        {"soullink-mqtt-host", required_argument, nullptr, 44},
        {"soullink-mqtt-port", required_argument, nullptr, 45},
        {"soullink-mqtt-prefix", required_argument, nullptr, 46},
        {"soullink-mqtt-user", required_argument, nullptr, 47},
        {"soullink-mqtt-pass", required_argument, nullptr, 48},
        {"soullink-mdns-refresh", required_argument, nullptr, 56},
        {"soullink-sync-root", required_argument, nullptr, 52},
        {"soullink-sync-state", required_argument, nullptr, 53},
        {"soullink-compat-id", no_argument, nullptr, 54},
        {"soullink-composite-id", no_argument, nullptr, 55},
        // Multi-model options
        {"model2",        required_argument, nullptr, 10},
        {"model2-skip",   required_argument, nullptr, 11},
        {"model2-conf",   required_argument, nullptr, 12},
        {"model2-weight", required_argument, nullptr, 21},
        {"model3",        required_argument, nullptr, 13},
        {"model3-skip",   required_argument, nullptr, 14},
        {"model3-conf",   required_argument, nullptr, 15},
        {"model3-weight", required_argument, nullptr, 22},
        {"weighted-scheduler", no_argument,  nullptr, 23},
        {"max-models-per-frame", required_argument, nullptr, 24},
        {"test-adaptive-hand-person", no_argument, nullptr, 25},
        {"test-hand-slot", required_argument, nullptr, 26},
        {"test-person-slot", required_argument, nullptr, 27},
        {"test-weight-high", required_argument, nullptr, 28},
        {"test-weight-low", required_argument, nullptr, 29},
        {"test-no-hand-frames", required_argument, nullptr, 30},
        {"verbose",       no_argument,       nullptr, 'v'},
        {"help",          no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    namespace DP = sc::SoulCamDp;
    auto& ov = cfg.cli_overrides;

    while ((opt = getopt_long(argc, argv, "vhf:b:p:m:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'W': cfg.stream.width       = atoi(optarg); ov.insert(DP::stream_width);     break;
            case 'H': cfg.stream.height      = atoi(optarg); ov.insert(DP::stream_height);    break;
            case 'f': cfg.stream.fps         = atoi(optarg); ov.insert(DP::stream_fps);       break;
            case 'b': cfg.rtsp.bitrate_kbps  = atoi(optarg); ov.insert(DP::rtsp_bitrate);     break;
            case 'p': cfg.rtsp.port          = atoi(optarg); ov.insert(DP::rtsp_port);        break;
            case 'm': cfg.rtsp.mount         = optarg;       ov.insert(DP::rtsp_mount);       break;
            case 'S': cfg.sensor.width       = atoi(optarg); break;
            case 'T': cfg.sensor.height      = atoi(optarg); break;
            case 'A': cfg.enable_ai          = true;         ov.insert(DP::enable_ai);        break;
            case 'O': cfg.enable_overlay    = true;          ov.insert(DP::enable_overlay);   break;
            case 'N': cfg.enable_onvif     = true; cfg.enable_onvif_device = true; ov.insert(DP::enable_onvif); break;
            case 'R': cfg.onvif_port       = atoi(optarg);  break;
            case 'J': cfg.enable_snapshot   = true;         break;
            case 'K': cfg.snapshot_port     = atoi(optarg);  break;
            case 'M': cfg.rknn.model_path    = optarg;       ov.insert(DP::ai_model_path);    break;
            case 20:  cfg.primary_model_weight = atoi(optarg); break;
            case 'c': cfg.rknn.conf_threshold = atof(optarg); ov.insert(DP::ai_conf_threshold); break;
            case 'n': cfg.rknn.nms_threshold  = atof(optarg); break;
            case 'L': cfg.rknn.labels         = optarg;       ov.insert(DP::ai_labels);        break;
            case 16:  cfg.hand_fast_switch    = true;         break;
            case 17:  cfg.hand_fast_growth    = static_cast<float>(atof(optarg)); break;
            case 18:  cfg.hand_fast_area_ratio = static_cast<float>(atof(optarg)); break;
            case 19:  cfg.hand_fast_hold_frames = atoi(optarg); break;
            case 1:   cfg.isp.mainpath       = optarg;       break;
            case 2:   cfg.isp.selfpath       = optarg;       break;
            case 3:   cfg.isp.media_dev      = optarg;       break;
            case 4:   cfg.scene_sock         = optarg;       break;
            case 5:   cfg.ctrl_sock          = optarg;       break;
            // Soullink module
            case 40:  cfg.soullink.enable = true;  ov.insert(DP::enable_soullink); break;
            case 41:  cfg.soullink.enable = false; ov.insert(DP::enable_soullink); break;
            case 42:  cfg.soullink.service_identifier = optarg; break;
            case 43:  cfg.soullink.mdns_service_type = optarg; break;
            case 44:  cfg.soullink.mqtt_host = optarg;       break;
            case 45:  cfg.soullink.mqtt_port = atoi(optarg); break;
            case 46:  cfg.soullink.mqtt_topic_prefix = optarg; break;
            case 47:  cfg.soullink.mqtt_username = optarg;   break;
            case 48:  cfg.soullink.mqtt_password = optarg;   break;
            case 56:  cfg.soullink.mdns_refresh_sec = atoi(optarg); break;
            case 52:  cfg.soullink.sync_root = optarg; ov.insert(DP::soullink_sync_root); break;
            case 53:  cfg.soullink.sync_state_path = optarg; break;
            case 54:  cfg.soullink.use_composite_client_id = false; break;
            case 55:  cfg.soullink.use_composite_client_id = true;  break;
            // Multi-model
            case 10:  model2_path = optarg;                  ov.insert(DP::model2_path);      break;
            case 11:  model2_skip = atoi(optarg);            break;
            case 12:  model2_conf = static_cast<float>(atof(optarg)); ov.insert(DP::model2_conf); break;
            case 21:  model2_weight = atoi(optarg);          break;
            case 13:  model3_path = optarg;                  break;
            case 14:  model3_skip = atoi(optarg);            break;
            case 15:  model3_conf = static_cast<float>(atof(optarg)); break;
            case 22:  model3_weight = atoi(optarg);          break;
            case 23:  cfg.weighted_scheduler = true;         ov.insert(DP::weighted_scheduler);    break;
            case 24:  cfg.max_models_per_frame = atoi(optarg); ov.insert(DP::max_models_per_frame); break;
            case 25:  cfg.test_adaptive_hand_person = true;  ov.insert(DP::adaptive_tracking);    break;
            case 26:  cfg.test_hand_slot = atoi(optarg); break;
            case 27:  cfg.test_person_slot = atoi(optarg); break;
            case 28:  cfg.test_weight_high = atoi(optarg); break;
            case 29:  cfg.test_weight_low = atoi(optarg); break;
            case 30:  cfg.test_no_hand_frames_to_fallback = atoi(optarg); break;
            case 'v': cfg.verbose            = true;         ov.insert(DP::verbose);           break;
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
        slot.run_weight          = model2_weight;
        cfg.extra_models.push_back(std::move(slot));
    }
    if (!model3_path.empty()) {
        sc::ModelSlotConfig slot;
        slot.rknn.model_path     = model3_path;
        slot.rknn.conf_threshold = model3_conf;
        slot.rknn.nms_threshold  = cfg.rknn.nms_threshold;
        slot.skip_frames         = model3_skip;
        slot.run_weight          = model3_weight;
        cfg.extra_models.push_back(std::move(slot));
    }

    // Clamp advanced hand-switch parameters to safe ranges.
    if (cfg.hand_fast_growth < 0.0f) cfg.hand_fast_growth = 0.0f;
    if (cfg.hand_fast_area_ratio < 0.0f) cfg.hand_fast_area_ratio = 0.0f;
    if (cfg.hand_fast_hold_frames < 1) cfg.hand_fast_hold_frames = 1;
    if (cfg.test_adaptive_hand_person) cfg.weighted_scheduler = true;
    if (cfg.primary_model_weight < 1) cfg.primary_model_weight = 1;
    if (cfg.max_models_per_frame < 1) cfg.max_models_per_frame = 1;
    if (cfg.test_hand_slot < 0) cfg.test_hand_slot = 0;
    if (cfg.test_person_slot < 0) cfg.test_person_slot = 0;
    if (cfg.test_weight_high < 1) cfg.test_weight_high = 1;
    if (cfg.test_weight_low < 1) cfg.test_weight_low = 1;
    if (cfg.test_no_hand_frames_to_fallback < 1) cfg.test_no_hand_frames_to_fallback = 1;
    if (cfg.soullink.mqtt_port < 1 || cfg.soullink.mqtt_port > 65535) cfg.soullink.mqtt_port = 1883;
    if (cfg.soullink.mdns_refresh_sec < 1) cfg.soullink.mdns_refresh_sec = 5;
    if (cfg.soullink.mqtt_topic_prefix.empty()) cfg.soullink.mqtt_topic_prefix = "soulcam/debug/";

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
// Rive renderer (GPU thread)
// ---------------------------------------------------------------------------
static sc::RiveRenderer g_rive_renderer;

// ---------------------------------------------------------------------------
// ONVIF metadata stream
// ---------------------------------------------------------------------------
static sc::OnvifStream* g_onvif = nullptr;
static sc::HandTargetTracker g_hand_tracker;
static bool g_enable_hand_tracker = false;

// ---------------------------------------------------------------------------
// AI detection callback -- logs + publishes to scene hub + ONVIF
// ---------------------------------------------------------------------------

static void on_detections(const std::vector<sc::Detection>& dets,
                           int frame_w, int frame_h) {
    std::vector<sc::Detection> tracked;
    const std::vector<sc::Detection>* active = &dets;

    // Optional test policy:
    // - when any hand appears -> prefer hand model weight and output hand target
    // - when hand disappears for N frames -> prefer person model and output person target
    if (g_adaptive_test_cfg.enabled) {
        const std::vector<sc::Detection> hand_dets =
            dets_for_slot(dets, g_adaptive_test_cfg.hand_slot);
        const std::vector<sc::Detection> person_dets =
            dets_for_slot(dets, g_adaptive_test_cfg.person_slot);
        const bool hand_seen = !hand_dets.empty();
        if (hand_seen) {
            g_adaptive_no_hand_frames = 0;
            if (g_adaptive_test_mode != AdaptiveTestMode::HandPreferred) {
                g_adaptive_test_mode = AdaptiveTestMode::HandPreferred;
                apply_adaptive_test_weights_if_needed(g_adaptive_test_mode);
            }
        } else {
            g_adaptive_no_hand_frames++;
            if (g_adaptive_no_hand_frames >= g_adaptive_test_cfg.no_hand_frames_to_fallback &&
                g_adaptive_test_mode != AdaptiveTestMode::PersonPreferred) {
                g_adaptive_test_mode = AdaptiveTestMode::PersonPreferred;
                apply_adaptive_test_weights_if_needed(g_adaptive_test_mode);
            }
        }

        // Output target follows current policy mode, not only current-frame hand_seen.
        if (g_adaptive_test_mode == AdaptiveTestMode::HandPreferred) {
            tracked = g_hand_policy_tracker.update(hand_dets);
        } else {
            tracked = g_person_policy_tracker.update(person_dets);
        }
        active = &tracked;
    } else if (g_enable_hand_tracker) {
        tracked = g_hand_tracker.update(dets);
        active = &tracked;
    }

    const auto& out = *active;

    SC_LOG_DEBUG("Detections: raw=%zu tracked=%zu in %dx%d",
                 dets.size(), out.size(), frame_w, frame_h);
    for (const auto& d : out) {
        SC_LOG_DEBUG("  [%s] %.2f @ (%d,%d)-(%d,%d)",
                     d.label ? d.label : "?", d.confidence,
                     d.left, d.top, d.right, d.bottom);
    }

    // Update shared overlay state (thread-safe)
    sc::overlay_update(out, frame_w, frame_h);

    // Feed Rive renderer (thread-safe, zero-copy into shared buffer)
    g_rive_renderer.updateDetections(out, frame_w, frame_h);

    // Push to ONVIF metadata stream
    if (g_onvif) {
        sc::onvif_stream_push(g_onvif, out, frame_w, frame_h);
    }

    // Publish JSON to scene hub (extended format with model_id)
    if (g_scene_fd < 0) return;

    std::ostringstream msg;
    msg << "{\"source\":\"soulcam\",\"type\":\"detections\",\"count\":"
        << out.size() << ",\"objects\":[";
    bool first = true;
    for (const auto& d : out) {
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
                g_hand_tracker.reset();
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
        slot_cfg.run_weight          = json_int(msg, "weight", 1);
        slot_cfg.name                = json_str(msg, "name");

        int idx = sc::ai_capture_add_model(ai, slot_cfg);
        if (idx >= 0) {
            g_hand_tracker.reset();
            SC_LOG_INFO("Control: added model slot %d (%s, skip=%d, weight=%d)",
                        idx, path.c_str(), slot_cfg.skip_frames, slot_cfg.run_weight);
        }
    } else if (cmd == "remove_model") {
        int slot = json_int(msg, "slot", -1);
        if (slot <= 0) {
            SC_LOG_WARN("Control: remove_model requires 'slot' > 0");
            return;
        }
        if (ai) {
            int rc = sc::ai_capture_remove_model(ai, slot);
            if (rc == 0) {
                g_hand_tracker.reset();
                SC_LOG_INFO("Control: removed model slot %d", slot);
            }
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
            SC_LOG_INFO("  slot %d: [%s] %s (conf=%.2f, skip=%d, weight=%d, %s)",
                        i, info.name.c_str(), info.rknn.model_path.c_str(),
                        info.rknn.conf_threshold, info.skip_frames, info.run_weight,
                        info.enabled ? "enabled" : "disabled");
        }
    } else if (cmd == "debug_models") {
        if (!ai) {
            SC_LOG_INFO("Control: debug_models -- AI pipeline not running");
            return;
        }
        std::string report = sc::ai_capture_debug_status(ai);
        SC_LOG_INFO("%s", report.c_str());
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
// SoulLink sysCmd model operations (subcmds 7-11)
// ---------------------------------------------------------------------------

static void process_soullink_sys_cmd(
    sc::soullink::Module* sl, sc::AiCapture* ai, sc::Config& cfg,
    const sc::soullink::SysCmdRequest& req)
{
    auto get_str = [&](const char* key) -> std::string {
        if (!req.data.is_object()) return {};
        const auto* v = req.data.get(key);
        return (v && v->is_string()) ? v->as_string() : std::string{};
    };
    auto get_int = [&](const char* key, int def) -> int {
        if (!req.data.is_object()) return def;
        const auto* v = req.data.get(key);
        return (v && v->is_number()) ? v->as_int(def) : def;
    };
    auto get_float = [&](const char* key, float def) -> float {
        if (!req.data.is_object()) return def;
        const auto* v = req.data.get(key);
        return (v && v->is_number()) ? static_cast<float>(v->as_number()) : def;
    };
    auto get_bool = [&](const char* key, bool def) -> bool {
        if (!req.data.is_object()) return def;
        const auto* v = req.data.get(key);
        return (v && v->is_bool()) ? v->as_bool() : def;
    };

    switch (req.subcmd) {
        case 7: { // model swap: {subcmd:7, slot:0, path:"...", conf:0.3}
            std::string path = get_str("path");
            if (path.empty()) {
                sl->respondSysCmd(false, "swap: missing 'path'");
                return;
            }
            if (!ai) {
                sl->respondSysCmd(false, "swap: AI pipeline not running");
                return;
            }
            int slot = get_int("slot", 0);
            sc::RknnConfig rc;
            rc.model_path     = path;
            rc.conf_threshold = get_float("conf", cfg.rknn.conf_threshold);
            rc.nms_threshold  = get_float("nms", 0.45f);
            int err = sc::ai_capture_swap_model(ai, rc, slot);
            if (err == 0) {
                if (slot == 0) {
                    cfg.rknn.model_path = path;
                    cfg.rknn.conf_threshold = rc.conf_threshold;
                }
                g_hand_tracker.reset();
                sl->respondSysCmd(true, "slot " + std::to_string(slot) + " swapped to " + path);
            } else {
                sl->respondSysCmd(false, "swap failed for slot " + std::to_string(slot));
            }
            break;
        }
        case 8: { // model add: {subcmd:8, path:"...", conf:0.3, skip:0, weight:1}
            std::string path = get_str("path");
            if (path.empty()) { sl->respondSysCmd(false, "add: missing 'path'"); return; }
            if (!ai) { sl->respondSysCmd(false, "add: AI pipeline not running"); return; }
            sc::ModelSlotConfig slot_cfg;
            slot_cfg.rknn.model_path     = path;
            slot_cfg.rknn.conf_threshold = get_float("conf", cfg.rknn.conf_threshold);
            slot_cfg.rknn.nms_threshold  = 0.45f;
            slot_cfg.skip_frames         = get_int("skip", 0);
            slot_cfg.run_weight          = get_int("weight", 1);
            int idx = sc::ai_capture_add_model(ai, slot_cfg);
            if (idx >= 0) {
                g_hand_tracker.reset();
                sl->respondSysCmd(true, "added model slot " + std::to_string(idx));
            } else {
                sl->respondSysCmd(false, "add model failed");
            }
            break;
        }
        case 9: { // model remove: {subcmd:9, slot:1}
            int slot = get_int("slot", -1);
            if (slot <= 0) { sl->respondSysCmd(false, "remove: 'slot' must be > 0"); return; }
            if (!ai) { sl->respondSysCmd(false, "remove: AI pipeline not running"); return; }
            int err = sc::ai_capture_remove_model(ai, slot);
            if (err == 0) {
                g_hand_tracker.reset();
                sl->respondSysCmd(true, "removed slot " + std::to_string(slot));
            } else {
                sl->respondSysCmd(false, "remove failed for slot " + std::to_string(slot));
            }
            break;
        }
        case 10: { // model enable: {subcmd:10, slot:1, enable:true}
            int slot = get_int("slot", -1);
            if (slot < 0) { sl->respondSysCmd(false, "enable: missing 'slot'"); return; }
            if (!ai) { sl->respondSysCmd(false, "enable: AI pipeline not running"); return; }
            bool en = get_bool("enable", true);
            sc::ai_capture_enable_model(ai, slot, en);
            sl->respondSysCmd(true, "slot " + std::to_string(slot) + (en ? " enabled" : " disabled"));
            break;
        }
        case 11: { // model list
            if (!ai) { sl->respondSysCmd(false, "list: AI pipeline not running"); return; }
            using JV = sc::soullink::JsonValue;
            int count = sc::ai_capture_model_count(ai);
            JV::Array arr;
            for (int i = 0; i < count; i++) {
                auto info = sc::ai_capture_get_model_info(ai, i);
                JV::Object obj;
                obj["slot"]    = JV(static_cast<int64_t>(i));
                obj["name"]    = JV(info.name);
                obj["path"]    = JV(info.rknn.model_path);
                obj["conf"]    = JV(static_cast<double>(info.rknn.conf_threshold));
                obj["skip"]    = JV(static_cast<int64_t>(info.skip_frames));
                obj["weight"]  = JV(static_cast<int64_t>(info.run_weight));
                obj["enabled"] = JV(info.enabled);
                arr.emplace_back(JV(std::move(obj)));
            }
            sl->respondSysCmd(true, std::to_string(count) + " model slot(s)",
                              JV(std::move(arr)));
            break;
        }
        default:
            sl->respondSysCmd(false, "unknown sysCmd subcmd " + std::to_string(req.subcmd));
            break;
    }
}

// ---------------------------------------------------------------------------
// Store <-> Config bridging
// ---------------------------------------------------------------------------

static void config_to_store(const sc::Config& cfg) {
    using namespace sc::SoulCamDp;
    auto& s = sc::Store::instance();
    const auto& ov = cfg.cli_overrides;

    auto has = [&](int dp) { return ov.count(dp) > 0; };

    if (has(stream_width))        s.set(stream_width,        (uint32_t)cfg.stream.width);
    if (has(stream_height))       s.set(stream_height,       (uint32_t)cfg.stream.height);
    if (has(stream_fps))          s.set(stream_fps,          (uint32_t)cfg.stream.fps);
    if (has(rtsp_bitrate))        s.set(rtsp_bitrate,        (uint32_t)cfg.rtsp.bitrate_kbps);
    if (has(rtsp_port))           s.set(rtsp_port,           (uint32_t)cfg.rtsp.port);
    if (has(rtsp_mount))          s.set(rtsp_mount,          std::string(cfg.rtsp.mount));
    if (has(enable_ai))           s.set(enable_ai,           cfg.enable_ai);
    if (has(enable_overlay))      s.set(enable_overlay,      cfg.enable_overlay);
    if (has(ai_conf_threshold))   s.set(ai_conf_threshold,   cfg.rknn.conf_threshold);
    if (has(ai_model_path))       s.set(ai_model_path,       std::string(cfg.rknn.model_path));
    if (has(ai_labels))           s.set(ai_labels,           std::string(cfg.rknn.labels));
    if (has(enable_soullink))     s.set(enable_soullink,     cfg.soullink.enable);
    if (has(enable_onvif))        s.set(enable_onvif,        cfg.enable_onvif);
    if (has(verbose))             s.set(verbose,             cfg.verbose);
    if (has(adaptive_tracking))   s.set(adaptive_tracking,   cfg.test_adaptive_hand_person);
    if (has(weighted_scheduler))  s.set(weighted_scheduler,  cfg.weighted_scheduler);
    if (has(max_models_per_frame))s.set(max_models_per_frame,(uint32_t)cfg.max_models_per_frame);
    if (has(soullink_sync_root))  s.set(soullink_sync_root,  std::string(cfg.soullink.sync_root));
    if (has(model2_path) && !cfg.extra_models.empty())
        s.set(model2_path, std::string(cfg.extra_models[0].rknn.model_path));
    if (has(model2_conf) && !cfg.extra_models.empty())
        s.set(model2_conf, cfg.extra_models[0].rknn.conf_threshold);
}

static void store_to_config(sc::Config& cfg) {
    using namespace sc::SoulCamDp;
    auto& s = sc::Store::instance();

    cfg.stream.width          = (int)s.get<uint32_t>(stream_width);
    cfg.stream.height         = (int)s.get<uint32_t>(stream_height);
    cfg.stream.fps            = (int)s.get<uint32_t>(stream_fps);
    cfg.rtsp.bitrate_kbps     = (int)s.get<uint32_t>(rtsp_bitrate);
    cfg.rtsp.port             = (int)s.get<uint32_t>(rtsp_port);
    cfg.rtsp.mount            = s.get<std::string>(rtsp_mount);
    cfg.enable_ai             = s.get<bool>(enable_ai);
    cfg.enable_overlay        = s.get<bool>(enable_overlay);
    cfg.rknn.conf_threshold   = s.get<float>(ai_conf_threshold);
    cfg.rknn.model_path       = s.get<std::string>(ai_model_path);
    cfg.rknn.labels           = s.get<std::string>(ai_labels);
    cfg.soullink.enable       = s.get<bool>(enable_soullink);
    cfg.enable_onvif          = s.get<bool>(enable_onvif);
    cfg.verbose               = s.get<bool>(verbose);
    cfg.soullink.sync_root    = s.get<std::string>(soullink_sync_root);
    cfg.test_adaptive_hand_person = s.get<bool>(adaptive_tracking);
    cfg.weighted_scheduler    = s.get<bool>(weighted_scheduler);
    cfg.max_models_per_frame  = (int)s.get<uint32_t>(max_models_per_frame);

    // Model 2 from DPs (if not already loaded from CLI)
    std::string m2 = s.get<std::string>(model2_path);
    if (!m2.empty() && cfg.extra_models.empty()) {
        sc::ModelSlotConfig slot;
        slot.rknn.model_path     = m2;
        slot.rknn.conf_threshold = s.get<float>(model2_conf);
        slot.rknn.nms_threshold  = 0.45f;
        cfg.extra_models.push_back(std::move(slot));
    }
    if (cfg.test_adaptive_hand_person) cfg.weighted_scheduler = true;

    // Derived / hard-coded (no longer DPs)
    cfg.rtsp.gop              = cfg.stream.fps;
    cfg.rtsp.encoder          = "mpp";
    cfg.enable_onvif_device   = cfg.enable_onvif;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // 1. Parse CLI args into Config (sets all fields including defaults)
    sc::Config cfg = parse_args(argc, argv);

    // 2. Initialize Store: defaults -> load persisted -> overlay CLI values
    sc::Store::instance().initialize();
    sc::Store::instance().load();
    config_to_store(cfg);         // only explicit CLI args override persisted values
    if (!cfg.cli_overrides.empty())
        sc::Store::instance().save();
    store_to_config(cfg);         // ensure Config is in sync
    g_enable_hand_tracker = should_enable_hand_target_tracker(cfg);
    g_hand_tracker.set_config(make_hand_tracker_config(cfg), true);
    g_adaptive_test_cfg.enabled = cfg.test_adaptive_hand_person;
    g_adaptive_test_cfg.hand_slot = cfg.test_hand_slot;
    g_adaptive_test_cfg.person_slot = cfg.test_person_slot;
    g_adaptive_test_cfg.high_weight = cfg.test_weight_high;
    g_adaptive_test_cfg.low_weight = cfg.test_weight_low;
    g_adaptive_test_cfg.no_hand_frames_to_fallback = cfg.test_no_hand_frames_to_fallback;
    g_adaptive_test_mode = AdaptiveTestMode::Neutral;
    g_adaptive_no_hand_frames = 0;
    g_hand_policy_tracker.reset();
    g_person_policy_tracker.reset();

    if (cfg.verbose) {
        sc::log_set_level(sc::LogLevel::DEBUG);
    }

    SC_LOG_INFO("SoulCam v%s starting", "0.2.0");
    SC_LOG_INFO("Hand target tracker: %s (labels=\"%s\")",
                g_enable_hand_tracker ? "enabled" : "disabled",
                cfg.rknn.labels.c_str());
    SC_LOG_INFO("Hand fast switch (phase3): %s (growth>=%.2f, ratio>=%.2f, hold=%d)",
                cfg.hand_fast_switch ? "enabled" : "disabled",
                cfg.hand_fast_growth,
                cfg.hand_fast_area_ratio,
                cfg.hand_fast_hold_frames);
    SC_LOG_INFO("Model scheduler: %s (max_models_per_frame=%d, primary_weight=%d)",
                cfg.weighted_scheduler ? "weighted" : "run-all",
                cfg.max_models_per_frame,
                cfg.primary_model_weight);
    SC_LOG_INFO("Adaptive hand/person test policy: %s (hand_slot=%d, person_slot=%d, hi=%d, lo=%d, fallback=%d)",
                cfg.test_adaptive_hand_person ? "enabled" : "disabled",
                cfg.test_hand_slot, cfg.test_person_slot,
                cfg.test_weight_high, cfg.test_weight_low,
                cfg.test_no_hand_frames_to_fallback);

    // --- Set GStreamer plugin path BEFORE gst_init() ---
    // rgaconvert lives in a custom path; GStreamer scans plugins at init time.
    const char* existing_pp = g_getenv("GST_PLUGIN_PATH");
    std::string plugin_path = SOULCAM_RGA_PLUGIN_PATH;
    if (existing_pp && existing_pp[0] != '\0') {
        plugin_path = plugin_path + ":" + existing_pp;
    }
    g_setenv("GST_PLUGIN_PATH", plugin_path.c_str(), TRUE);

    // Suppress noisy GStreamer/RGA debug unless verbose.
    // RGA plugin uses raw printf (bypasses GST_DEBUG), so we redirect stderr
    // to /dev/null and route SoulCam logs through a saved fd.
    if (!cfg.verbose) {
        sc::log_redirect_stderr_quiet();
        g_setenv("GST_DEBUG", "0", TRUE);
    }

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
            g_ai_for_policy = nullptr;
        } else {
            g_ai_for_policy = ai;
            if (g_adaptive_test_cfg.enabled) {
                apply_adaptive_test_weights_if_needed(AdaptiveTestMode::Neutral);
            }
        }
    }

    // --- Rive renderer (optional, GPU thread) ---
    {
        using namespace sc::SoulCamDp;
        auto& store = sc::Store::instance();
        bool rive_enabled = store.get<bool>(enable_rive);
        std::string riv_path = store.get<std::string>(rive_file);
        uint32_t rive_res = store.get<uint32_t>(rive_resolution);
        std::string rive_tgt = store.get<std::string>(rive_target);

        sc::RiveRendererConfig rcfg;
        rcfg.riv_file = riv_path;
        rcfg.resolution = rive_res > 0 ? rive_res : 500;
        rcfg.target_label = rive_tgt.empty() ? "person" : rive_tgt;
        rcfg.enabled = rive_enabled && !riv_path.empty();

        g_rive_renderer.start(rcfg);

        // DP change listener for runtime Rive config
        store.addChangeListener([](int key, const sc::StateType&, const sc::StateType&) {
            using namespace sc::SoulCamDp;
            auto& s = sc::Store::instance();
            switch (key) {
                case enable_rive:
                    g_rive_renderer.setEnabled(s.get<bool>(enable_rive));
                    break;
                case rive_resolution:
                    g_rive_renderer.setResolution(s.get<uint32_t>(rive_resolution));
                    break;
                case rive_file:
                    g_rive_renderer.setRivFile(s.get<std::string>(rive_file));
                    break;
                case rive_target:
                    g_rive_renderer.setTargetLabel(s.get<std::string>(rive_target));
                    break;
            }
        });

        if (rive_enabled && !riv_path.empty()) {
            SC_LOG_INFO("Rive renderer: started (file=%s, res=%u, target=%s)",
                        riv_path.c_str(), rive_res, rive_tgt.c_str());
        } else {
            SC_LOG_INFO("Rive renderer: standby (set enable_rive=true and rive_file via DP)");
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

    // --- Soullink module (in-process) ---
    std::unique_ptr<sc::soullink::Module> soullink;
    if (cfg.soullink.enable) {
        soullink = std::make_unique<sc::soullink::Module>(cfg);
        soullink->start();
    }

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
    SC_LOG_INFO("  I/O mode: userptr");
    if (ai) {
        int model_count = sc::ai_capture_model_count(ai);
        SC_LOG_INFO("  AI: selfpath %dx%d@%d %s -> %d model slot(s)",
                    cfg.ai.width, cfg.ai.height, cfg.ai.fps,
                    cfg.ai.src_fmt.c_str(), model_count);
        for (int i = 0; i < model_count; i++) {
            auto info = sc::ai_capture_get_model_info(ai, i);
            SC_LOG_INFO("    slot %d: [%s] %s (conf=%.2f, skip=%d, weight=%d)",
                        i, info.name.c_str(), info.rknn.model_path.c_str(),
                        info.rknn.conf_threshold, info.skip_frames, info.run_weight);
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
    SC_LOG_INFO("  Rive renderer: %s", g_rive_renderer.running() ? "running (GPU thread)" : "standby");
    SC_LOG_INFO("  Soullink: %s", soullink ? "enabled (native C++ module)" : "disabled");
    SC_LOG_INFO("  Press Ctrl+C to stop");
    fprintf(stderr, "\n");

    // --- Main loop: poll control socket + SoulLink commands ---
    while (!g_shutdown) {
        ctrl_poll(ai, cfg);
        if (soullink) {
            soullink->poll();
            sc::soullink::SysCmdRequest req;
            while (soullink->popSysCmd(req)) {
                process_soullink_sys_cmd(soullink.get(), ai, cfg, req);
            }
        }
        usleep(100000);  // 100ms polling interval
    }

    // --- Cleanup ---
    SC_LOG_INFO("Shutting down...");
    g_rive_renderer.stop();
    if (soullink) soullink->stop();
    ctrl_close();
    sc::snapshot_server_stop(snap_srv);
    sc::onvif_device_stop(onvif_dev);
    sc::onvif_stream_stop(g_onvif);
    g_onvif = nullptr;
    sc::ai_capture_stop(ai);
    g_ai_for_policy = nullptr;
    scene_hub_close();
    sc::rtsp_server_stop(rtsp);

    SC_LOG_INFO("SoulCam stopped");
    return 0;
}
