# SoulCam C++ Framework — Architecture & Build Guide

_Board: Vicharak (RK3566) · Camera: RPi Camera (G) (OV5647 160° fisheye)_
_Created: 2026-02-09_

---

## Overview

SoulCam is a C++ IP camera framework for RK3566 with AI capabilities.
It replaces the earlier Python/GStreamer prototype with a single-binary,
fully hardware-accelerated pipeline.

### Architecture

```
┌──────────────────────────────────────────────┐
│            ISP (rkisp v21, RKAIQ 3A)         │
│  OV5647 → CSI → ISP Processing (AWB,AE,CCM) │
├────────────────┬─────────────────────────────┤
│  mainpath      │  selfpath                   │
│  /dev/video8   │  /dev/video9                │
│  1280x960 NV12 │  640x480 NV12               │
└────────┬───────┴────────────┬────────────────┘
         │                    │
         ▼                    ▼
  ┌──────────────┐    ┌──────────────┐
  │ MPP (HW)     │    │ RGA 2D (HW)  │
  │ NV12 → H.264 │    │ NV12 → RGB   │
  └──────┬───────┘    └──────┬───────┘
         │                   │
         ▼                   ▼
  ┌──────────────┐    ┌──────────────┐
  │ RTSP Server  │    │ RKNN NPU     │
  │ :8554/cam    │    │ YOLOv8       │
  └──────────────┘    └──────┬───────┘
                             │
                             ▼
                      ┌──────────────┐
                      │ Scene Hub    │
                      │ (detections) │
                      └──────────────┘
```

### Why dual-path ISP?

| Feature | Mainpath (/dev/video8) | Selfpath (/dev/video9) |
|---------|----------------------|----------------------|
| Resolution | Up to full sensor (2592×1944) | Independent, up to ~2048px wide |
| Scaler | ISP HW scaler | ISP HW scaler (independent) |
| Format | UYVY, NV12, etc. | UYVY, NV12, etc. |
| Best for | IPC/RTSP stream | AI inference |

Both paths share the same ISP processing pipeline (3A, AWB, CCM, etc.)
and run simultaneously with **zero additional CPU/RGA cost** — the ISP
hardware does the splitting and independent scaling.

### HW-accelerated pipeline (zero CPU pixel work)

```
ISP(NV12) →[DMA]→ MPP(NV12→H.264) →[CPU]→ RTP → RTSP
```

NV12 goes directly from the ISP to the MPP encoder — no RGA conversion
needed. The only CPU work is RTP payloading (lightweight memcpy) and RTSP
signaling (TCP session management).

**Verified: 0.0% CPU usage** at 1280×960 @ 30fps, ~4 Mbps H.264 High.

---

## Source Layout

```
src/
├── CMakeLists.txt              # Build (GStreamer + RKNN + optional OpenCV)
├── soulcam.h                   # Config types (IspDevices, StreamConfig, etc.)
├── main.cpp                    # Entry point, CLI args, orchestration, control socket
├── pipeline/
│   ├── isp_config.h/.cpp       # ISP dual-path setup (media-ctl + v4l2-ctl)
│   ├── rtsp_server.h/.cpp      # GstRtspServer (HW: RGA + MPP)
│   ├── ai_capture.h/.cpp       # Selfpath capture → appsink → multi-model RKNN
│   ├── overlay.h/.cpp          # Cairo detection overlay on RTSP stream
│   ├── onvif_metadata.h/.cpp   # ONVIF analytics metadata RTSP stream
│   ├── onvif_device.h/.cpp     # ONVIF device service (WS-Discovery + SOAP)
│   ├── snapshot.h/.cpp         # JPEG snapshot HTTP endpoint
│   └── tuya_ipc.h/.cpp         # Tuya IPC SDK adapter (stub when no SDK)
├── ai/
│   ├── detector.h/.cpp         # RKNN model wrapper (with stub for x86)
│   └── model_pipeline.h/.cpp   # Multi-model orchestrator (N models on same frame)
└── util/
    └── logger.h/.cpp           # Timestamped logging
```

---

## Build

### Prerequisites (install once)

```bash
sudo apt install -y \
    build-essential cmake pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstrtspserver-1.0-dev \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-rtsp
```

### Build on device

```bash
cd ~/SoulCam
bash scripts/build.sh           # Release
bash scripts/build.sh debug     # Debug (verbose GStreamer)
bash scripts/build.sh clean     # Clean + rebuild
```

### Deploy from WSL

```bash
./scripts/deploy.sh 192.168.1.45 ubuntu
```

---

## Usage

### RTSP only (default)

```bash
./build/soulcam
```

### RTSP + AI

```bash
./build/soulcam --ai --model rk3566/yolov8n.rknn
```

### RTSP + ONVIF device service (NVR discovery)

```bash
./build/soulcam --onvif-device
```

### DMA-BUF zero-copy mode

```bash
./build/soulcam --dmabuf
./build/soulcam --dmabuf --ai --model rk3566/yolov8n.rknn
```

### Full features (AI + Overlay + ONVIF metadata + ONVIF device service)

```bash
./build/soulcam --ai --overlay --onvif --onvif-device --model rk3566/yolov8n.rknn
```

### Multi-model pipeline (multiple RKNN models on same frame)

```bash
# Two models: yolov8n (every frame) + yolov8s (every 3rd frame)
./build/soulcam --ai \
    --model rk3566/yolov8n.rknn \
    --model2 rk3566/yolov8s.rknn --model2-skip 2

# Three models: fast + accurate + heavy (different skip rates)
./build/soulcam --ai \
    --model rk3566/yolov8n.rknn \
    --model2 rk3566/yolov8s.rknn --model2-skip 2 \
    --model3 rk3566/yolov8m.rknn --model3-skip 4
```

### Custom resolution / bitrate

```bash
./build/soulcam --width 640 --height 480 --fps 30 --bitrate 2000
```

### Full options

```
./build/soulcam -h

Stream options:
  --width W          Stream width       (default: 1280)
  --height H         Stream height      (default: 960)
  --fps F            Framerate           (default: 30)
  --bitrate K        H.264 bitrate kbps  (default: 4000)
  --port P           RTSP port           (default: 8554)
  --mount M          RTSP mount point    (default: /cam)
  --encoder E        mpp|x264            (default: mpp)

Sensor options:
  --sensor-width W   Sensor mode width   (default: 1296)
  --sensor-height H  Sensor mode height  (default: 972)

AI options:
  --ai               Enable AI on selfpath
  --overlay          Draw detection boxes on RTSP stream (requires --ai)
  --onvif            Enable ONVIF metadata stream on port+1 (requires --ai)
  --model PATH       Primary RKNN model (slot 0)
  --ai-width W       AI capture width    (default: 640)
  --ai-height H      AI capture height   (default: 480)
  --ai-fps F         AI capture FPS      (default: 30)
  --conf-thresh F    Detection confidence (default: 0.25)
  --nms-thresh F     NMS threshold       (default: 0.45)

Multi-model pipeline:
  --model2 PATH      Second model (slot 1)
  --model2-skip N    Run model 2 every N+1 frames (default: 0)
  --model2-conf F    Model 2 confidence threshold
  --model3 PATH      Third model (slot 2)
  --model3-skip N    Run model 3 every N+1 frames (default: 0)
  --model3-conf F    Model 3 confidence threshold

ONVIF device service:
  --onvif-device     Enable ONVIF device service (WS-Discovery + SOAP)
  --onvif-port P     ONVIF HTTP port (default: 8080)

Device options:
  --mainpath DEV     Mainpath device     (default: /dev/video8)
  --selfpath DEV     Selfpath device     (default: /dev/video9)
  --media DEV        Media device        (default: /dev/media1)

Performance options:
  --dmabuf           Use DMA-BUF io-mode (zero-copy ISP→RGA→MPP)

Runtime control:
  --ctrl-sock PATH   Control socket      (default: /tmp/soulcam_ctrl.sock)

General:
  -v, --verbose      Verbose logging
```

---

## Testing

### From PC/WSL

```bash
# Quick validation
ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.45:8554/cam -t 8 -f null -

# Stream info
ffprobe -rtsp_transport tcp -v quiet -print_format json \
    -show_streams rtsp://192.168.1.45:8554/cam

# View live
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay \
    -framedrop rtsp://192.168.1.45:8554/cam

# Record 10s clip
ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.45:8554/cam \
    -t 10 -c copy soulcam_clip.mp4

# Use test script
./scripts/test_rtsp.sh 192.168.1.45 check
./scripts/test_rtsp.sh 192.168.1.45 play
./scripts/test_rtsp.sh 192.168.1.45 probe
./scripts/test_rtsp.sh 192.168.1.45 record
```

### On device

```bash
# Check process
pgrep -a soulcam

# Check RTSP port
ss -lntp | grep 8554

# CPU usage (should be ~0% for HW pipeline)
top -bn1 -p $(pgrep soulcam) | tail -2

# RGA activity (interrupt count should increase)
cat /proc/interrupts | grep rga

# Soulcam log
tail -f /tmp/soulcam.log
```

---

## Verification Results (updated 2026-02-11)

| Metric | Result |
|--------|--------|
| Resolution | 1280×960 |
| Framerate | 30 fps |
| Codec | H.264 High Profile, Level 4.0 |
| Bitrate | ~3.8 Mbps (target: 4.0 Mbps) |
| CPU usage (RTSP only) | **0.0%** (ISP NV12 direct to MPP, no RGA needed) |
| CPU usage (RTSP+AI) | **~11%** (NEON-optimized post-processing) |
| Memory | ~27 MB (RTSP) / ~37 MB (RTSP+AI) |
| AI fps | **~22.7** (YOLOv8n INT8, 640×640) |
| Pipeline | `v4l2src(NV12) → mpph264enc(HW) → RTSP` |
| Startup time | ~200ms (ISP config) + instant RTSP |

### Comparison vs old Python pipeline

| Metric | Old (Python) | New (C++) |
|--------|-------------|-----------|
| CPU usage | ~15-25% | **0.0%** |
| Memory | ~80 MB (3 Python processes) | **~27 MB** (single binary) |
| Latency | ~200ms (shm hop) | **<100ms** (direct pipeline) |
| Processes | 3 (capture + RTSP + optional overlay) | **1** |
| Software conversion | videoconvert (SW) | **None** (all HW) |

---

## Dependencies on Device

| Package | Version | Purpose |
|---------|---------|---------|
| `gstreamer1.0-rockchip1` | 1.14-4 | `mpph264enc` (MPP HW encoder) |
| `libgstrtspserver-1.0-dev` | 1.24.2 | GstRtspServer C API |
| `libgstreamer1.0-dev` | 1.24.2 | GStreamer core C API |
| `libgstreamer-plugins-base1.0-dev` | 1.24.1 | Base plugins + video |
| `rgaconvert` plugin | custom build | NV12→RGB RGA conversion (AI + overlay paths) |
| `librga2` | 2.2.0 | RGA 2D engine userspace |
| `librknnrt.so` | (from rknn/) | RKNN NPU runtime |
| `cmake` | 3.28.3 | Build system |

### GStreamer plugin locations

```
System:  /usr/lib/aarch64-linux-gnu/gstreamer-1.0/
Custom:  /usr/local/lib/aarch64-linux-gnu/gstreamer-1.0/  (rgaconvert)
```

---

## AI Pipeline (verified 2026-02-09)

ISP selfpath outputs NV12 directly. RGA converts NV12→RGB for RKNN:

```
selfpath (/dev/video9)
  NV12 640×480 @ 30fps
    → rgaconvert (HW: NV12→RGB + resize 640×640)    ← RGA 2D engine, 0% CPU
    → appsink
    → RKNN YOLOv8n (INT8, 640×640, NPU)
    → Scene hub (JSON over Unix socket)
```

### Post-processing

Full YOLOv8 post-processing integrated into `ai/detector.cpp`:
- 3-branch DFL (Distribution Focal Loss) decode
- Quantized INT8 + FP32 support
- Per-class NMS
- 80 COCO class labels

### Scene hub

Detection results published as JSON datagrams to `/tmp/soulcam_scene.sock`:

```json
{"source":"soulcam","type":"detections","count":5,"objects":[
  {"cls_id":0,"label":"person","conf":0.890,"box":{"left":267,"top":162,"right":477,"bottom":493}},
  ...
]}
```

### Dual-path performance (updated 2026-02-11)

| Metric | RTSP only | RTSP + AI (pre-NEON) | RTSP + AI (NEON) |
|--------|-----------|---------------------|------------------|
| RTSP fps | 30 | 30 | **30** |
| AI fps | — | ~21.7 | **~22.7** |
| CPU | 0.0% | ~18% | **~11%** |
| Memory | ~27 MB | ~37 MB | **~37 MB** |

The mainpath uses NV12 direct to MPP (no RGA needed). The selfpath uses
RGA for NV12→RGB conversion + resize at 0% CPU. Post-processing CPU dropped
from ~18% to **~11%** with NEON SIMD optimization (batch score scanning +
fast DFL softmax). The ~11% is from RKNN post-processing in `detector.cpp`,
not from video conversion.

### NV12 First-Frame Warm-Up (resolved 2026-02-09)

The ISP's NV12 output was previously thought to be broken (UV plane all 0x80).
**This was a testing artifact** — only the very first frame has invalid UV data
(ISP MI warm-up). From frame #2 onwards, NV12 works correctly on both paths.
NV12 and UYVY produce identical image quality (eye-checked via RTSP).

The SoulCam pipeline now uses **NV12 direct** on the mainpath (no RGA
conversion needed) and NV12→RGB via RGA on the selfpath for AI inference.

See `doc/isp/NV12_Y_ZERO_BUG.md` for the full investigation and resolution.

---

## NEON SIMD Post-Processing (verified 2026-02-11)

ARM NEON intrinsics applied to YOLOv8 post-processing in `ai/detector.cpp`:

```
Before (scalar):
  for each cell (8400 total):           ← 80×80 + 40×40 + 20×20
    for each class (80):                ← scalar comparison
      find max class score
    if above threshold:
      DFL decode: 64× expf() + softmax ← scalar, heap-alloc per cell

After (NEON batch):
  for each 16-cell batch (525 total):
    NEON score_sum reject: vld1q_s8 + vcgeq_s8 → skip if all 16 below
    for each class (80):
      vld1q_s8 → vcgtq_s8 → vbslq_u8  ← 16 cells simultaneously
    for passing cells:
      neon_exp_f32 (4-wide) × 16       ← fast minimax polynomial
      vfmaq_f32 weighted sum            ← DFL decode
      stack-allocated temps             ← no malloc
```

### NEON helpers

| Function | Purpose | Speedup |
|----------|---------|---------|
| `neon_exp_f32()` | 4-wide exp(x) via 2^(x·log₂e), 5th-degree minimax | ~4× vs scalar expf |
| `neon_compute_dfl_16()` | Vectorized softmax + weighted-index sum for DFL | ~4× per box side |
| Batch score scan | 16 cells × 80 classes with NEON max-tracking | ~16× throughput |

### Results

| Metric | Before NEON | After NEON | Change |
|--------|------------|------------|--------|
| CPU (RTSP+AI) | ~18% | **~11%** | **-39%** |
| AI fps | ~21.7 | **~22.7** | +4.6% |
| Memory | ~37 MB | ~37 MB | — |

The CPU reduction comes from three sources:
1. **Batch score scanning** (~60% of savings): processing 16 grid cells per
   NEON iteration instead of one, with batch score_sum quick-reject
2. **NEON DFL softmax** (~25%): 4-wide exp + FMA replace 64 scalar expf calls
3. **Heap allocation elimination** (~15%): stack arrays replace std::vector
   in hot loops, removing thousands of malloc/free per frame

The `__ARM_NEON` guard preserves scalar fallback for x86 development builds.

---

## DMA-BUF Zero-Copy I/O (verified 2026-02-09)

Optional `--dmabuf` flag switches v4l2src from `io-mode=2` (userptr) to
`io-mode=4` (DMA-BUF export). In DMA-BUF mode, the ISP/V4L2 driver exports
DMA-BUF file descriptors that downstream elements (mpph264enc on mainpath,
rgaconvert on selfpath) can import directly -- buffers stay in device memory
and are never copied to CPU-accessible memory.

```bash
# Enable DMA-BUF (RTSP only)
./build/soulcam --dmabuf

# DMA-BUF + AI
./build/soulcam --dmabuf --ai --model rk3566/yolov8n.rknn
```

### DMA-BUF vs userptr comparison (2026-02-09)

| Metric | userptr (io-mode=2) | dmabuf (io-mode=4) | Notes |
|--------|--------------------|--------------------|-------|
| **RTSP only** | | | |
| RTSP fps | 30 | **30** | Identical |
| CPU | 0.0% | **0.0%** | Both fully HW |
| Memory | ~27 MB | **~27 MB** | ~Same |
| **RTSP + AI** | | | |
| RTSP fps | 30 | **30** | No degradation |
| AI fps | ~21.7 | **~21.5** | Within margin |
| CPU | ~18% | **~18%** | RKNN post-proc |
| Memory | ~37 MB | **~42 MB** | DMA-BUF overhead |

**Conclusion**: DMA-BUF works correctly on both mainpath and selfpath. The
performance difference is within measurement noise -- the existing NV12 direct
pipeline is already well-optimized (mainpath has zero RGA/CPU overhead), so
the buffer passing improvement from DMA-BUF is minimal. DMA-BUF is available
as a `--dmabuf` flag for future use (e.g., if buffer sizes increase or new
elements are added to the pipeline).

---

## Debug Overlay (verified 2026-02-09)

Optional `--overlay` flag draws detection bounding boxes on the RTSP stream
using `cairooverlay` in the GStreamer pipeline:

```
v4l2src(NV12) → rgaconvert(HW, BGRA) → cairooverlay → rgaconvert(HW, NV12) → mpph264enc → RTSP
```

The overlay uses two RGA passes (NV12→BGRA and BGRA→NV12) with cairo drawing
in between. Detection coordinates are scaled from AI model space (640×640) to
stream space (1280×960). Eye-checked: bounding boxes and labels render correctly
over full-color NV12 video.

| Mode | RTSP fps | AI fps | CPU |
|------|----------|--------|-----|
| RTSP only | 30 | — | 0% |
| RTSP + AI | 30 | ~22 | ~18% |
| RTSP + AI + overlay | ~18 | ~21 | ~20% |

---

## Scene Hub

### Publisher (C++ → Unix socket)
Detection JSON published from `main.cpp` to `/tmp/soulcam_scene.sock` via
`sendto()` on a Unix datagram socket. Zero overhead when no listener is bound.

### Listener (`scene/scene_hub.py`)
Python consumer that:
- Binds to the Unix socket and receives detection JSON
- Prints detections to stdout
- Optionally serves an HTTP API on `--http <port>`

```bash
# Print detections to terminal
python3 scene/scene_hub.py

# With HTTP API on port 8080
python3 scene/scene_hub.py --http 8080

# Query detections
curl http://192.168.1.45:8080/detections
curl http://192.168.1.45:8080/status
```

---

## Model Hot-Swap (verified 2026-02-09)

Switch RKNN models at runtime without restarting the pipeline. The GStreamer
capture pipeline (v4l2src → rgaconvert → appsink) keeps running — only the
NPU model is swapped.

### Usage

```bash
# Swap model via control socket (requires socat or python)
echo '{"cmd":"swap_model","path":"/home/ubuntu/YoloV8-NPU/rk3566/yolov8s.rknn"}' | \
    socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# With custom confidence/NMS thresholds
echo '{"cmd":"swap_model","path":"rk3566/yolov8s.rknn","conf":0.3,"nms":0.5}' | \
    socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Python alternative
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.sendto(b'{\"cmd\":\"swap_model\",\"path\":\"rk3566/yolov8s.rknn\"}',
         '/tmp/soulcam_ctrl.sock')
"

# Status check
echo '{"cmd":"status"}' | socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Ping
echo '{"cmd":"ping"}' | socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock
```

### How it works

1. New model is loaded on the NPU (RKNN init)
2. Mutex lock acquired on the detector pointer
3. Old detector swapped out, new detector swapped in
4. Mutex released — next frame uses the new model
5. Old detector destroyed (RKNN teardown)

The swap takes ~200ms (model load + NPU init). During the swap, frames are
still captured but inference is briefly paused.

---

## ONVIF Metadata Stream (verified 2026-02-09)

ONVIF-compliant analytics metadata served via a secondary RTSP stream on
`port+1` (default: 8555). Detection events use the ONVIF schema
`tt:MetadataStream / tt:VideoAnalytics / tt:Frame / tt:Object`.

### Enable

```bash
./build/soulcam --ai --onvif --model rk3566/yolov8n.rknn
```

### Endpoints

| Endpoint | Protocol | Description |
|----------|----------|-------------|
| `rtsp://<ip>:8555/cam/meta` | RTSP/RTP | Live metadata stream (ONVIF XML) |
| `http://<ip>:8080/onvif/metadata` | HTTP | Pull-based metadata query (scene_hub) |

### ONVIF XML format

```xml
<?xml version="1.0" encoding="UTF-8"?>
<tt:MetadataStream xmlns:tt="http://www.onvif.org/ver10/schema">
  <tt:VideoAnalytics>
    <tt:Frame UtcTime="2026-02-09T14:55:00Z">
      <tt:Transformation>
        <tt:Translate x="-1.0" y="-1.0"/>
        <tt:Scale x="0.003125" y="0.003125"/>
      </tt:Transformation>
      <tt:Object ObjectId="1">
        <tt:Appearance>
          <tt:Shape>
            <tt:BoundingBox left="-0.1656" top="-0.4938"
                            right="0.4906" bottom="0.5438"/>
          </tt:Shape>
          <tt:Class>
            <tt:Type Likelihood="0.890">person</tt:Type>
          </tt:Class>
        </tt:Appearance>
      </tt:Object>
    </tt:Frame>
  </tt:VideoAnalytics>
</tt:MetadataStream>
```

Coordinates use ONVIF normalized space: x,y in [-1,+1] where (-1,-1) = top-left.

---

## Multi-Model Pipeline (verified 2026-02-12)

Supports running multiple RKNN models on the same selfpath frame.  Each
model slot has independent confidence thresholds and frame-skip settings.
The RK3566 NPU is single-core, so models run sequentially per frame.

### Architecture

```
selfpath (/dev/video9)
  NV12 640×480 @ 30fps
    → rgaconvert (HW: NV12→RGB + resize 640×640)
    → appsink
    → ModelPipeline (orchestrator)
        ├── slot 0: Detector (yolov8n)  ← runs every frame
        ├── slot 1: Detector (yolov8s)  ← runs every 3rd frame (skip=2)
        └── slot 2: Detector (yolov8m)  ← runs every 5th frame (skip=4)
    → merged detections (tagged with model_id)
    → callback → scene hub / overlay / ONVIF
```

### Frame skipping strategy

Frame skipping allows mixing a fast primary model (every frame) with
slower high-accuracy models (every N frames), balancing latency vs CPU.

| skip_frames | Effective rate | Example use case |
|-------------|---------------|------------------|
| 0           | Every frame   | Fast primary detector (yolov8n) |
| 2           | Every 3rd     | High-confidence verification (yolov8s) |
| 4           | Every 5th     | Heavy model, periodic deep scan (yolov8m) |
| 9           | Every 10th    | Expensive secondary task (face recognition) |

### Performance (verified 2026-02-12)

| Configuration | AI fps | CPU | Memory |
|--------------|--------|-----|--------|
| 1 model (yolov8n) | ~22.7 | ~11% | ~37 MB |
| 2 models (yolov8n + yolov8s, skip=2) | ~17 | ~10% | ~60 MB |
| 3 models (yolov8n + yolov8s + yolov8m, skip=2,4) | ~14 | ~10% | ~101 MB |

### Runtime model management

Models can be added, removed, swapped, and enabled/disabled at runtime
via the control socket (no pipeline restart needed):

```bash
# Add a model at runtime
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.sendto(b'{\"cmd\":\"add_model\",\"path\":\"/path/to/model.rknn\",\"skip\":2}',
         '/tmp/soulcam_ctrl.sock')
"

# List all model slots
echo '{"cmd":"list_models"}' | socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Swap model in a specific slot
echo '{"cmd":"swap_model","slot":1,"path":"rk3566/yolov8m.rknn"}' | \
    socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Disable a model slot (keeps loaded, stops inference)
echo '{"cmd":"enable_model","slot":1,"enable":false}' | \
    socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Remove a model slot (unloads from NPU)
echo '{"cmd":"remove_model","slot":1}' | \
    socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock
```

### Scene hub JSON format (multi-model)

Detection JSON now includes `model` field identifying which slot produced
each detection:

```json
{"source":"soulcam","type":"detections","count":5,"objects":[
  {"model":0,"cls_id":0,"label":"person","conf":0.890,"box":{"left":267,"top":162,"right":477,"bottom":493}},
  {"model":1,"cls_id":0,"label":"person","conf":0.950,"box":{"left":265,"top":160,"right":480,"bottom":495}}
]}
```

---

## Systemd Services

### `soulcam.service`
Auto-starts the RTSP server at boot. Edit `ExecStart` to enable AI/overlay.

```bash
sudo systemctl enable soulcam          # enable at boot
sudo systemctl start soulcam           # start now
sudo systemctl status soulcam          # check status
journalctl -u soulcam -f               # follow logs
```

### `scene_hub.service`
Starts scene hub after soulcam, serves HTTP API on port 8080.

```bash
sudo systemctl enable scene_hub
sudo systemctl start scene_hub
```

Both services are installed at `/etc/systemd/system/` and enabled for boot.

---

## ONVIF Device Service (verified 2026-02-09)

ONVIF Profile S/T compliant device service for NVR/VMS auto-discovery.
Enabled with `--onvif-device` flag.

### Architecture

```
┌─────────────────────────────────────────────────┐
│          ONVIF Device Service                    │
│                                                  │
│  ┌─────────────────────┐  ┌───────────────────┐ │
│  │ WS-Discovery        │  │ HTTP SOAP Service │ │
│  │ UDP 239.255.255.250 │  │ TCP :8080         │ │
│  │ :3702               │  │                   │ │
│  │                     │  │ /onvif/device_svc │ │
│  │ - Hello on startup  │  │ /onvif/media_svc  │ │
│  │ - Probe → ProbeMatch│  │ /onvif/analytics  │ │
│  └─────────────────────┘  └───────────────────┘ │
└─────────────────────────────────────────────────┘
```

### Supported ONVIF operations

| Endpoint | Operation | Description |
|----------|-----------|-------------|
| Device | GetDeviceInformation | Manufacturer, model, firmware |
| Device | GetCapabilities | Device + Media + Analytics services |
| Device | GetServices | Service namespace + XAddr |
| Device | GetScopes | Profile S/T scopes |
| Device | GetSystemDateAndTime | UTC + local time |
| Device | GetNetworkInterfaces | IPv4 address + interface info |
| Media | GetProfiles | H.264 stream profile (resolution, fps, bitrate) |
| Media | GetStreamUri | RTSP URI for the main stream |

### WS-Discovery

On startup, the device sends a **Hello** multicast to `239.255.255.250:3702`,
announcing itself as a `dn:NetworkVideoTransmitter`. NVRs (Milestone, Genetec,
Blue Iris, etc.) that scan for ONVIF devices will receive this announcement.

When an NVR sends a **Probe** message, the device responds with a **ProbeMatch**
containing its UUID, scopes, and the XAddr (HTTP SOAP service URL).

### Testing

```bash
# Test GetDeviceInformation
curl -X POST http://192.168.1.45:8080/onvif/device_service \
  -H "Content-Type: application/soap+xml" \
  -d '<?xml version="1.0"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tds="http://www.onvif.org/ver10/device/wsdl"><s:Body><tds:GetDeviceInformation/></s:Body></s:Envelope>'

# Test GetStreamUri
curl -X POST http://192.168.1.45:8080/onvif/media_service \
  -H "Content-Type: application/soap+xml" \
  -d '<?xml version="1.0"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:trt="http://www.onvif.org/ver10/media/wsdl"><s:Body><trt:GetStreamUri/></s:Body></s:Envelope>'

# Status page
curl http://192.168.1.45:8080/
```

---

## Completed Steps

- [x] Integrate YOLOv8 post-processing (DFL decode + NMS) into detector.cpp
- [x] Scene hub integration (JSON over Unix datagram socket)
- [x] Dual-path RTSP + AI verified end-to-end on device
- [x] RGA for AI path: replaced software videoconvert+videoscale with rgaconvert
      → RTSP restored to 30fps, AI inference +53% faster (14.6→22.3 fps)
- [x] Debug overlay: cairooverlay with RGA pipeline, ~18fps with boxes
- [x] Scene hub listener: Python consumer with HTTP API
- [x] Systemd services: soulcam.service + scene_hub.service, enabled at boot
- [x] DMA-BUF optimization: `io-mode=4` (dmabuf export) tested and verified
      on both mainpath (RTSP) and selfpath (AI).
- [x] Model hot-swap: runtime RKNN model switching via Unix control socket
      (`/tmp/soulcam_ctrl.sock`). Thread-safe mutex-protected detector swap.
      No pipeline restart needed — only the NPU model changes.
- [x] ONVIF metadata stream: ONVIF-compliant XML analytics metadata served
      via separate RTSP stream (port 8555 `/cam/meta`). Detection events
      formatted as `tt:MetadataStream/tt:VideoAnalytics/tt:Frame/tt:Object`
      with normalized coordinates. Also available via HTTP at scene_hub
      `/onvif/metadata` endpoint.
- [x] Control socket: runtime command interface at `/tmp/soulcam_ctrl.sock`.
      Supports `swap_model`, `status`, `ping` commands via JSON over Unix
      datagram. Enables external automation and model management.
- [x] ONVIF device service: WS-Discovery (UDP multicast 239.255.255.250:3702)
      + HTTP SOAP service (default port 8080). Supports GetDeviceInformation,
      GetCapabilities, GetServices, GetScopes, GetSystemDateAndTime,
      GetNetworkInterfaces, GetProfiles, GetStreamUri. Hello announcement
      on startup, ProbeMatch on NVR discovery requests. Zero dependencies.
      Enable with `--onvif-device` flag.
- [x] NV12 "bug" resolved (2026-02-09): The NV12 UV-plane issue was a
      first-frame warm-up artifact, not a hardware bug. Previous tests
      captured only 3-5 frames and analyzed frame #1 (always UV=0x80).
      With 50-frame captures, UV data is valid from frame #2 onwards.
      Pipeline switched from UYVY+RGA to **NV12 direct** on mainpath
      (one fewer GStreamer element). Eye-checked: full color, correct
      exposure, bounding boxes working with overlay enabled.
      See `doc/isp/NV12_Y_ZERO_BUG.md` for full analysis.
- [x] NEON SIMD post-processing (2026-02-11): ARM NEON intrinsics for
      YOLOv8 post-processing in `detector.cpp`. Three optimizations:
      1. **Batch score scanning**: Process 16 grid cells simultaneously using
         `vld1q_s8` / `vcgtq_s8` / `vbslq_u8` for the 80-class max-finding
         loop. Batch score_sum quick-reject via `vmaxvq_u8`.
      2. **NEON DFL softmax**: `neon_compute_dfl_16()` replaces 64 scalar
         `expf()` calls with 16 vectorized 4-wide `neon_exp_f32()` (5th-degree
         minimax polynomial, <0.05% relative error). Vectorized weighted sum
         with `vfmaq_f32` + `vaddvq_f32`.
      3. **Heap allocation elimination**: Replaced `std::vector<float>` temp
         buffers inside hot loops with stack-allocated `float[64]` arrays,
         eliminating thousands of malloc/free per frame.
      Result: CPU ~18% → **~11%** (~39% reduction), AI fps ~21.7 → **~22.7**.
      Scalar fallback maintained for non-NEON builds (x86 development).
- [x] GStreamer plugin path fix (2026-02-11): Moved `GST_PLUGIN_PATH` setup
      from `rtsp_server_start()` to `main()` before `gst_init()`. Ensures
      rgaconvert plugin is discovered during GStreamer registry scan at init
      time, fixing intermittent "not-linked" AI pipeline failures.
- [x] Strategic pivot ONVIF → Tuya (2026-02-12): Deprioritized ONVIF
      development in favor of Tuya IPC SDK integration for Alexa / Google
      Assistant / HomeKit streaming. ONVIF code retained as optional feature.
      Created `doc/tuya/TUYA_INTEGRATION_PLAN.md` with full integration plan.
- [x] JPEG snapshot endpoint (2026-02-12): Single-frame JPEG capture via
      ISP selfpath (/dev/video9). HTTP server on port 8088 with endpoints:
      `GET /snapshot` (JPEG image) and `GET /snapshot/info` (JSON metadata).
      Uses one-shot GStreamer pipeline: `v4l2src num-buffers=1 → videoconvert
      → jpegenc → appsink`. ISP selfpath auto-configured when `--snapshot`
      is enabled. Capture latency ~0.7s. Enable with `--snapshot` flag.
- [x] Tuya IPC SDK adapter layer (2026-02-12): Skeleton module
      (`pipeline/tuya_ipc.h/.cpp`) with complete interfaces for Tuya
      integration: `tuya_push_video_frame()` for H.264 ring buffer feed,
      `tuya_notify_event()` for AI detection push notifications, status
      callbacks, and DP command handling. Compiles in stub mode when Tuya
      SDK is not present (`SOULCAM_HAVE_TUYA` not defined). Real SDK
      activation requires TuyaOS IPC SDK binaries + device credentials.
- [x] Multi-model pipeline (2026-02-12): Support running multiple RKNN
      models on the same selfpath frame. New `ai/model_pipeline.h/.cpp`
      orchestrator manages N model slots, each with independent config
      and frame-skip settings. Key features:
      1. **Frame skipping**: Each slot can skip N frames (e.g., yolov8n
         every frame, yolov8s every 3rd frame). Balances latency vs CPU.
      2. **Runtime management**: Add/remove/swap/enable/disable models
         via control socket (`add_model`, `remove_model`, `swap_model`,
         `enable_model`, `list_models` commands).
      3. **CLI support**: `--model2`, `--model3` with per-model `--skip`
         and `--conf` options.
      4. **Detection tagging**: Each detection carries `model_id` field.
         Scene hub JSON includes `"model":N` per detection.
      Tested: 3 models (yolov8n + yolov8s + yolov8m) running simultaneously
      with skip=0,2,4. CPU ~10%, memory ~101 MB. Sequential NPU execution
      with no pipeline restart needed for model changes.

## Next Steps

### Priority 1: Future Enhancements

1. **Cascade model pipeline**: Chain model outputs (e.g., person detection
   → face crop → face recognition). Currently all models see the full
   frame; cascade mode would crop ROIs from model A and feed to model B.

### Retained (optional, low priority)

- **ONVIF device service**: Already implemented and working (`--onvif-device`).
  Kept as optional feature for local NVR compatibility. No further ONVIF
  development planned (authentication, etc.) unless specifically needed.

- **Tuya IPC SDK Integration** (target: Alexa / Google / HomeKit):
  Original goal was to connect to Tuya IoT Cloud for streaming to Amazon
  Alexa (Echo Show), Google Assistant (Chromecast), and Apple HomeKit.
  See `doc/tuya/TUYA_INTEGRATION_PLAN.md` for the full integration plan.

  Completed preparatory work:
  - **JPEG snapshot endpoint** *(completed 2026-02-12)*: Single-frame JPEG
    capture from ISP selfpath. HTTP `GET /snapshot` on port 8088. Enable
    with `--snapshot` flag.
  - **Tuya adapter layer** *(completed 2026-02-12)*: Skeleton module
    (`pipeline/tuya_ipc.h/.cpp`) with interfaces for Tuya IPC SDK integration.
    Not linked to actual Tuya SDK (requires SDK binaries from Tuya Developer
    Platform).

  Remaining work (if needed):
  - Tuya Cloud setup (register on platform, get credentials)
  - H.264 frame tap + Tuya ring buffer integration
  - AI event → Tuya notification bridge
  - Audio support (ALSA → AAC/PCM for two-way audio)
  - Local SD card recording (Tuya `tuya_ipc_ss_*` APIs)
