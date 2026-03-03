#pragma once
// ============================================================================
// SoulCam -- Core configuration and shared types
//
// Hardware:  Vicharak (RK3566) + OV5647 160deg fisheye
// ISP:       rkisp v21, RKAIQ v6.0x8.0
// Paths:     mainpath /dev/video8   (IPC stream, 1280x960 NV12)
//            selfpath /dev/video9   (AI feed,    640x480  NV12)
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

// ---------------------------------------------------------------------------
// ISP device topology  (from KEY_POINTS.md)
// ---------------------------------------------------------------------------
struct IspDevices {
    // Media device
    std::string media_dev    = "/dev/media1";
    // Sensor sub-device
    std::string sensor_entity = "m00_b_ov5647 1-0036";
    std::string sensor_subdev = "/dev/v4l-subdev2";
    // CSI DPHY
    std::string csi_dphy     = "rockchip-csi2-dphy0";
    // ISP sub-devices
    std::string csi_subdev   = "rkisp-csi-subdev";
    std::string isp_subdev   = "rkisp-isp-subdev";
    // Video capture nodes
    std::string mainpath     = "/dev/video8";   // rkisp_mainpath
    std::string selfpath     = "/dev/video9";   // rkisp_selfpath
};

// ---------------------------------------------------------------------------
// Stream resolution / format
// ---------------------------------------------------------------------------
struct StreamConfig {
    int  width          = 1280;
    int  height         = 960;
    int  fps            = 30;
    // ISP source format on mainpath.
    // NV12 works correctly (UV valid from frame #2 onwards, frame #1 warm-up).
    // The previous Y=0/UV=0x80 "bug" was caused by analyzing only the first
    // ISP warm-up frame — see doc/isp/NV12_Y_ZERO_BUG.md for full analysis.
    std::string src_fmt = "NV12";
};

struct AiConfig {
    int  width          = 640;
    int  height         = 480;
    int  fps            = 30;
    // Selfpath format -- NV12 works correctly (UV valid from frame #2).
    // RGA converts NV12→RGB for RKNN inference.
    std::string src_fmt = "NV12";
};

// ---------------------------------------------------------------------------
// Sensor configuration
// ---------------------------------------------------------------------------
struct SensorConfig {
    int  width          = 1296;     // OV5647 2x2-binned mode (fast 4:3)
    int  height         = 972;
    std::string mbus_fmt = "SGBRG10_1X10";
    int  vblank         = 24;       // vertical blanking for timing
};

// ---------------------------------------------------------------------------
// RTSP configuration
// ---------------------------------------------------------------------------
struct RtspConfig {
    int         port        = 8554;
    std::string mount       = "/cam";
    int         bitrate_kbps = 4000;
    int         gop         = 30;       // key-frame interval (frames)
    // Encoder: "mpp" (HW) or "x264" (SW fallback)
    std::string encoder     = "mpp";
};

// ---------------------------------------------------------------------------
// AI / RKNN configuration
// ---------------------------------------------------------------------------
struct RknnConfig {
    std::string model_path;             // e.g. "rk3566/yolov8n.rknn"
    float       conf_threshold = 0.25f;
    float       nms_threshold  = 0.45f;
    std::string labels;                 // comma-separated labels (e.g. "hand")
                                        // empty = auto-detect from model (COCO 80)
};

// ---------------------------------------------------------------------------
// Multi-model pipeline configuration
// ---------------------------------------------------------------------------
struct ModelSlotConfig {
    std::string name;                   // Human-readable name (e.g. "yolov8n")
    RknnConfig  rknn;                   // Model path + thresholds
    int         skip_frames = 0;        // Run every N+1 frames (0 = every frame)
    int         run_weight  = 1;        // Weighted scheduler share (relative)
    bool        enabled     = true;     // Can be toggled at runtime
};

// ---------------------------------------------------------------------------
// Top-level application config
// ---------------------------------------------------------------------------
struct Config {
    IspDevices   isp;
    SensorConfig sensor;
    StreamConfig stream;
    AiConfig     ai;
    RtspConfig   rtsp;
    RknnConfig   rknn;

    bool         enable_ai      = false;   // --ai flag to enable selfpath + RKNN
    bool         enable_overlay = false;   // --overlay flag to draw detection boxes
    bool         enable_onvif   = false;   // --onvif flag for ONVIF metadata stream
    bool         enable_onvif_device = false; // --onvif-device for WS-Discovery + SOAP
    bool         enable_snapshot = false;  // --snapshot flag for JPEG snapshot HTTP server
    bool         use_dmabuf     = false;   // --dmabuf flag for zero-copy DMA-BUF I/O
    bool         verbose        = false;
    int          onvif_port     = 8080;    // ONVIF device service HTTP port
    int          snapshot_port  = 8088;    // JPEG snapshot HTTP port

    // Scene hub: publish detection JSON over Unix datagram socket
    std::string  scene_sock   = "/tmp/soulcam_scene.sock";

    // Control socket: receive runtime commands (model hot-swap, status)
    std::string  ctrl_sock    = "/tmp/soulcam_ctrl.sock";

    // Multi-model: additional model slots beyond the primary (rknn)
    // Slot 0 is always the primary model (from --model / rknn).
    // Slots 1+ are extra models (from --model2, --model3, etc.)
    std::vector<ModelSlotConfig> extra_models;

    // Weighted model scheduler (optional).
    // If enabled, only up to max_models_per_frame are run per frame,
    // selected by weighted round-robin across loaded+enabled slots.
    bool         weighted_scheduler = false;
    int          max_models_per_frame = 1;
    int          primary_model_weight = 1;

    // Test-only adaptive scheduling policy:
    // prefer hand model when hand is present, otherwise prefer person model.
    bool         test_adaptive_hand_person = false;
    int          test_hand_slot = 1;
    int          test_person_slot = 0;
    int          test_weight_high = 10;
    int          test_weight_low = 1;
    int          test_no_hand_frames_to_fallback = 8;

    // Advanced hand-target switching (Phase 3).
    // Disabled by default; applies only when hand single-target tracker is enabled.
    bool         hand_fast_switch = false;
    float        hand_fast_growth = 0.18f;
    float        hand_fast_area_ratio = 0.90f;
    int          hand_fast_hold_frames = 3;
};

// ---------------------------------------------------------------------------
// Detection result (shared between AI and stream overlay)
// ---------------------------------------------------------------------------
struct Detection {
    int         cls_id;
    const char* label;
    float       confidence;
    int         left, top, right, bottom;
    int         model_id = 0;       // Which model slot produced this detection
};

}  // namespace sc
