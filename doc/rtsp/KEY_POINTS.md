## RTSP Key Points (RK3566 SoulCam)

_Updated: 2026-02-09_

### C++ Framework (current, recommended)
- Binary: `build/soulcam` (built from `src/`)
- Default mount: `rtsp://192.168.1.45:8554/cam`
- **Fully hardware-accelerated**: RGA (UYVY→NV12) + MPP (H.264 encode)
- **0% CPU** for pixel processing (verified 2026-02-09)
- Single process, single GStreamer pipeline, no shm hop
- Working resolution: 1280x960 @ 30fps, H.264 High, ~4 Mbps
- See `doc/framework/SOULCAM_FRAMEWORK.md` for full details

### Quick start (C++ framework)
```bash
cd ~/SoulCam
bash scripts/build.sh
./build/soulcam                              # RTSP only
./build/soulcam --ai --model rk3566/yolov8n.rknn  # RTSP + AI
./build/soulcam -v                            # verbose
```

### Legacy Python pipeline (Option C, deprecated)
- RTSP server: `ipc/rk3566_rtsp_server.py`
- Capture daemon: `ipc/rk3566_capture_shm.py`
- Uses shm sockets + videoconvert (software) -- higher CPU, more latency
- Still works for prototyping / debug overlay

### Base RTSP pipeline
- Default mount: `rtsp://192.168.1.45:8554/cam`
- Working with ISP: use stream size 1280x960 (4:3)
- 1296x972 can fail with `gst v4l2src` negotiation (height step mismatch)
- Prefer ISP UYVY + daemon conversion to NV12 (RGA or videoconvert)

### Option C (capture once, fan out)
- Single capture daemon publishes shm sockets:
  - `/tmp/soulcam_stream.sock` (stream feed)
  - `/tmp/soulcam_cv.sock` (YOLO input)
  - `/tmp/soulcam_cv_debug.sock` (debug overlay input)
- RTSP reads from shm, so it never touches `/dev/video8`
- ISP-safe launch (avoid 1296x972):
  ```bash
  DAEMONIZE=1 SENSOR_PRESET=fast4_3 STREAM_WIDTH=1280 STREAM_HEIGHT=960 \
    SOURCE_FORMAT=UYVY STREAM_FORMAT=NV12 USE_RGA=1 \
    ./ipc/run_rk3566_camera_daemon.sh
  ```

### Debug overlay (YOLO boxes on RTSP)
- Enable debug overlay:
  ```bash
  DAEMONIZE=1 SENSOR_PRESET=fast4_3 DEBUG_OVERLAY=1 DEBUG_WIDTH=640 DEBUG_HEIGHT=480 DEBUG_FPS=15 \
    SOURCE_FORMAT=UYVY STREAM_FORMAT=NV12 USE_RGA=1 \
    ./ipc/run_rk3566_camera_daemon.sh
  ```
- Run YOLO (debug socket):
  ```bash
  DEBUG_SOCK=/tmp/soulcam_debug_scene.sock \
    ./YoloV8_NPU rk3566/yolov8n.rknn shm://cv
  ```
- View debug stream (same mount):
  ```text
  rtsp://192.168.1.45:8554/cam
  ```

### Verification

#### C++ framework
```bash
# WSL/PC stream check
ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.45:8554/cam -t 8 -f null -

# Stream info (codec, resolution, profile)
ffprobe -rtsp_transport tcp -v quiet -print_format json \
    -show_streams rtsp://192.168.1.45:8554/cam

# On device: check process and CPU
pgrep -a soulcam
top -bn1 -p $(pgrep soulcam) | tail -2

# On device: check RGA HW activity
cat /proc/interrupts | grep rga
```

#### Legacy Python
```bash
pgrep -af rk3566_capture_shm.py
pgrep -af rk3566_debug_overlay.py
pgrep -af rk3566_rtsp_server.py
pgrep -af YoloV8_NPU
ls -l /tmp/soulcam_*sock
```

### Verified Results (2026-02-09, C++ framework)
| Metric | Value |
|--------|-------|
| Resolution | 1280×960 |
| Framerate | 30 fps |
| Codec | H.264 High Profile Level 4.0 |
| Bitrate | ~3.8 Mbps |
| CPU usage | 0.0% |
| Memory | 19.7 MB RSS |
| Pipeline | v4l2src→rgaconvert(HW)→mpph264enc(HW)→RTSP |

