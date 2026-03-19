#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <variant>

namespace sc {

using StateType = std::variant<uint32_t, int, float, bool, std::string>;

namespace SoulCamDp {

    enum Key : int {
        START = 0,
        PERSIST_START = START,

        // Stream
        stream_width,           // uint32_t  1280
        stream_height,          // uint32_t  960
        stream_fps,             // uint32_t  30

        // RTSP
        rtsp_bitrate,           // uint32_t  4000  (kbps)
        rtsp_port,              // uint32_t  8554

        // AI
        enable_ai,              // bool      false
        enable_overlay,         // bool      false
        ai_conf_threshold,      // float     0.25

        // Feature toggles
        enable_soullink,        // bool      true
        enable_onvif,           // bool      false
        verbose,                // bool      false

        // Multi-model & adaptive tracking
        model2_conf,            // float     0.15
        adaptive_tracking,      // bool      false
        weighted_scheduler,     // bool      false
        max_models_per_frame,   // uint32_t  1

        // Rive renderer
        enable_rive,            // bool      false
        rive_resolution,        // uint32_t  500

        // Interframe tracker
        tracker_yolo_interval,  // uint32_t  1     (1 = disabled)
        tracker_enable_mosse,   // bool      true
        tracker_mosse_psr,      // float     7.0
        tracker_mosse_learn_rate,// float    0.125
        tracker_mosse_patch_size,// uint32_t 64
        tracker_roi_padding,    // float     2.0
        tracker_smooth_factor,  // float     0.6
        tracker_adaptive_interval, // bool   false
        tracker_max_skip,       // uint32_t  8
        tracker_min_skip,       // uint32_t  2
        tracker_hand_confirm,   // uint32_t  3  (YOLO frames with hand to confirm switch)
        tracker_hand_lost,      // uint32_t  5  (YOLO frames without hand to fallback)
        ai_target_fps,          // uint32_t  0  (0 = unlimited; >0 = cap AI pipeline FPS)

        VALUE_END,

        // String persist keys (offset to avoid binary-format issues in future)
        STRING_START = 100,
        rtsp_mount,             // string    "/cam"
        ai_model_path,          // string    ""
        ai_labels,              // string    ""
        soullink_sync_root,     // string    "/home/ubuntu/SoulCam"
        model2_path,            // string    ""
        rive_file,              // string    ""
        rive_target,            // string    "person"
        model2_labels,          // string    ""  (e.g. "hand")
        STRING_END,

        PERSIST_END = STRING_END,

        // RAM-only (runtime health / status)
        RAM_START = 1000,
        rtsp_online,            // 1001  bool
        stream_subscribed,      // 1002  bool
        module_ready,           // 1003  bool
        RAM_END,

        END = RAM_END,
    };

    inline bool isPersist(int key) {
        return (key > PERSIST_START && key < VALUE_END) ||
               (key > STRING_START && key < STRING_END);
    }

    inline bool isRam(int key) {
        return key > RAM_START && key < RAM_END;
    }

    inline bool isValid(int key) { return isPersist(key) || isRam(key); }

    std::unordered_map<int, StateType> getDefaultValueMap();
    std::map<std::string, int>         getPersistKeyMap();
    const char*                        keyName(int key);

}  // namespace SoulCamDp
}  // namespace sc
