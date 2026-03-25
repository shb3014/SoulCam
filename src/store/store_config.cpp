#include "store/store_config.h"

namespace sc::SoulCamDp {

std::unordered_map<int, StateType> getDefaultValueMap() {
    return {
        // Stream
        {stream_width,          (uint32_t)1280},
        {stream_height,         (uint32_t)960},
        {stream_fps,            (uint32_t)30},

        // RTSP
        {rtsp_bitrate,          (uint32_t)4000},
        {rtsp_port,             (uint32_t)8554},

        // AI
        {enable_ai,             true},
        {enable_overlay,        false},
        {ai_conf_threshold,     0.25f},

        // Feature toggles
        {enable_soullink,       true},
        {enable_onvif,          false},
        {verbose,               false},

        // Multi-model & adaptive tracking
        {model2_conf,           0.15f},
        {adaptive_tracking,     false},
        {weighted_scheduler,    false},
        {max_models_per_frame,  (uint32_t)1},

        // Rive renderer
        {enable_rive,           false},
        {rive_resolution,       (uint32_t)500},

        // Interframe tracker
        {tracker_yolo_interval,   (uint32_t)1},
        {tracker_enable_mosse,    true},
        {tracker_mosse_psr,       7.0f},
        {tracker_mosse_learn_rate,0.125f},
        {tracker_mosse_patch_size,(uint32_t)64},
        {tracker_roi_padding,     2.0f},
        {tracker_smooth_factor,   0.6f},
        {tracker_adaptive_interval, false},
        {tracker_max_skip,        (uint32_t)8},
        {tracker_min_skip,        (uint32_t)2},
        {tracker_hand_confirm,    (uint32_t)3},
        {tracker_hand_lost,       (uint32_t)5},
        {ai_target_fps,           (uint32_t)0},

        // Perception pipeline
        {enable_perception,             false},
        {perception_max_tracked,        (uint32_t)5},
        {perception_embed_dim,          (uint32_t)128},
        {perception_embed_input,        (uint32_t)128},
        {perception_hot_tier_max,       (uint32_t)1000},
        {perception_interest_novelty_hl, 24.0f},
        {perception_interest_motion_w,   0.15f},
        {perception_interest_threshold,  0.10f},
        {perception_enrollment_delay,    (uint32_t)5},
        {perception_vlm_enabled,         false},

        // Strings
        {rtsp_mount,            std::string("/cam")},
        {ai_model_path,         std::string("")},
        {ai_labels,             std::string("")},
        {soullink_sync_root,    std::string("/home/ubuntu/SoulCam")},
        {model2_path,           std::string("")},
        {rive_file,             std::string("")},
        {rive_target,           std::string("person")},
        {model2_labels,         std::string("")},

        // Perception pipeline (string)
        {perception_embedder_model, std::string("")},
        {perception_memory_dir,     std::string("/var/lib/soulcam/memory")},
        {perception_vlm_api_url,    std::string("")},
        {perception_vlm_api_key,    std::string("")},
        {perception_vlm_model,      std::string("gpt-4o")},

        // RAM (runtime)
        {rtsp_online,           false},
        {stream_subscribed,     false},
        {module_ready,          false},
    };
}

std::map<std::string, int> getPersistKeyMap() {
    return {
        {"stream_width",          stream_width},
        {"stream_height",         stream_height},
        {"stream_fps",            stream_fps},
        {"rtsp_bitrate",          rtsp_bitrate},
        {"rtsp_port",             rtsp_port},
        {"enable_ai",             enable_ai},
        {"enable_overlay",        enable_overlay},
        {"ai_conf_threshold",     ai_conf_threshold},
        {"enable_soullink",       enable_soullink},
        {"enable_onvif",          enable_onvif},
        {"verbose",               verbose},
        {"model2_conf",           model2_conf},
        {"adaptive_tracking",     adaptive_tracking},
        {"weighted_scheduler",    weighted_scheduler},
        {"max_models_per_frame",  max_models_per_frame},
        {"enable_rive",           enable_rive},
        {"rive_resolution",       rive_resolution},
        {"tracker_yolo_interval",   tracker_yolo_interval},
        {"tracker_enable_mosse",    tracker_enable_mosse},
        {"tracker_mosse_psr",       tracker_mosse_psr},
        {"tracker_mosse_learn_rate",tracker_mosse_learn_rate},
        {"tracker_mosse_patch_size",tracker_mosse_patch_size},
        {"tracker_roi_padding",     tracker_roi_padding},
        {"tracker_smooth_factor",   tracker_smooth_factor},
        {"tracker_adaptive_interval", tracker_adaptive_interval},
        {"tracker_max_skip",        tracker_max_skip},
        {"tracker_min_skip",        tracker_min_skip},
        {"tracker_hand_confirm",    tracker_hand_confirm},
        {"tracker_hand_lost",       tracker_hand_lost},
        {"ai_target_fps",           ai_target_fps},
        // Perception pipeline
        {"enable_perception",           enable_perception},
        {"perception_max_tracked",      perception_max_tracked},
        {"perception_embed_dim",        perception_embed_dim},
        {"perception_embed_input",      perception_embed_input},
        {"perception_hot_tier_max",     perception_hot_tier_max},
        {"perception_interest_novelty_hl", perception_interest_novelty_hl},
        {"perception_interest_motion_w",   perception_interest_motion_w},
        {"perception_interest_threshold",  perception_interest_threshold},
        {"perception_enrollment_delay",    perception_enrollment_delay},
        {"perception_vlm_enabled",         perception_vlm_enabled},
        {"rtsp_mount",            rtsp_mount},
        {"ai_model_path",         ai_model_path},
        {"ai_labels",             ai_labels},
        {"soullink_sync_root",    soullink_sync_root},
        {"model2_path",           model2_path},
        {"rive_file",             rive_file},
        {"rive_target",           rive_target},
        {"model2_labels",         model2_labels},
        // Perception pipeline (string)
        {"perception_embedder_model", perception_embedder_model},
        {"perception_memory_dir",     perception_memory_dir},
        {"perception_vlm_api_url",    perception_vlm_api_url},
        {"perception_vlm_api_key",    perception_vlm_api_key},
        {"perception_vlm_model",      perception_vlm_model},
    };
}

const char* keyName(int key) {
    static const std::map<std::string, int> km = getPersistKeyMap();
    for (const auto& [name, id] : km) {
        if (id == key) return name.c_str();
    }
    switch (key) {
        case rtsp_online:       return "rtsp_online";
        case stream_subscribed: return "stream_subscribed";
        case module_ready:      return "module_ready";
        default:                return "unknown";
    }
}

}  // namespace sc::SoulCamDp
