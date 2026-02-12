## Session Summary: RK3566 RTSP + ISP (2026-01-30)

### Goal
Investigate why RTSP stream looks red while direct ISP capture (NV12) looks normal.
Enable GPU/HW acceleration (RGA + MPP) and test whether that fixes colors.

### Target Device
- Host: 192.168.1.45 (Ubuntu 24.04.3 LTS, arm64)
- User: ubuntu

### Key Observations
- Direct ISP NV12 capture via `v4l2-ctl` (mainpath) looks normal.
- RTSP stream via GStreamer pipeline shows red cast.
- Color shift likely introduced by GStreamer colorspace conversion path (UYVY->NV12/I420).
- `rkisp` GStreamer source exists but **segfaults** on `gst-launch-1.0`.
- RGA conversion plugin was not present initially; added later.

### Files Touched (repo)
Changed files during the session (all changes are in repo; device was sync’d repeatedly):
- `ipc/rk3566_capture_shm.py`
  - Added configurable `--source-format` and `--stream-format`.
  - Added `--use-rga` to switch conversion element from `videoconvert` to `rgaconvert`.
  - Added logic to support NV21 stream output.
  - Introduced conversion chain logic; current CV path uses `videoconvert ! videoscale ! videoconvert`.
- `ipc/rk3566_rtsp_server.py`
  - Added `--encoder` option with `mpp` (hardware) and `openh264` / `x264`.
  - Added `--shm-format` support for `NV21` and `YUYV`.
  - Added `--swap-uv` option (for NV12/NV21).
  - When `encoder=mpp`, caps are kept in NV12 for `mpph264enc`.
- `ipc/run_rk3566_camera_daemon.sh`
  - Added `SOURCE_FORMAT`, `STREAM_FORMAT`, `RTSP_SHM_FORMAT`, `RTSP_ENCODER`.
  - Added `USE_RGA` and `RTSP_SWAP_UV`.
  - If `USE_RGA=1`, sets `GST_PLUGIN_PATH` for `/usr/local` plugins.

### Device Packages Installed
Installed via apt:
- `gstreamer1.0-rockchip1`
- `librockchip-vpu0`
- `rockchip-mpp-demos`
- `librockchip-mpp-dev`
- build deps: `build-essential cmake meson ninja-build pkg-config git libdrm-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev`

Already present (or updated):
- `librga2`, `librockchip-mpp1`

### Built from Source on Device
1) **librga** from official Rockchip repo
   - Repo: https://github.com/airockchip/librga
   - No CMake build files; used prebuilt `libs/Linux/gcc-aarch64`.
   - Installed to `/usr/local/lib` and `/usr/local/include/rga`.

2) **GStreamer RGA convert plugin**
   - Repo: https://github.com/higithubhi/gstreamer-rgaconvert
   - Built with Meson/Ninja.
   - Meson `install` failed due to install.dat mismatch, so plugin was **manually installed**:
     - `/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0/libgstrgaconvert.so`
   - Verified with:
     - `GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0 gst-inspect-1.0 rgaconvert`
   - **Limitation**: rgaconvert **does not accept UYVY** input.

### RGA / MPP Availability (post-install)
- `mpph264enc` present (hardware encoder).
- `rgaconvert` present (RGA conversion plugin).
- `rkisp` source present but segfaults on `gst-launch-1.0`.

### Pipeline Experiments
1) **NV12 RTSP baseline (software)**
   - Still red cast.
2) **NV21 stream / swap UV**
   - Reduced red but still off.
3) **UYVY stream to RTSP (software)**
   - Still red cast.
4) **Hardware MPP encoder**
   - RTSP is hardware-encoded (H.264 High).
   - Red cast still present.
5) **RGA conversion path**
   - Fails because rgaconvert does not accept UYVY input.

### rkisp Probe Result
`gst-launch-1.0 rkisp ...` segfaults immediately on device:
- `Setting pipeline to PAUSED ... Caught SIGSEGV`
So rkisp path could not be validated.

### Current Known-Limited Options
- ISP mainpath NV12 is broken in kernel/MI path (see `doc/aiq/KEY_POINTS.md`).
- Direct UYVY output works; conversion pipeline appears to introduce color shift.
- GPU conversion is blocked until either:
  - RGA can accept UYVY (not supported by `rgaconvert`), or
  - rkisp can output NV12 without crash.

### 2026-01-31 Update (RGA path fixed)
- **rgaconvert now accepts UYVY and works with v4l2src**.
- Changes made **on device** in `gstreamer-rgaconvert`:
  - Added UYVY caps and mapping to `RK_FORMAT_UYVY_422`.
  - Added packed YUV422 stride handling (`pixel_stride=2` for UYVY/YUYV/VYUY/YVYU).
  - Ensured `fd=-1` when using mapped (non-dmabuf) buffers.
- Verification pipelines (device):
  ```
  GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0 \
    gst-launch-1.0 -v v4l2src device=/dev/video8 io-mode=2 num-buffers=30 \
    ! video/x-raw,format=UYVY,width=640,height=480,framerate=30/1 \
    ! rgaconvert ! video/x-raw,format=NV12,width=640,height=480 ! fakesink
  ```
  ```
  GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0 \
    gst-launch-1.0 -v v4l2src device=/dev/video8 io-mode=2 num-buffers=30 \
    ! video/x-raw,format=UYVY,width=1280,height=960,framerate=30/1 \
    ! rgaconvert ! video/x-raw,format=NV12,width=1280,height=960 ! fakesink
  ```
- With `USE_RGA=1`, the RTSP daemon starts cleanly at 1280x960 and shm sockets are created.

### Useful Commands (device)
GStreamer plugin check:
```
gst-inspect-1.0 mpph264enc
GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0 gst-inspect-1.0 rgaconvert
gst-inspect-1.0 rkisp
```

RGA plugin location:
```
/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0/libgstrgaconvert.so
```

Test capture (normal ISP):
```
v4l2-ctl -d /dev/video8 \
  --set-fmt-video=width=640,height=480,pixelformat=NV12 \
  --stream-mmap --stream-count=5 --stream-to=/tmp/nv12_aiq.raw
```

### Next Steps (recommended)
1) Fix or replace `rgaconvert` so it can handle UYVY (or add a UYVY->NV12 stage before RGA).
2) Debug `rkisp` segfaults; if rkisp can output NV12 directly, use it as the source.
3) Address kernel NV12 path on mainpath (as noted in `doc/aiq/KEY_POINTS.md`).
4) Once NV12 is reliable from ISP, use `mpph264enc` to keep pipeline fully HW-accelerated.
