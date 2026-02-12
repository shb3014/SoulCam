# SoulCam × Tuya IPC SDK — Integration Plan

_Created: 2026-02-12_

---

## Goal

Connect SoulCam to the **Tuya IoT Cloud** to enable:
- **Amazon Alexa** (Echo Show) live streaming
- **Google Assistant** (Chromecast) live streaming
- **Apple HomeKit** streaming (via Tuya HomeKit bridge)
- **Tuya Smart / Smart Life** app: P2P live preview, playback, cloud storage
- Push notifications on AI events (person detection, etc.)

## Why Not ONVIF?

ONVIF is a protocol for **local NVR/VMS** discovery and control (Blue Iris,
Milestone, Genetec, etc.) on a LAN. SoulCam's primary use case is **cloud-based
smart home integration** via Tuya, which has its own protocol stack:

| Feature | ONVIF | Tuya IPC SDK |
|---------|-------|-------------|
| Alexa Echo Show | No | **Built-in** (`TUYA_ECHOSHOW_*`) |
| Google Chromecast | No | **Built-in** (`TUYA_CHROMECAST_*`) |
| HomeKit | No | **Tuya HomeKit bridge** |
| Mobile app | No | **Tuya Smart / Smart Life** P2P |
| Cloud storage | No | **Built-in** (event + continuous) |
| AI event notifications | No | **Built-in** (`tuya_ipc_notify_*`) |
| Local NVR discovery | **Yes** | Optional ONVIF module |

The existing ONVIF code (`onvif_device.cpp`, `onvif_metadata.cpp`) is retained
as an optional feature for local NVR compatibility.

---

## Architecture

### Current (RTSP only)

```
ISP mainpath (NV12 1280×960)
  → MPP H.264 encode (HW)
  → GstRtspServer (:8554/cam)

ISP selfpath (NV12 640×480)
  → RGA NV12→RGB (HW)
  → RKNN YOLOv8 (NPU)
  → Scene Hub (JSON)
```

### Target (RTSP + Tuya)

```
ISP mainpath (NV12 1280×960)
  → MPP H.264 encode (HW)
  → tee
    ├─→ GstRtspServer (:8554/cam)    [local RTSP, kept for debugging]
    └─→ appsink → Tuya ring buffer   [cloud streaming]
                    ├─→ P2P (Tuya Smart app)
                    ├─→ Echo Show
                    ├─→ Chromecast
                    ├─→ Cloud storage
                    └─→ HomeKit bridge

ISP selfpath (NV12 640×480)
  → RGA NV12→RGB (HW)
  → RKNN YOLOv8 (NPU)
  ├─→ Scene Hub (JSON, local)
  └─→ Tuya event notification (JPEG snapshot + push)
```

### Key Integration Points

1. **H.264 Frame Feed**: Tap encoded H.264 NAL units from GStreamer pipeline
   → push into Tuya ring buffer via `TUYA_APP_Put_Frame()`
2. **JPEG Snapshot**: Capture single NV12 frame → JPEG encode → attach to
   Tuya event notifications (`NOTIFICATION_CONTENT_JPEG`)
3. **Device Control**: Map Tuya DP (data point) commands to SoulCam controls
   (e.g., resolution switch, AI on/off, model swap)
4. **AI Events**: Bridge YOLOv8 detections to Tuya notification system
   (`EVENT_TYPE_MOTION_DETECT`, `EVENT_TYPE_HUMAN_DETECT`)

---

## Tuya IPC SDK Overview

### SDK Structure

```
tuya_ipc_sdk/
├── sdk/
│   ├── include/
│   │   ├── tuya_ipc_api.h          # Core initialization
│   │   ├── tuya_ipc_media.h        # Media info, ring buffer
│   │   ├── tuya_ipc_p2p.h          # P2P live streaming
│   │   ├── tuya_ipc_cloud_storage.h # Cloud storage
│   │   ├── tuya_ipc_stream_storage.h # Local SD recording
│   │   ├── tuya_ring_buffer.h      # Ring buffer API
│   │   └── ...
│   └── libs/
│       └── libtuya_ipc.a           # Static library (platform-specific)
└── ...
```

### Core APIs

```c
// 1. Initialize SDK
TUYA_IPC_ENV_VAR_S env;
env.product_key = "PID";
env.uuid = "UUID";
env.auth_key = "AUTHKEY";
env.storage_path = "/var/lib/soulcam/tuya/";
tuya_ipc_init_sdk(&env);

// 2. Configure media info
IPC_MEDIA_INFO_S media_info;
media_info.channel_enable[E_CHANNEL_VIDEO_MAIN] = TRUE;
media_info.video_codec[E_CHANNEL_VIDEO_MAIN] = TUYA_CODEC_VIDEO_H264;
media_info.video_width[E_CHANNEL_VIDEO_MAIN] = 1280;
media_info.video_height[E_CHANNEL_VIDEO_MAIN] = 960;
media_info.video_fps[E_CHANNEL_VIDEO_MAIN] = 30;
media_info.video_bitrate[E_CHANNEL_VIDEO_MAIN] = TUYA_VIDEO_BITRATE_2M;

// 3. Start SDK services
tuya_ipc_start_sdk(&media_info);

// 4. Open ring buffer
Ring_Buffer_User_Handle_S handle = tuya_ipc_ring_buffer_open(
    0, 0, E_IPC_STREAM_VIDEO_MAIN, RBUF_OPEN_TYPE_W);

// 5. Push H.264 frames continuously
MEDIA_FRAME_S frame;
frame.type = (is_keyframe ? E_VIDEO_I_FRAME : E_VIDEO_PB_FRAME);
frame.p_buf = h264_nal_data;
frame.size = h264_nal_size;
frame.pts = timestamp_us;
frame.timestamp = utc_ms;
TUYA_APP_Put_Frame(handle, &frame);

// 6. Push AI events
NOTIFICATION_UNIT_T unit;
unit.data = jpeg_buffer;
unit.len = jpeg_size;
unit.type = NOTIFICATION_CONTENT_JPEG;
tuya_ipc_notify_with_event(NOTIFICATION_NAME_HUMAN, &unit, 1);
```

### Supported Video Codecs

| Codec | Enum | Notes |
|-------|------|-------|
| H.264 | `TUYA_CODEC_VIDEO_H264` | **Primary**, SoulCam uses this |
| H.265 | `TUYA_CODEC_VIDEO_H265` | Supported, future option |
| MJPEG | `TUYA_CODEC_VIDEO_MJPEG` | For low-power devices |

### Stream Channels

| Channel | Purpose | SoulCam Mapping |
|---------|---------|----------------|
| `E_CHANNEL_VIDEO_MAIN` | HD stream (P2P, cloud) | Mainpath 1280×960 |
| `E_CHANNEL_VIDEO_SUB` | SD stream (thumbnail) | Could use selfpath 640×480 |
| `E_CHANNEL_AUDIO` | Audio | Future (mic input) |

---

## Implementation Phases

### Phase 1: Foundation (current — no SDK needed)

- [x] **JPEG snapshot endpoint**: HTTP `GET /snapshot` on port 8088
- [x] **Tuya adapter skeleton**: `pipeline/tuya_ipc.h/.cpp` with interfaces
- [x] **Framework doc updated**: Next steps reflect Tuya direction
- [x] **Integration plan**: This document

### Phase 2: Tuya Cloud Setup (user action)

1. **Register** on [Tuya Developer Platform](https://platform.tuya.com)
2. **Create product**: Smart Camera → IP Camera → TuyaOS → Custom Solution
3. **Select functions**: Live streaming, Motion detection, Cloud storage
4. **Get credentials**: Download PID, UUID, AuthKey from Hardware Development
5. **Get SDK**: Download TuyaOS IPC SDK (request aarch64-linux from support
   if not available in standard downloads)
6. **Get pairing token**: Via Tuya Smart app QR code scan

### Phase 3: SDK Integration

1. **Link SDK**: Add `libtuya_ipc.a` to CMakeLists.txt
2. **Initialize**: `tuya_ipc_init_sdk()` with PID/UUID/AuthKey
3. **Device pairing**: Implement wired mode (Ethernet LAN discovery)
4. **H.264 frame tap**: Add GStreamer tee + appsink on mainpath pipeline
5. **Ring buffer feed**: Push H.264 NAL units to Tuya ring buffer
6. **Test P2P**: Verify live streaming in Tuya Smart app

### Phase 4: Smart Home Integration

1. **Echo Show**: Register `TUYA_ECHOSHOW_CALLBACK`, test with Alexa
2. **Chromecast**: Register `TUYA_CHROMECAST_CALLBACK`, test with Google Home
3. **HomeKit**: Enable Tuya HomeKit bridge if supported by product
4. **AI events**: Bridge YOLOv8 detections to Tuya event notifications
5. **Cloud storage**: Enable event-based recording with JPEG snapshots

### Phase 5: Production Hardening

1. **OTA**: Implement firmware update callback
2. **Audio**: Add mic input → PCM/G.711 → Tuya audio ring buffer
3. **DP commands**: Handle device control from app (resolution, AI, etc.)
4. **Watchdog**: Auto-restart on Tuya SDK connection failure

---

## GStreamer Pipeline Refactoring (Phase 3)

### Current RTSP pipeline (launch string)

```
v4l2src device=/dev/video8 io-mode=2 do-timestamp=true
  ! video/x-raw,format=NV12,width=1280,height=960,framerate=30/1
  ! queue leaky=downstream max-size-buffers=3
  ! mpph264enc bps=4000000 gop=30
  ! h264parse config-interval=1
  ! rtph264pay name=pay0 pt=96 config-interval=1
```

### Proposed refactored pipeline

Instead of modifying the RTSP factory launch string (which is
media-per-session), create a **standalone encoding pipeline** that feeds
both RTSP and Tuya:

```
v4l2src device=/dev/video8 io-mode=2 do-timestamp=true
  ! video/x-raw,format=NV12,width=1280,height=960,framerate=30/1
  ! queue leaky=downstream max-size-buffers=3
  ! mpph264enc bps=4000000 gop=30
  ! h264parse config-interval=1
  ! tee name=t
    t. ! queue ! appsink name=tuya_sink   ← Tuya ring buffer feed
    t. ! queue ! rtph264pay name=pay0     ← RTSP server (could be appsrc-based)
```

**Note**: This requires refactoring from GstRtspMediaFactory launch string
to a custom pipeline. The RTSP server would use `appsrc` to receive encoded
H.264 from the shared pipeline. This ensures both RTSP and Tuya receive
frames even when no RTSP client is connected (important for cloud storage
and always-on P2P availability).

---

## File Changes Summary

### New files

| File | Purpose |
|------|---------|
| `doc/tuya/TUYA_INTEGRATION_PLAN.md` | This document |
| `src/pipeline/snapshot.h` | JPEG snapshot API |
| `src/pipeline/snapshot.cpp` | JPEG snapshot implementation |
| `src/pipeline/tuya_ipc.h` | Tuya adapter interfaces |
| `src/pipeline/tuya_ipc.cpp` | Tuya adapter skeleton |

### Modified files

| File | Change |
|------|--------|
| `doc/framework/SOULCAM_FRAMEWORK.md` | Updated Next Steps, ONVIF deprioritized |
| `src/CMakeLists.txt` | Added snapshot + tuya_ipc sources |
| `src/soulcam.h` | Added snapshot + Tuya config fields |
| `src/main.cpp` | Added `--snapshot` flag, snapshot endpoint, Tuya init |

---

## References

- [Tuya Developer Platform](https://developer.tuya.com/en)
- [TuyaOS IPC SDK APIs](https://developer.tuya.com/en/docs/iot-device-dev/sdk-api-interface-description-doc?id=K95019j0wensz)
- [TuyaOS 5.x SDK Development Guide](https://developer.tuya.com/en/docs/iot-device-dev/IPC_SDK?id=Kaqe10hg0htn5)
- [Tuya IPC SDK Demo (GitHub)](https://github.com/tuya/tuya-iotos-embeded-multimedia-demo)
- [Stream to HomeKit (TuyaOS)](https://developer.tuya.com/en/docs/iot-device-dev/tuyaos-package-ipc-device?id=Kcoxn17qoorbp)
