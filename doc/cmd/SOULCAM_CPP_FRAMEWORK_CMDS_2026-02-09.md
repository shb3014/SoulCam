# SoulCam C++ Framework — Build, Deploy & Test Commands

_Board: Vicharak (RK3566) · Camera: OV5647 160° fisheye_
_Date: 2026-02-09_

---

## 1. Deploy from WSL to device

```bash
cd /home/shb3014/embeddedProjects/SoulCam

# Sync src, scripts, rknn to device (flat layout)
sshpass -p 'shb084ww' rsync -avz --progress \
    --exclude='build/' \
    --exclude='kernel_*/' \
    --exclude='ubuntu-rockchip-*/' \
    --exclude='device_backups/' \
    --exclude='old/' \
    --exclude='.git/' \
    --exclude='__pycache__/' \
    --exclude='*.o' \
    --exclude='*.pyc' \
    -e "ssh -o StrictHostKeyChecking=no" \
    src/ scripts/ rknn/ \
    ubuntu@192.168.1.45:/home/ubuntu/SoulCam/
```

## 2. Install build dependencies (once)

```bash
# On device
sudo apt install -y \
    build-essential cmake pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstrtspserver-1.0-dev \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-rtsp
```

## 3. Build on device

```bash
ssh ubuntu@192.168.1.45

cd ~/SoulCam
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j4
```

Or use the script:
```bash
bash scripts/build.sh          # Release
bash scripts/build.sh debug    # Debug
bash scripts/build.sh clean    # Clean + rebuild
```

## 4. Run RTSP server

```bash
# On device: set GST_PLUGIN_PATH for rgaconvert and start
export GST_PLUGIN_PATH="/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0:${GST_PLUGIN_PATH:-}"

# Foreground (Ctrl+C to stop)
./build/soulcam

# Background (daemonized)
nohup ./build/soulcam > /tmp/soulcam.log 2>&1 &

# Verbose mode
./build/soulcam -v

# RTSP + AI (selfpath)
./build/soulcam --ai --model rk3566/yolov8n.rknn
```

## 5. Stop RTSP server

```bash
# On device
kill $(pgrep soulcam)
```

## 6. Test RTSP stream (from WSL/PC)

```bash
# Quick check (8s, null output)
ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.45:8554/cam -t 8 -f null -

# Stream info (codec, resolution, profile)
ffprobe -rtsp_transport tcp -v quiet -print_format json \
    -show_streams rtsp://192.168.1.45:8554/cam

# View live
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay \
    -framedrop rtsp://192.168.1.45:8554/cam

# Record 10s clip
ffmpeg -y -rtsp_transport tcp -i rtsp://192.168.1.45:8554/cam \
    -t 10 -c copy soulcam_clip.mp4

# Or use the test script
./scripts/test_rtsp.sh 192.168.1.45 check
./scripts/test_rtsp.sh 192.168.1.45 play
./scripts/test_rtsp.sh 192.168.1.45 probe
./scripts/test_rtsp.sh 192.168.1.45 record
```

## 7. Verify on device

```bash
# Process running
pgrep -a soulcam

# RTSP port listening
ss -lntp | grep 8554

# CPU usage (expect 0% for HW pipeline)
top -bn1 -p $(pgrep soulcam) | tail -2

# RGA hardware active (interrupt count should increase)
cat /proc/interrupts | grep rga

# Tail live log
tail -f /tmp/soulcam.log
```

## 8. Cleanup (kill all camera processes)

```bash
# On device
pkill -f soulcam
pkill -f rk3566_capture_shm.py
pkill -f rk3566_rtsp_server.py
pkill -f rk3566_debug_overlay.py
pkill -f gst-launch-1.0
```

## 9. Run RTSP + AI (dual-path)

```bash
# On device
export GST_PLUGIN_PATH="/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0:${GST_PLUGIN_PATH:-}"

# Foreground
./build/soulcam -v --ai --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn

# Background
nohup ./build/soulcam --ai --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn > /tmp/soulcam.log 2>&1 &

# Check AI detections
tail -f /tmp/soulcam.log | grep "Detections:"
```

## 10. RTSP + AI + Overlay (debug boxes)

```bash
# On device -- draw detection boxes on stream
export GST_PLUGIN_PATH="/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0:${GST_PLUGIN_PATH:-}"
./build/soulcam -v --ai --overlay --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn
```

## 11. Scene hub (consume detection JSON)

```bash
# On device -- listen for detections, print to stdout
python3 scene/scene_hub.py

# With HTTP API on port 8080
python3 scene/scene_hub.py --http 8080

# Query from any machine
curl http://192.168.1.45:8080/detections
curl http://192.168.1.45:8080/status
```

## 12. Systemd services (auto-start at boot)

```bash
# Install services (already done)
sudo cp service/soulcam.service /etc/systemd/system/
sudo cp service/scene_hub.service /etc/systemd/system/
sudo systemctl daemon-reload

# Enable at boot
sudo systemctl enable soulcam scene_hub

# Start/stop
sudo systemctl start soulcam
sudo systemctl stop soulcam
sudo systemctl status soulcam

# Follow logs
journalctl -u soulcam -f
journalctl -u scene_hub -f

# Edit ExecStart to enable AI/overlay mode:
sudo systemctl edit soulcam --full
# Change ExecStart to:
#   /home/ubuntu/SoulCam/build/soulcam --ai --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn
# Or with overlay:
#   /home/ubuntu/SoulCam/build/soulcam --ai --overlay --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn
```

## 11. Selfpath NV12 bug

```bash
# NV12 has Y=0 bug on BOTH mainpath and selfpath (verified 2026-02-09)
# Always use UYVY for ISP capture, convert downstream.
# Test:
v4l2-ctl -d /dev/video9 --set-fmt-video=width=640,height=480,pixelformat=NV12 \
    --stream-mmap --stream-count=5 --stream-to=/dev/shm/selfpath_nv12.raw
xxd -l 64 /dev/shm/selfpath_nv12.raw  # all zeros = Y=0 bug

v4l2-ctl -d /dev/video9 --set-fmt-video=width=640,height=480,pixelformat=UYVY \
    --stream-mmap --stream-count=5 --stream-to=/dev/shm/selfpath_uyvy.raw
xxd -l 64 /dev/shm/selfpath_uyvy.raw  # 80 00 pattern = valid UYVY
```

---

## Verified Results (2026-02-09)

### RTSP only

| Metric | Value |
|--------|-------|
| Build | Clean, zero warnings (GCC 13.3, C++17) |
| Resolution | 1280×960 |
| Framerate | 30 fps (236 frames / 7.96s) |
| Codec | H.264 High Profile, Level 4.0 |
| Bitrate | ~3.8 Mbps (target 4.0) |
| CPU usage | **0.0%** |
| Memory | 19.7 MB RSS |
| RGA HW | Active (436+ interrupts in 8s) |
| Pipeline | `v4l2src → rgaconvert(HW) → mpph264enc(HW) → RTSP` |

### RTSP + AI (dual-path, RGA hardware)

| Metric | Value |
|--------|-------|
| RTSP stream | 1280×960 @ **30 fps** (no degradation) |
| AI inference | **~22.3 fps** (224 inferences / 10s) |
| Model | YOLOv8n (640×640, quantized INT8) |
| Detections | person 0.88, vase 0.82, potted plant 0.62 |
| CPU usage | 10-18% (RKNN post-processing only, not video conversion) |
| Memory | 37 MB RSS |
| RKNN SDK | 2.3.2, driver 0.9.7 |
| AI pipeline | `selfpath(UYVY 640×480) → rgaconvert(HW, RGB 640×640) → RKNN` |
| Scene hub | JSON detections published to `/tmp/soulcam_scene.sock` |

### DMA-BUF zero-copy (verified 2026-02-09)

```bash
# DMA-BUF mode (io-mode=4) -- zero-copy buffer passing ISP→RGA→MPP
./build/soulcam --dmabuf
./build/soulcam --dmabuf --ai --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn
./build/soulcam --dmabuf --ai --overlay --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn
```

## 13. Model hot-swap (runtime)

```bash
# Swap to a different RKNN model at runtime (no pipeline restart)
echo '{"cmd":"swap_model","path":"/home/ubuntu/YoloV8-NPU/rk3566/yolov8s.rknn"}' | \
    socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Or with Python
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.sendto(b'{\"cmd\":\"swap_model\",\"path\":\"rk3566/yolov8s.rknn\"}', '/tmp/soulcam_ctrl.sock')
"

# Check status
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.sendto(b'{\"cmd\":\"status\"}', '/tmp/soulcam_ctrl.sock')
"
# Response in /tmp/soulcam.log
```

## 14. RTSP + AI + ONVIF metadata stream

```bash
# On device -- enable ONVIF metadata stream on port 8555
export GST_PLUGIN_PATH="/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0:${GST_PLUGIN_PATH:-}"
./build/soulcam -v --ai --onvif --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn

# ONVIF metadata available at:
#   RTSP:  rtsp://192.168.1.45:8555/cam/meta
#   HTTP:  http://192.168.1.45:8080/onvif/metadata  (via scene_hub)
```

## 15. Scene hub with ONVIF HTTP endpoint

```bash
# Start scene hub with HTTP API (includes /onvif/metadata)
python3 scene_hub.py --http 8080

# Query ONVIF XML metadata
curl http://192.168.1.45:8080/onvif/metadata

# Query JSON detections (existing)
curl http://192.168.1.45:8080/detections
```

### Optimization paths
- ~~Use DMA-BUF (io-mode=4) for zero-copy between ISP/RGA/MPP~~ ✅ Done
- ~~Model hot-swap: runtime RKNN model switching~~ ✅ Done (v0.2.0)
- ~~ONVIF metadata stream~~ ✅ Done (v0.2.0, port 8555)
- Fix NV12 UV-plane bug (Y works, UV is 0x80 -- see doc/isp/NV12_Y_ZERO_BUG.md)
- Optimize RKNN post-processing (NEON SIMD for DFL decode + NMS)
