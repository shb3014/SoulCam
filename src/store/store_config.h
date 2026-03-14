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
        model2_conf,            // float     0.25
        adaptive_tracking,      // bool      false
        weighted_scheduler,     // bool      false
        max_models_per_frame,   // uint32_t  1

        // Rive renderer
        enable_rive,            // bool      false
        rive_resolution,        // uint32_t  500

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
