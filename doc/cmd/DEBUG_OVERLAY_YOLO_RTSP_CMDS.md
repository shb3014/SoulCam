## Debug Overlay + YOLO RTSP (Option C) - Commands

### Start debug overlay RTSP (RK3566)
```bash
cd /home/ubuntu/SoulCam
DAEMONIZE=1 SENSOR_PRESET=fast4_3 STREAM_WIDTH=1280 STREAM_HEIGHT=960 \
  SOURCE_FORMAT=UYVY STREAM_FORMAT=NV12 USE_RGA=1 \
  DEBUG_OVERLAY=1 DEBUG_WIDTH=640 DEBUG_HEIGHT=480 DEBUG_FPS=15 \
  ./ipc/run_rk3566_camera_daemon.sh
```

### Direct NV12 stream (not recommended)
```bash
cd /home/ubuntu/SoulCam
s
```
Note: Iasdfgtsdfb5 5 5 5 5 5 5 5 5 5 5 5 5 h;P NV12 output can fail or produce incorrect colors on this stack.
Prefer UYVY from ISP and convert to NV12 in the daemon (RGA or videoconvert).

### Capture ISP vs RGA frames (on device)
```bash
# ISP direct (UYVY) - match media graph (1296x972)
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=1296,height=972,pixelformat=UYVY \
  --stream-mmap --stream-count=15 --stream-to=/tmp/isp_uyvy.raw

# Convert ISP UYVY to PNG (frame 0)
ffmpeg -y -f rawvideo -pix_fmt uyvy422 -s 1296x972 -i /tmp/isp_uyvy.raw \
  -frames:v 1 /tmp/isp_uyvy.png

# RGA path (UYVY -> NV12) using rgaconvert (1280x960 RTSP-safe size)
GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0 \
  gst-launch-1.0 -v v4l2src device=/dev/video8 io-mode=2 num-buffers=2 \
  ! video/x-raw,format=UYVY,width=1280,height=960,framerate=30/1 \
  ! rgaconvert ! video/x-raw,format=NV12,width=1280,height=960 \
  ! filesink location=/tmp/rga_nv12.raw

# Convert RGA NV12 to PNG (frame 0)
ffmpeg -y -f rawvideo -pix_fmt nv12 -s 1280x960 -i /tmp/rga_nv12.raw \
  -frames:v 1 /tmp/rga_nv12.png
```

### Run YOLO and send detections to overlay
```bash
cd /home/ubuntu/YoloV8-NPU
DEBUG_SOCK=/tmp/soulcam_debug_scene.sock \
  ./YoloV8_NPU rk3566/yolov8n.rknn shm://cv
```

### View stream (PC)
```text
rtsp://192.168.1.45:8554/cam
```

### Verify stream from WSL (optional)
```bash
ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.45:8554/cam -t 6 -f null -
```

### Check status on RK3566
```bash
pgrep -af rk3566_capture_shm.py
pgrep -af rk3566_debug_overlay.py
pgrep -af rk3566_rtsp_server.py
pgrep -af YoloV8_NPU
ls -l /tmp/soulcam_*sock
```

