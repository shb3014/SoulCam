#pragma once
// ============================================================================
// Tuya IPC SDK Adapter Layer
//
// Provides the interface between SoulCam's GStreamer pipeline and the
// Tuya IPC SDK.  This is a skeleton implementation that defines all the
// interfaces needed for full Tuya integration.
//
// Integration status:
//   - Interfaces defined and documented
//   - Stub implementations (no actual Tuya SDK linked yet)
//   - To activate: obtain TuyaOS IPC SDK for aarch64-linux, link
//     libtuya_ipc.a, and enable SOULCAM_HAVE_TUYA in CMakeLists.txt
//
// See doc/tuya/TUYA_INTEGRATION_PLAN.md for the full integration plan.
//
// Data flow:
//   SoulCam GStreamer pipeline (H.264 NAL units)
//     → tuya_push_video_frame()
//     → Tuya ring buffer
//     → P2P / Echo Show / Chromecast / Cloud Storage
//
//   SoulCam AI detections (person, pet, etc.)
//     → tuya_notify_event()
//     → Tuya notification system
//     → Push notification to Tuya Smart app
// ============================================================================

#include "soulcam.h"
#include <cstdint>
#include <vector>
#include <functional>

namespace sc {

// ---------------------------------------------------------------------------
// Tuya device credentials
// ---------------------------------------------------------------------------
struct TuyaCredentials {
    std::string product_id;     // PID from Tuya Developer Platform
    std::string uuid;           // Device UUID (unique per device)
    std::string auth_key;       // Device AuthKey
    std::string storage_path;   // Path for Tuya DB files (non-volatile)
    std::string firmware_ver;   // Firmware version string (x.y.z)
};

// ---------------------------------------------------------------------------
// Tuya IPC configuration
// ---------------------------------------------------------------------------
struct TuyaConfig {
    TuyaCredentials creds;

    // Main stream (HD, for P2P live preview and cloud storage)
    int  main_width      = 1280;
    int  main_height     = 960;
    int  main_fps        = 30;
    int  main_bitrate_kbps = 2048;  // Tuya max for cloud storage pricing

    // Sub stream (SD, for thumbnail/low-bandwidth preview)
    bool sub_enabled     = false;
    int  sub_width       = 640;
    int  sub_height      = 480;
    int  sub_fps         = 15;
    int  sub_bitrate_kbps = 512;

    // Audio (future)
    bool audio_enabled   = false;
};

// ---------------------------------------------------------------------------
// Video frame types (matches Tuya SDK MEDIA_FRAME_TYPE_E)
// ---------------------------------------------------------------------------
enum class TuyaFrameType {
    VIDEO_P_FRAME = 0,   // P-frame (predictive)
    VIDEO_I_FRAME = 1,   // I-frame (keyframe)
    AUDIO_FRAME   = 3,   // Audio frame
};

// ---------------------------------------------------------------------------
// Video frame to push to Tuya
// ---------------------------------------------------------------------------
struct TuyaVideoFrame {
    TuyaFrameType type;
    const uint8_t* data;     // H.264 NAL unit data
    uint32_t       size;     // Data size in bytes
    uint64_t       pts;      // Presentation timestamp (microseconds)
    uint64_t       utc_ms;   // UTC time in milliseconds
};

// ---------------------------------------------------------------------------
// AI event types (matches Tuya notification names)
// ---------------------------------------------------------------------------
enum class TuyaEventType {
    MOTION_DETECT  = 0,   // Generic motion
    HUMAN_DETECT   = 9,   // Person detected
    PET_DETECT     = 10,  // Pet (cat/dog) detected
    CAR_DETECT     = 11,  // Vehicle detected
    FACE_DETECT    = 14,  // Face detected
    BABY_CRY       = 12,  // Baby cry (audio, future)
    ABNORMAL_SOUND = 13,  // Abnormal sound (audio, future)
};

// ---------------------------------------------------------------------------
// Tuya device status
// ---------------------------------------------------------------------------
enum class TuyaStatus {
    UNREGISTERED = 0,
    REGISTERED   = 1,
    ACTIVATED    = 2,
    MQTT_ONLINE  = 3,
    MQTT_OFFLINE = 4,
};

// ---------------------------------------------------------------------------
// Callback types
// ---------------------------------------------------------------------------

// Called when Tuya SDK status changes
using TuyaStatusCallback = std::function<void(TuyaStatus status)>;

// Called when app requests live streaming start/stop
using TuyaStreamCallback = std::function<void(bool start, int channel)>;

// Called when app sends a DP (data point) command
using TuyaDpCallback = std::function<void(int dp_id, const std::string& value)>;

// ---------------------------------------------------------------------------
// Opaque handle
// ---------------------------------------------------------------------------
struct TuyaIpc;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Initialize the Tuya IPC SDK adapter.
// Returns nullptr if Tuya SDK is not available (stub mode).
TuyaIpc* tuya_ipc_init(const TuyaConfig& tcfg, const Config& cfg);

// Start Tuya services (P2P, cloud connection, etc.)
// Call after tuya_ipc_init() and after video pipeline is ready.
int tuya_ipc_start(TuyaIpc* t);

// Stop all Tuya services and clean up.
void tuya_ipc_stop(TuyaIpc* t);

// ---------------------------------------------------------------------------
// Video frame feed
// ---------------------------------------------------------------------------

// Push a single H.264 video frame to Tuya ring buffer.
// Called from GStreamer appsink callback in the main encoding pipeline.
// `channel`: 0 = main stream, 1 = sub stream
// Thread-safe.
int tuya_push_video_frame(TuyaIpc* t, int channel, const TuyaVideoFrame& frame);

// ---------------------------------------------------------------------------
// AI event notification
// ---------------------------------------------------------------------------

// Notify Tuya of an AI detection event.
// `event`:   event type (person, pet, etc.)
// `jpeg`:    JPEG snapshot data (for push notification thumbnail)
// `jpeg_sz`: JPEG data size
// Thread-safe.
int tuya_notify_event(TuyaIpc* t, TuyaEventType event,
                      const uint8_t* jpeg, uint32_t jpeg_sz);

// ---------------------------------------------------------------------------
// Status & callbacks
// ---------------------------------------------------------------------------

// Get current Tuya connection status.
TuyaStatus tuya_get_status(TuyaIpc* t);

// Register callbacks.
void tuya_set_status_callback(TuyaIpc* t, TuyaStatusCallback cb);
void tuya_set_stream_callback(TuyaIpc* t, TuyaStreamCallback cb);
void tuya_set_dp_callback(TuyaIpc* t, TuyaDpCallback cb);

}  // namespace sc
