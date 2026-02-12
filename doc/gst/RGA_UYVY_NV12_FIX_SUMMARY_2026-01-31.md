# RGA UYVY->NV12 Fix Summary (2026-01-31)

## Goal
Make `rgaconvert` reliably handle ISP UYVY input and produce correct NV12 output on RK3566, then validate output against ISP frames (no RTSP tuning).

## What Changed (Device)
### librga / headers
- Removed older `/usr/local/lib/librga.so` and `/usr/local/include/rga` (backed up).
- Using system `librga2` (from PPA): `2.2.0-1+git20231208.a9fc19e6~noble1`.
- `rgaconvert` now links to `/lib/aarch64-linux-gnu/librga.so.2`.

### rgaconvert (gstreamer-rgaconvert)
Key fixes and instrumentation in `gstrgaconvert.c`:
- **UYVY added to caps** (sink/src).
- **UYVY mapping** added in `gst_gst_format_to_rga_format()`.
- **Packed YUV422 stride handling** added (`UYVY/YUYV/VYUY/YVYU` => `pixel_stride=2`).
- **Buffer mapping before RGA info** to avoid early failure when `virAddr` is unset.
- **UYVY->NV12 path uses `imcvtcolor_t`** (im2d) for conversion.
- Added **debug prints** for format, stride, mapping, and RGA info (can be removed once stable).

### Capture test mode in launcher
`ipc/run_rk3566_camera_daemon.sh` supports:
- `CAPTURE_TEST=1` to capture ISP UYVY + RGA NV12 and **convert only the last frame**.
- `CAPTURE_COUNT` (default 5) for ISP stabilization.
- `CAPTURE_DIR` (default `/dev/shm` to avoid full root filesystem).

Example:
```bash
CAPTURE_TEST=1 CAPTURE_COUNT=5 CAPTURE_DIR=/dev/shm \
  STREAM_WIDTH=1280 STREAM_HEIGHT=960 FPS=30 USE_RGA=1 \
  ./ipc/run_rk3566_camera_daemon.sh
```
Outputs:
- `/dev/shm/isp_uyvy.png`
- `/dev/shm/rga_nv12.png`

## Validation Commands
### 1) File-based conversion (no camera)
```bash
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=1280,height=960,pixelformat=UYVY \
  --stream-mmap --stream-count=2 --stream-to=/dev/shm/isp_uyvy.raw

GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0 \
  gst-launch-1.0 -v filesrc location=/dev/shm/isp_uyvy.raw ! \
  rawvideoparse format=uyvy width=1280 height=960 framerate=30/1 ! \
  rgaconvert ! video/x-raw,format=NV12,width=1280,height=960 ! fakesink
```

### 2) Live camera UYVY -> NV12 (RGA)
```bash
GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0 \
  gst-launch-1.0 -v v4l2src device=/dev/video8 io-mode=2 num-buffers=30 \
  ! video/x-raw,format=UYVY,width=1280,height=960,framerate=30/1 \
  ! rgaconvert ! video/x-raw,format=NV12,width=1280,height=960 ! fakesink
```

## Notes / Gotchas
- `gst_value_set_int_range_step` warnings are **benign** caps warnings.
- **/dev/video8 can be busy** if debug overlay or RTSP daemon is running.
  - Stop them before direct capture:
    ```bash
    pkill -f rk3566_debug_overlay.py || true
    pkill -f rk3566_capture_shm.py || true
    pkill -f rk3566_rtsp_server.py || true
    pkill -f gst-launch-1.0 || true
    ```
- Root filesystem is **full**; use `/dev/shm` for temporary files.

## Result
- `rgaconvert` now accepts UYVY input and converts to NV12 using `imcvtcolor_t`.
- Both ISP and RGA frame captures can be compared using last-frame PNGs.
