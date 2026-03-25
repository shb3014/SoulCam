# SoulCam DP Catalog

All configuration and runtime state is managed through the **Store** system
(`src/store/`), which mirrors the Ivy `SystemStore` pattern.  
DPs are backed by typed `StateType = std::variant<uint32_t, int, float, bool, std::string>`.

Remote access: `setDp`/`getDp`/`getDpAll` over MQTT, plus `sysCmd(data=6)` for
the full DP metadata list (section 8.4 of `soullink_client.md`).

---

## Persist DPs — Numeric (saved to `/var/lib/soulcam/store.json`)

| DP | Name | Type | Default | Description |
|---:|------|------|--------:|-------------|
| 1 | `stream_width` | u32 | 1280 | ISP main-path capture width in px. |
| 2 | `stream_height` | u32 | 960 | ISP main-path capture height in px. |
| 3 | `stream_fps` | u32 | 30 | V4L2 capture framerate. Also derives RTSP GOP (1 keyframe/sec). |
| 4 | `rtsp_bitrate` | u32 | 4000 | H.264 target bitrate in kbps. |
| 5 | `rtsp_port` | u32 | 8554 | TCP port the RTSP server listens on. |
| 6 | `enable_ai` | bool | true | Master switch for AI inference pipeline (selfpath + RKNN). |
| 7 | `enable_overlay` | bool | false | Draw bounding-box overlays on RTSP stream (requires AI). |
| 8 | `ai_conf_threshold` | float | 0.25 | Minimum detection confidence score for primary model. |
| 9 | `enable_soullink` | bool | true | Start the SoulLink module (mDNS + MQTT + SoulCmd). |
| 10 | `enable_onvif` | bool | false | Start ONVIF metadata stream + WS-Discovery device service. |
| 11 | `verbose` | bool | false | Set log level to DEBUG. |
| 12 | `model2_conf` | float | 0.25 | Confidence threshold for second model slot. |
| 13 | `adaptive_tracking` | bool | false | Enable adaptive hand/person tracking policy. Implies weighted scheduler. |
| 14 | `weighted_scheduler` | bool | false | Enable weighted round-robin model scheduler (vs run-all). |
| 15 | `max_models_per_frame` | u32 | 1 | Max model slots to run per frame in weighted scheduler mode. |
| 16 | `enable_rive` | bool | false | Enable GPU-rendered Rive animation on DRM/KMS display. |
| 17 | `rive_resolution` | u32 | 500 | Rive render resolution in px (square). Lower = faster. |
| 18 | `tracker_yolo_interval` | u32 | 1 | Run YOLO every N frames; lightweight tracker on others. 1 = tracker disabled. |
| 19 | `tracker_enable_mosse` | bool | true | Enable KCF visual correlation (false = Kalman-only). |
| 20 | `tracker_mosse_psr` | float | 7.0 | PSR threshold below which KCF falls back to Kalman. |
| 21 | `tracker_mosse_learn_rate` | float | 0.125 | KCF filter adaptation rate (0..1). |
| 22 | `tracker_mosse_patch_size` | u32 | 64 | KCF ROI size (power of 2, 16..256). |
| 23 | `tracker_roi_padding` | float | 2.0 | Search region = bbox × this factor (≥1.0). |
| 24 | `tracker_smooth_factor` | float | 0.6 | EMA smoothing alpha for YOLO re-anchor (0=none, 1=snap). |
| 25 | `tracker_adaptive_interval` | bool | false | Enable adaptive YOLO scheduling (vs fixed interval). |
| 26 | `tracker_max_skip` | u32 | 8 | Max consecutive tracker-only frames before forced YOLO. |
| 27 | `tracker_min_skip` | u32 | 2 | Min frames between YOLO runs (even when urgent). |
| 28 | `tracker_hand_confirm` | u32 | 3 | YOLO frames with hand detection to switch to HandPreferred. |
| 29 | `tracker_hand_lost` | u32 | 5 | YOLO frames without hand to fallback to PersonFallback. |
| 30 | `ai_target_fps` | u32 | 0 | AI pipeline target FPS. 0 = unlimited (process as fast as possible). >0 = drop frames to cap output rate. Takes effect immediately. |

## Persist DPs — String (offset 100+)

| DP | Name | Type | Default | Description |
|---:|------|------|---------|-------------|
| 101 | `rtsp_mount` | string | `"/cam"` | RTSP URL mount point (e.g. `rtsp://device:8554/cam`). |
| 102 | `ai_model_path` | string | `""` | Filesystem path to the primary RKNN model (slot 0). |
| 103 | `ai_labels` | string | `""` | Comma-separated class labels (e.g. `"person,car,dog"`). |
| 104 | `soullink_sync_root` | string | `"/home/ubuntu/SoulCam"` | Root directory for `syncFiles` protocol. |
| 105 | `model2_path` | string | `""` | Filesystem path to second RKNN model (slot 1). Empty = no model 2. |
| 106 | `rive_file` | string | `""` | Filesystem path to .riv animation file. Empty = no animation. |
| 107 | `rive_target` | string | `"person"` | Detection label to track (e.g. `"person"`, `"hand"`). |
| 108 | `model2_labels` | string | `""` | Comma-separated class labels for model 2 (e.g. `"hand"`). |

## RAM DPs — Runtime only (not persisted)

| DP | Name | Type | Default | Description |
|---:|------|------|---------|-------------|
| 1001 | `rtsp_online` | bool | false | RTSP server reachable (updated by health heartbeat). |
| 1002 | `stream_subscribed` | bool | false | SoulFlow has an active `subStream` subscription. |
| 1003 | `module_ready` | bool | false | Composite: `rtsp_online && frame_receiver_started`. |

## Persist DPs — Perception Pipeline

| DP | Name | Type | Default | Description |
|---:|------|------|--------:|-------------|
| 31 | `enable_perception` | bool | false | Enable cascading perception pipeline (multi-object recognition + tracking). |
| 32 | `perception_max_tracked` | u32 | 5 | Max simultaneous KCF tracker slots (top-K by interest). |
| 33 | `perception_embed_dim` | u32 | 128 | Embedding vector dimensionality. |
| 34 | `perception_embed_input` | u32 | 128 | Embedding model square input size (px). |
| 35 | `perception_hot_tier_max` | u32 | 1000 | Max objects in hot-tier RAM before cold-tier demotion. |
| 36 | `perception_interest_novelty_hl` | float | 24.0 | Novelty decay half-life in hours. |
| 37 | `perception_interest_motion_w` | float | 0.15 | Motion weight in interest scoring formula. |
| 38 | `perception_interest_threshold` | float | 0.10 | Min interest to keep a track alive. |
| 39 | `perception_enrollment_delay` | u32 | 5 | Frames an object must be stable before enrollment. |
| 40 | `perception_vlm_enabled` | bool | false | Enable async VLM API enrichment for enrolled objects. |

## Persist DPs — Perception Pipeline (String, offset 100+)

| DP | Name | Type | Default | Description |
|---:|------|------|---------|-------------|
| 109 | `perception_embedder_model` | string | `""` | RKNN embedding model path. Empty = stub mode. |
| 110 | `perception_memory_dir` | string | `"/var/lib/soulcam/memory"` | On-disk directory for object memory bank. |
| 111 | `perception_vlm_api_url` | string | `""` | VLM API endpoint URL (e.g. OpenAI chat/completions). |
| 112 | `perception_vlm_api_key` | string | `""` | VLM API authentication key. |
| 113 | `perception_vlm_model` | string | `"gpt-4o"` | VLM model name passed in API request. |

**Total: 52 DPs** (36 numeric persist + 13 string persist + 3 RAM)

---

## SoulLink command behavior

### DP commands (existing)

- **`setDp`** (`cmd=0`): Type-safe update via `Store::setFromJson()`. Persist DPs auto-save.
- **`getDp`** (`cmd=1`): Read specific DPs from Store.
- **`getDpAll`** (`cmd=2`): Read all DPs.

### sysCmd sub-commands (`cmd=21`)

| subcmd | Name | data format | Description |
|-------:|------|-------------|-------------|
| 1 | Restart | `1` or `{"subcmd":1}` | Restart the soulcam systemd service. Responds before restarting. |
| 6 | DP info | `6` (number) | Returns full DP metadata list (`id=2`, `dpList`) on `m/` topic. |
| 12 | RTSP info | `12` (number) | Returns RTSP endpoint/health snapshot (`id=3`) on `m/` topic. |
| 7 | Model swap | `{"subcmd":7, "slot":0, "path":"...", "conf":0.3}` | Hot-swap a model in a running slot. |
| 8 | Model add | `{"subcmd":8, "path":"...", "conf":0.3, "skip":0, "weight":1}` | Add a new model slot at runtime. |
| 9 | Model remove | `{"subcmd":9, "slot":1}` | Remove a model slot (slot > 0 only). |
| 10 | Model enable | `{"subcmd":10, "slot":1, "enable":true}` | Enable/disable a model slot. |
| 11 | Model list | `{"subcmd":11}` | Returns all model slots with path, conf, weight, enabled status. |

**Example (SoulFlow Cmd node):**

```json
{"cmd": 21, "data": {"subcmd": 7, "slot": 1, "path": "/home/ubuntu/models/hand.rknn", "conf": 0.10}}
```

Responses are published on the `m/` topic as notifications (`id=0`).
Model list (subcmd 11) returns slot details in `message.data`.

RTSP info (subcmd 12) returns:

```json
{
  "id": 3,
  "message": {
    "url": "rtsp://192.168.1.45:8554/cam",
    "host": "192.168.1.45",
    "port": 8554,
    "mount": "/cam",
    "online": true,
    "streamSubscribed": false,
    "moduleReady": true
  }
}
```

AI detections are now streamed as structured message packets:

```json
{
  "id": 4,
  "message": {
    "schema": "soulcam.aiDetections.v1",
    "tsMs": 1710400000000,
    "frame": { "width": 640, "height": 480 },
    "tracking": { "mode": "hand_target", "enabled": true },
    "rawCount": 8,
    "count": 1,
    "objects": [
      {
        "model": 1,
        "clsId": 0,
        "label": "hand",
        "conf": 0.934,
        "box": { "left": 118, "top": 96, "right": 262, "bottom": 284 },
        "center": { "x": 190, "y": 190 }
      }
    ]
  }
}
```

---

## Configuration flow

### Startup

1. `Store::initialize()` — allocate cache with defaults
2. `Store::load()` — read `/var/lib/soulcam/store.json`
3. `config_to_store()` — only explicit CLI args override persisted values
4. `Store::save()` — persist if CLI overrides were applied
5. `store_to_config()` — rebuild `sc::Config` from Store for pipeline init

### Runtime via SoulFlow

- **Persistent config** (DPs): Use `setDp` to change values like `model2_path`,
  `adaptive_tracking`, `ai_conf_threshold`. Changes persist and take effect on
  next restart.
- **Immediate model operations**: Use `sysCmd` subcmds 7-11 to hot-swap, add,
  remove, or enable/disable model slots without restarting.

### Example: configure adaptive hand tracking from SoulFlow

```
1. setDp: model2_path = "/home/ubuntu/models/hand.rknn"
2. setDp: model2_conf = 0.10
3. setDp: adaptive_tracking = true
4. setDp: max_models_per_frame = 1
5. sysCmd: 1   (restart to apply)
```

Or for immediate model swap:

```
1. sysCmd: {"subcmd": 8, "path": "/home/ubuntu/models/hand.rknn", "conf": 0.10}
2. sysCmd: {"subcmd": 10, "slot": 1, "enable": true}
```

### Example: enable Rive animation from SoulFlow

```
1. setDp: rive_file = "/home/ubuntu/SoulCam/rive-runtime/demos/rk3566_player/riv/avatar.riv"
2. setDp: rive_resolution = 500
3. setDp: rive_target = "person"
4. setDp: enable_rive = true
```

All four DPs persist and take effect immediately (no restart needed).
The Rive renderer runs on a dedicated GPU thread (Mali-G52) and does not
affect RTSP streaming or AI inference performance.

### Example: enable interframe tracker from SoulFlow

```
1. setDp: tracker_yolo_interval = 4
2. setDp: tracker_enable_mosse = true
3. setDp: tracker_mosse_psr = 7.0
4. setDp: tracker_smooth_factor = 0.6
5. setDp: tracker_adaptive_interval = true
6. setDp: tracker_max_skip = 8
7. setDp: tracker_min_skip = 2
```

All tracker DPs take effect immediately (no restart needed).
Setting `tracker_yolo_interval = 1` disables the tracker.
When `tracker_adaptive_interval = true`, the scheduler dynamically decides
when to run YOLO based on KCF PSR, velocity, and last YOLO confidence,
bounded by `tracker_min_skip` and `tracker_max_skip`.

### Example: enable perception pipeline from SoulFlow

```
1. setDp: enable_perception = true
2. setDp: perception_max_tracked = 3
3. setDp: perception_embedder_model = "/home/ubuntu/models/mobilenetv3_embed.rknn"
4. setDp: perception_memory_dir = "/var/lib/soulcam/memory"
5. sysCmd: 1   (restart to apply)
```

To add VLM enrichment:

```
1. setDp: perception_vlm_enabled = true
2. setDp: perception_vlm_api_url = "https://api.openai.com/v1/chat/completions"
3. setDp: perception_vlm_api_key = "sk-..."
4. setDp: perception_vlm_model = "gpt-4o"
5. sysCmd: 1   (restart to apply)
```

To tune interest scoring (no restart needed if future hot-reload is added):

```
1. setDp: perception_interest_novelty_hl = 12.0
2. setDp: perception_interest_motion_w = 0.25
3. setDp: perception_interest_threshold = 0.05
4. setDp: perception_enrollment_delay = 3
5. sysCmd: 1   (restart to apply)
```
