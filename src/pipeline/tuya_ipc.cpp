// ============================================================================
// Tuya IPC SDK Adapter — Skeleton Implementation
//
// This file provides stub implementations of the Tuya adapter interfaces.
// When the actual Tuya IPC SDK (`libtuya_ipc.a`) is linked and
// SOULCAM_HAVE_TUYA is defined, the stubs are replaced with real SDK calls.
//
// To enable Tuya integration:
//   1. Download TuyaOS IPC SDK for aarch64-linux
//   2. Place SDK headers in tuya_sdk/include/ and library in tuya_sdk/lib/
//   3. Enable in CMakeLists.txt: -DSOULCAM_HAVE_TUYA=1
//   4. Provide credentials via CLI: --tuya-pid, --tuya-uuid, --tuya-authkey
//
// See doc/tuya/TUYA_INTEGRATION_PLAN.md for the full plan.
// ============================================================================

#include "pipeline/tuya_ipc.h"
#include "util/logger.h"

#include <mutex>
#include <atomic>

// ---------------------------------------------------------------------------
// Tuya SDK headers (conditional)
// ---------------------------------------------------------------------------
#ifdef SOULCAM_HAVE_TUYA
#include "tuya_ipc_api.h"
#include "tuya_ipc_media.h"
#include "tuya_ipc_p2p.h"
#include "tuya_ring_buffer.h"
#include "tuya_ipc_cloud_storage.h"
#include "tuya_ipc_skill.h"
#endif

namespace sc {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct TuyaIpc {
    TuyaConfig           tcfg;
    Config               cfg;
    std::atomic<TuyaStatus> status{TuyaStatus::UNREGISTERED};
    std::mutex           frame_mutex;

    // Callbacks
    TuyaStatusCallback   on_status;
    TuyaStreamCallback   on_stream;
    TuyaDpCallback       on_dp;

#ifdef SOULCAM_HAVE_TUYA
    Ring_Buffer_User_Handle_S  rb_main = nullptr;
    Ring_Buffer_User_Handle_S  rb_sub  = nullptr;
    Ring_Buffer_User_Handle_S  rb_audio = nullptr;
#endif

    // Stats
    uint64_t frames_pushed = 0;
    uint64_t events_sent   = 0;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TuyaIpc* tuya_ipc_init(const TuyaConfig& tcfg, const Config& cfg) {
#ifdef SOULCAM_HAVE_TUYA
    if (tcfg.creds.product_id.empty() || tcfg.creds.uuid.empty() ||
        tcfg.creds.auth_key.empty()) {
        SC_LOG_ERROR("Tuya: missing credentials (PID/UUID/AuthKey)");
        return nullptr;
    }

    auto* t = new TuyaIpc();
    t->tcfg = tcfg;
    t->cfg = cfg;

    // Initialize Tuya SDK
    TUYA_IPC_ENV_VAR_S env = {};
    strncpy(env.product_key, tcfg.creds.product_id.c_str(), sizeof(env.product_key) - 1);
    strncpy(env.uuid, tcfg.creds.uuid.c_str(), sizeof(env.uuid) - 1);
    strncpy(env.auth_key, tcfg.creds.auth_key.c_str(), sizeof(env.auth_key) - 1);
    strncpy(env.storage_path, tcfg.creds.storage_path.c_str(), sizeof(env.storage_path) - 1);
    strncpy(env.dev_sw_version, tcfg.creds.firmware_ver.c_str(), sizeof(env.dev_sw_version) - 1);

    // TODO: Set up DP callbacks, status callbacks, OTA callback
    // env.dev_obj_dp_cb = ...
    // env.dev_dp_query_cb = ...
    // env.status_changed_cb = ...
    // env.gw_ug_cb = ...
    // env.gw_rst_cb = ...
    // env.gw_restart_cb = ...

    SC_LOG_INFO("Tuya: initializing SDK (PID=%s, UUID=%s)",
                tcfg.creds.product_id.c_str(), tcfg.creds.uuid.c_str());

    // int rc = tuya_ipc_init_sdk(&env);
    // if (rc != 0) {
    //     SC_LOG_ERROR("Tuya: SDK init failed (rc=%d)", rc);
    //     delete t;
    //     return nullptr;
    // }

    t->status = TuyaStatus::REGISTERED;
    return t;
#else
    (void)tcfg;
    (void)cfg;
    SC_LOG_INFO("Tuya: SDK not linked (SOULCAM_HAVE_TUYA not defined). "
                "Running in stub mode -- no cloud connectivity.");
    SC_LOG_INFO("Tuya: To enable, see doc/tuya/TUYA_INTEGRATION_PLAN.md");

    // Return a stub handle so the rest of the code can reference it
    auto* t = new TuyaIpc();
    t->tcfg = tcfg;
    t->cfg = cfg;
    return t;
#endif
}

int tuya_ipc_start(TuyaIpc* t) {
    if (!t) return -1;

#ifdef SOULCAM_HAVE_TUYA
    // Configure media info
    IPC_MEDIA_INFO_S media_info = {};

    // Main video stream
    media_info.channel_enable[E_CHANNEL_VIDEO_MAIN] = TRUE;
    media_info.video_codec[E_CHANNEL_VIDEO_MAIN] = TUYA_CODEC_VIDEO_H264;
    media_info.video_width[E_CHANNEL_VIDEO_MAIN] = t->tcfg.main_width;
    media_info.video_height[E_CHANNEL_VIDEO_MAIN] = t->tcfg.main_height;
    media_info.video_fps[E_CHANNEL_VIDEO_MAIN] = t->tcfg.main_fps;
    media_info.video_gop[E_CHANNEL_VIDEO_MAIN] = t->tcfg.main_fps;  // 1 GOP = 1s
    media_info.video_bitrate[E_CHANNEL_VIDEO_MAIN] = TUYA_VIDEO_BITRATE_2M;
    media_info.video_freq[E_CHANNEL_VIDEO_MAIN] = 90000;

    // Sub video stream (if enabled)
    if (t->tcfg.sub_enabled) {
        media_info.channel_enable[E_CHANNEL_VIDEO_SUB] = TRUE;
        media_info.video_codec[E_CHANNEL_VIDEO_SUB] = TUYA_CODEC_VIDEO_H264;
        media_info.video_width[E_CHANNEL_VIDEO_SUB] = t->tcfg.sub_width;
        media_info.video_height[E_CHANNEL_VIDEO_SUB] = t->tcfg.sub_height;
        media_info.video_fps[E_CHANNEL_VIDEO_SUB] = t->tcfg.sub_fps;
        media_info.video_gop[E_CHANNEL_VIDEO_SUB] = t->tcfg.sub_fps;
        media_info.video_bitrate[E_CHANNEL_VIDEO_SUB] = TUYA_VIDEO_BITRATE_512K;
        media_info.video_freq[E_CHANNEL_VIDEO_SUB] = 90000;
    }

    // TODO: Configure audio when mic support is added
    // media_info.channel_enable[E_CHANNEL_AUDIO] = TRUE;
    // media_info.audio_codec[E_CHANNEL_AUDIO] = TUYA_CODEC_AUDIO_PCM;
    // media_info.audio_sample[E_CHANNEL_AUDIO] = TUYA_AUDIO_SAMPLE_8K;
    // media_info.audio_databits[E_CHANNEL_AUDIO] = TUYA_AUDIO_DATABITS_16;
    // media_info.audio_channel[E_CHANNEL_AUDIO] = TUYA_AUDIO_CHANNEL_MONO;
    // media_info.audio_fps[E_CHANNEL_AUDIO] = 25;

    SC_LOG_INFO("Tuya: starting services (main=%dx%d@%d sub=%s)",
                t->tcfg.main_width, t->tcfg.main_height, t->tcfg.main_fps,
                t->tcfg.sub_enabled ? "enabled" : "disabled");

    // int rc = tuya_ipc_start_sdk(&media_info);
    // if (rc != 0) { ... }

    // Open ring buffer for main stream
    // t->rb_main = tuya_ipc_ring_buffer_open(0, 0, E_IPC_STREAM_VIDEO_MAIN, RBUF_OPEN_TYPE_W);
    // if (!t->rb_main) { ... }

    t->status = TuyaStatus::ACTIVATED;
    SC_LOG_INFO("Tuya: services started, waiting for cloud connection...");
    return 0;
#else
    SC_LOG_INFO("Tuya [stub]: start() called -- no-op in stub mode");
    return 0;
#endif
}

void tuya_ipc_stop(TuyaIpc* t) {
    if (!t) return;

#ifdef SOULCAM_HAVE_TUYA
    SC_LOG_INFO("Tuya: stopping services (frames_pushed=%lu, events_sent=%lu)",
                t->frames_pushed, t->events_sent);

    // Close ring buffers
    // if (t->rb_main) tuya_ipc_ring_buffer_close(t->rb_main);
    // if (t->rb_sub) tuya_ipc_ring_buffer_close(t->rb_sub);

    // tuya_ipc_uninit_sdk();
#else
    SC_LOG_INFO("Tuya [stub]: stop() called (frames_pushed=%lu, events_sent=%lu)",
                t->frames_pushed, t->events_sent);
#endif

    delete t;
}

// ---------------------------------------------------------------------------
// Video frame feed
// ---------------------------------------------------------------------------

int tuya_push_video_frame(TuyaIpc* t, int channel, const TuyaVideoFrame& frame) {
    if (!t) return -1;

#ifdef SOULCAM_HAVE_TUYA
    MEDIA_FRAME_S mf = {};
    mf.type = (frame.type == TuyaFrameType::VIDEO_I_FRAME)
              ? E_VIDEO_I_FRAME : E_VIDEO_PB_FRAME;
    mf.p_buf = const_cast<uint8_t*>(frame.data);
    mf.size = frame.size;
    mf.pts = frame.pts;
    mf.timestamp = frame.utc_ms;

    Ring_Buffer_User_Handle_S rb = (channel == 0) ? t->rb_main : t->rb_sub;
    if (!rb) return -1;

    int rc = TUYA_APP_Put_Frame(rb, &mf);
    if (rc == 0) {
        t->frames_pushed++;
    }
    return rc;
#else
    // Stub: just count frames
    (void)channel;
    (void)frame;
    t->frames_pushed++;
    if (t->frames_pushed % 900 == 0) {  // Log every 30 seconds at 30fps
        SC_LOG_DEBUG("Tuya [stub]: %lu video frames pushed (would go to Tuya cloud)",
                     t->frames_pushed);
    }
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// AI event notification
// ---------------------------------------------------------------------------

int tuya_notify_event(TuyaIpc* t, TuyaEventType event,
                      const uint8_t* jpeg, uint32_t jpeg_sz) {
    if (!t) return -1;

#ifdef SOULCAM_HAVE_TUYA
    NOTIFICATION_UNIT_T unit = {};
    unit.data = const_cast<char*>(reinterpret_cast<const char*>(jpeg));
    unit.len = jpeg_sz;
    unit.type = NOTIFICATION_CONTENT_JPEG;

    NOTIFICATION_NAME_E name;
    switch (event) {
        case TuyaEventType::HUMAN_DETECT:  name = NOTIFICATION_NAME_HUMAN; break;
        case TuyaEventType::PET_DETECT:    name = NOTIFICATION_NAME_PCD;   break;
        case TuyaEventType::FACE_DETECT:   name = NOTIFICATION_NAME_FACE;  break;
        case TuyaEventType::MOTION_DETECT:
        default:                           name = NOTIFICATION_NAME_MOTION; break;
    }

    // int rc = tuya_ipc_notify_with_event(name, &unit, 1);
    int rc = 0;
    if (rc == 0) {
        t->events_sent++;
        SC_LOG_INFO("Tuya: event notification sent (type=%d, jpeg=%u bytes)",
                    (int)event, jpeg_sz);
    }
    return rc;
#else
    (void)event;
    (void)jpeg;
    (void)jpeg_sz;
    t->events_sent++;
    SC_LOG_DEBUG("Tuya [stub]: event notification (type=%d, jpeg=%u bytes) -- "
                 "would push to Tuya cloud", (int)event, jpeg_sz);
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Status & callbacks
// ---------------------------------------------------------------------------

TuyaStatus tuya_get_status(TuyaIpc* t) {
    if (!t) return TuyaStatus::UNREGISTERED;
    return t->status.load();
}

void tuya_set_status_callback(TuyaIpc* t, TuyaStatusCallback cb) {
    if (t) t->on_status = std::move(cb);
}

void tuya_set_stream_callback(TuyaIpc* t, TuyaStreamCallback cb) {
    if (t) t->on_stream = std::move(cb);
}

void tuya_set_dp_callback(TuyaIpc* t, TuyaDpCallback cb) {
    if (t) t->on_dp = std::move(cb);
}

}  // namespace sc
