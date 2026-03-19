# SoulCam Build and Deployment

Technical reference for building, deploying, and managing SoulCam on
the RK3566 target device. SoulCam builds natively on-device (no cross-compilation).

---

## 1. Target Device

| Property | Value |
|----------|-------|
| SoC | Rockchip RK3566 (4x Cortex-A55 + Mali-G52 + 0.8 TOPS NPU) |
| Board | Orange Pi 3B (or similar RK3566 SBC) |
| OS | Ubuntu 24.04 (aarch64) |
| IP | `192.168.1.45` |
| SSH user | `ubuntu` |
| Project path (device) | `/home/ubuntu/SoulCam/` |
| Project path (host) | `/home/shb3014/embeddedProjects/SoulCam/` |
| Binary | `/home/ubuntu/SoulCam/build/soulcam` |

---

## 2. Prerequisites (one-time device setup)

```bash
sudo apt install -y \
    build-essential cmake pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-rtsp \
    libgstrtspserver-1.0-dev \
    libmosquitto-dev \
    libcairo2-dev \
    lld
```

Optional dependencies (auto-detected by CMake):
- **OpenCV** -- debug visualization (`libopencv-dev`)
- **Rive runtime** -- GPU-rendered animations (pre-built at `rive-runtime/`)
- **Tuya IPC SDK** -- cloud connectivity (at `tuya_sdk/`)

---

## 3. Build System

### 3.1 CMake layout

The `CMakeLists.txt` lives in `src/` (not the project root). CMake is
invoked with `src/` as the source directory and `build/` as the binary
directory.

```
SoulCam/
├── src/
│   ├── CMakeLists.txt        # main build definition
│   ├── main.cpp
│   ├── soulcam.h
│   ├── pipeline/             # GStreamer pipelines, AI capture, RTSP
│   ├── ai/                   # RKNN detector, tracker, model pipeline
│   ├── store/                # persistent config (DPs, store.json)
│   ├── soullink/             # MQTT module, sync engine
│   └── util/                 # logger
├── build/                    # CMake output (on-device only)
├── scripts/
│   ├── build.sh              # on-device build script
│   └── deploy.sh             # host→device sync + build
├── rknn/
│   ├── include/rknn_api.h    # RKNN SDK headers
│   └── lib/librknnrt.so      # RKNN runtime library
├── service/
│   └── soulcam.service       # systemd unit file
└── rive-runtime/             # Rive renderer (submodule)
```

### 3.2 CMake options

| Variable | Default | Description |
|----------|---------|-------------|
| `CMAKE_BUILD_TYPE` | `Release` | `Release` (-O2) or `Debug` (-O0 -g) |
| `RKNN_DIR` | `../rknn` | Path to RKNN SDK (with `include/` and `lib/`) |
| `RIVE_RUNTIME_DIR` | `../rive-runtime` | Path to Rive renderer source tree |
| `TUYA_SDK_DIR` | `../tuya_sdk` | Path to Tuya IPC SDK |

### 3.3 Build output

Single binary: `build/soulcam` (~5-10 MB depending on link-time Rive inclusion).
Statically links Rive; dynamically links GStreamer, RKNN, Cairo, Mosquitto.

---

## 4. Building

### 4.1 On-device (via scripts/build.sh)

```bash
ssh ubuntu@192.168.1.45
cd /home/ubuntu/SoulCam
bash scripts/build.sh            # Release build
bash scripts/build.sh debug      # Debug build
bash scripts/build.sh clean      # Clean rebuild
```

Build time: ~2-7 minutes (4x A55 at 1.8 GHz). Linking with Rive
(LLD + static LLVM bitcode) is the slowest step (~2-3 minutes).

### 4.2 Manual CMake

```bash
cd /home/ubuntu/SoulCam
mkdir -p build && cd build
cmake ../src -DCMAKE_BUILD_TYPE=Release -DRKNN_DIR=/home/ubuntu/SoulCam/rknn
cmake --build . -j$(nproc)
```

---

## 5. Deployment (host → device)

### 5.1 Quick deploy (incremental, source directories only)

For iterative development, sync only the directories that contain source
and config files. Fast (~2-5 seconds for changed files):

```bash
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"

sshpass -p 'shb084ww' rsync -avz \
  -e "ssh $SSH_OPTS" \
  /home/shb3014/embeddedProjects/SoulCam/src/ \
  ubuntu@192.168.1.45:/home/ubuntu/SoulCam/src/

sshpass -p 'shb084ww' rsync -avz \
  -e "ssh $SSH_OPTS" \
  /home/shb3014/embeddedProjects/SoulCam/scripts/ \
  ubuntu@192.168.1.45:/home/ubuntu/SoulCam/scripts/

sshpass -p 'shb084ww' rsync -avz \
  -e "ssh $SSH_OPTS" \
  /home/shb3014/embeddedProjects/SoulCam/soullink/ \
  ubuntu@192.168.1.45:/home/ubuntu/SoulCam/soullink/
```

Then build on device:

```bash
sshpass -p 'shb084ww' ssh $SSH_OPTS \
  ubuntu@192.168.1.45 "cd /home/ubuntu/SoulCam && bash scripts/build.sh"
```

### 5.2 Full deploy (scripts/deploy.sh)

Syncs the entire project tree (excluding `build/`, `.git/`, large dirs)
and builds on device. Slower on first run due to submodule data
(rive-runtime, CubismNativeSamples):

```bash
./scripts/deploy.sh                    # default: 192.168.1.45
./scripts/deploy.sh 192.168.1.100      # custom IP
./scripts/deploy.sh 192.168.1.100 pi   # custom user
```

The script uses SSH key auth. For password auth, use `sshpass`:

```bash
sshpass -p 'shb084ww' ./scripts/deploy.sh
```

### 5.3 Full deploy with sshpass (manual)

```bash
sshpass -p 'shb084ww' rsync -avz --progress \
  --exclude='build/' --exclude='kernel_*/' \
  --exclude='ubuntu-rockchip-*/' --exclude='device_backups/' \
  --exclude='old/' --exclude='.git/' --exclude='__pycache__/' \
  --exclude='*.o' --exclude='*.so' \
  -e "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null" \
  /home/shb3014/embeddedProjects/SoulCam/ \
  ubuntu@192.168.1.45:/home/ubuntu/SoulCam/
```

**Warning:** The first full sync transfers rive-runtime (~12K files)
and CubismNativeSamples. This can take 5-10 minutes. Use the incremental
method (5.1) for iterative development.

---

## 6. Systemd Service

### 6.1 Service file

Located at `service/soulcam.service`. Key configuration:

```ini
[Service]
User=ubuntu
WorkingDirectory=/home/ubuntu/SoulCam
Environment=GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0
Environment=LD_LIBRARY_PATH=/opt/mesa-pls/lib/aarch64-linux-gnu
ExecStart=/home/ubuntu/SoulCam/build/soulcam --snapshot
Restart=on-failure
RestartSec=3
```

The `ExecStart` line controls which features are enabled at boot.
Comment/uncomment the desired line in the service file. Common
configurations:

| Configuration | ExecStart flags |
|---------------|----------------|
| RTSP + Snapshot (default) | `--snapshot` |
| RTSP + AI + Snapshot | `--ai --snapshot --model /path/to/model.rknn` |
| RTSP + AI + Overlay | `--ai --overlay --snapshot --model /path/to/model.rknn` |
| Full (AI + Overlay + ONVIF) | `--ai --overlay --onvif --snapshot --model /path/to/model.rknn` |

### 6.2 Service management

```bash
# Install (first time)
sudo cp service/soulcam.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable soulcam

# Daily operations
sudo systemctl start soulcam
sudo systemctl stop soulcam
sudo systemctl restart soulcam
sudo systemctl status soulcam

# View logs
sudo journalctl -u soulcam -f                    # follow live
sudo journalctl -u soulcam --since "30s ago"     # recent
sudo journalctl -u soulcam -n 200 --no-pager     # last 200 lines
```

### 6.3 Remote service management (from host)

```bash
SSH="sshpass -p 'shb084ww' ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ubuntu@192.168.1.45"

# Restart after deploy
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"

# Check status
$SSH "echo 'shb084ww' | sudo -S systemctl status soulcam 2>&1 | head -20"

# Tail logs
$SSH "echo 'shb084ww' | sudo -S journalctl -u soulcam --since '15s ago' --no-pager 2>&1"
```

---

## 7. Runtime Configuration (no rebuild needed)

Most SoulCam parameters are exposed as DPs (Data Points) and can be
changed at runtime via SoulLink `setDp` commands (MQTT or SoulFlow).
Changes persist to `/var/lib/soulcam/store.json` and survive restarts.

```bash
# Edit store.json directly on device (takes effect on next restart)
sudo nano /var/lib/soulcam/store.json

# Or use MQTT setDp for immediate effect (no restart)
mosquitto_pub -h <broker> -t 'soulcam/debug/in/<id>' \
  -m '{"cmd":0,"data":[{"dp":18,"value":4}]}'
```

See `soullink/docs/dp_catalog.md` for the full DP reference.

---

## 8. Model Management

RKNN models live on the device filesystem. The primary model and hand model
paths are configured via DPs or CLI flags.

### 8.1 Model locations

| Model | Device path | DP |
|-------|------------|----|
| Primary (person) | `/home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn` | `ai_model_path` (102) |
| Hand | `/home/ubuntu/models/hand_yolov8n_rk3566_i8_20260301.rknn` | `model2_path` (105) |

### 8.2 Hot-swap models at runtime

No rebuild or restart needed:

```bash
# Via control socket
echo '{"cmd":"swap_model","slot":1,"path":"/home/ubuntu/models/new_hand.rknn","conf":0.10}' | \
  socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Via SoulLink sysCmd
# (MQTT) {"cmd":21,"data":{"subcmd":7,"slot":1,"path":"/home/ubuntu/models/new_hand.rknn","conf":0.10}}
```

### 8.3 Upload a model from host

```bash
sshpass -p 'shb084ww' scp -o StrictHostKeyChecking=no \
  /path/to/new_model.rknn \
  ubuntu@192.168.1.45:/home/ubuntu/models/
```

---

## 9. Typical Development Workflow

```
 Host (WSL/Linux)                    Target (RK3566)
 ────────────────                    ───────────────
 1. Edit source files
     ↓
 2. rsync src/ → device              ← receives files
     ↓
 3. SSH: bash scripts/build.sh       → cmake + make -j4
     ↓                                   (~2-7 min)
 4. SSH: sudo systemctl restart      → soulcam restarts
     ↓
 5. Check logs / test stream         ← journalctl -u soulcam -f
     ↓
 6. Iterate (back to step 1)
```

### One-liner: sync + build + restart + logs

```bash
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
PASS="shb084ww"
DEVICE="ubuntu@192.168.1.45"
REMOTE="/home/ubuntu/SoulCam"

sshpass -p "$PASS" rsync -avz -e "ssh $SSH_OPTS" \
  src/ scripts/ soullink/ \
  "$DEVICE:$REMOTE/" && \
sshpass -p "$PASS" ssh $SSH_OPTS "$DEVICE" \
  "cd $REMOTE && bash scripts/build.sh && echo $PASS | sudo -S systemctl restart soulcam" && \
sleep 5 && \
sshpass -p "$PASS" ssh $SSH_OPTS "$DEVICE" \
  "echo $PASS | sudo -S journalctl -u soulcam --since '10s ago' --no-pager 2>&1"
```

---

## 10. Verifying the Build

### 10.1 RTSP stream test (from host)

```bash
# Probe stream info
ffprobe -rtsp_transport tcp -v quiet -print_format json \
  -show_streams rtsp://192.168.1.45:8554/cam

# Low-latency playback
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay \
  -framedrop rtsp://192.168.1.45:8554/cam

# Record 8 seconds to null (verify no errors)
ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.45:8554/cam -t 8 -f null -
```

### 10.2 AI pipeline verification

Check startup logs for model loading and FPS:

```bash
sudo journalctl -u soulcam --since "30s ago" | grep -E "(slot|weight|AI pipeline|TargetPick|Target policy)"
```

Expected output:
```
ModelPipeline: slot 0 [yolov8n] loaded (...)
ModelPipeline: slot 1 [hand_yolov8n...] loaded (...)
Target policy: enabled (hand_slot=1, person_slot=0, ...)
AI pipeline: 14.6 fps total (YOLO 3.5 fps + tracker 11.0 fps, ...)
```

### 10.3 Snapshot endpoint

```bash
curl -o /tmp/snap.jpg http://192.168.1.45:8088/snapshot
```

---

## 11. Troubleshooting

### Build fails: "gstreamer-rtsp-server-1.0 not found"

```bash
sudo apt install libgstrtspserver-1.0-dev
# Some Rockchip BSPs use: gstreamer1.0-rtsp-devel
```

### Build fails: linking takes forever / OOM killed

Linking with Rive (LLD + LLVM bitcode) requires ~1.5 GB RAM. If the
device has < 2 GB, add swap:

```bash
sudo fallocate -l 2G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
```

### Service starts but RTSP stream is black

1. Check ISP is running: `sudo systemctl status rkaiq-3a`
2. Check sensor: `v4l2-ctl -d /dev/video8 --all`
3. Check GStreamer plugin path: `echo $GST_PLUGIN_PATH`

### rsync hangs during full project sync

The rive-runtime submodule contains ~12K files. Use incremental sync
(section 5.1) for iterative development. Only run the full sync when
submodule content changes.

### "Permission denied" on sudo via SSH

Use the `echo password | sudo -S` pattern for non-interactive sudo:

```bash
sshpass -p 'shb084ww' ssh ... "echo 'shb084ww' | sudo -S systemctl restart soulcam"
```
