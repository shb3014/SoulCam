# SoulCam Commands (Consolidated)

Target device: `192.168.1.45`  
SSH user: `ubuntu`  

This file consolidates SoulCam-related commands from:
- `doc/cmd/SOULCAM_CPP_FRAMEWORK_CMDS_2026-02-09.md`
- `rive-runtime/docs/CMDS.md` (SoulCam-relevant parts)
- Current native Soullink implementation (`src/main.cpp`, `src/soullink/module.cpp`)

---

## 1) Connect / Sync / Build

```bash
cd /home/shb3014/embeddedProjects/SoulCam

# Sync source to device
sshpass -p 'shb084ww' rsync -avz --progress \
  --exclude='build/' --exclude='.git/' --exclude='__pycache__/' \
  --exclude='*.o' --exclude='*.pyc' \
  -e "ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null" \
  src/ scripts/ rknn/ \
  ubuntu@192.168.1.45:/home/ubuntu/SoulCam/
```

```bash
# Build on device
sshpass -p 'shb084ww' ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ubuntu@192.168.1.45
cd /home/ubuntu/SoulCam
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j4
```

```bash
# Optional helper script
cd /home/ubuntu/SoulCam
bash scripts/build.sh
```

---

## 2) Run SoulCam

```bash
# RTSP only
./build/soulcam

# Verbose
./build/soulcam -v

# RTSP + AI
./build/soulcam --ai --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn

# RTSP + AI + overlay
./build/soulcam --ai --overlay --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn

# DMA-BUF zero-copy
./build/soulcam --dmabuf
./build/soulcam --dmabuf --ai --model /home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn
```

```bash
# Stop
pkill -f soulcam
```

---

## 3) Verify RTSP

```bash
ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.45:8554/cam -t 8 -f null -

ffprobe -rtsp_transport tcp -v quiet -print_format json \
  -show_streams rtsp://192.168.1.45:8554/cam

ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay \
  -framedrop rtsp://192.168.1.45:8554/cam
```

---

## 4) Services / Logs

```bash
sudo cp service/soulcam.service /etc/systemd/system/
sudo cp service/scene_hub.service /etc/systemd/system/
sudo systemctl daemon-reload

sudo systemctl enable soulcam scene_hub
sudo systemctl start soulcam
sudo systemctl stop soulcam
sudo systemctl status soulcam

journalctl -u soulcam -f
journalctl -u scene_hub -f
```

---

## 5) Runtime Control Socket Commands

Control socket default: `/tmp/soulcam_ctrl.sock`

```bash
# Ping / status
echo '{"cmd":"ping"}' | socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock
echo '{"cmd":"status"}' | socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Swap primary model (slot 0)
echo '{"cmd":"swap_model","path":"/home/ubuntu/YoloV8-NPU/rk3566/yolov8s.rknn"}' | \
  socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Swap model in a specific slot
echo '{"cmd":"swap_model","slot":1,"path":"/home/ubuntu/models/hand.rknn","conf":0.10,"nms":0.45}' | \
  socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Add / remove / list models
echo '{"cmd":"add_model","path":"/home/ubuntu/models/extra.rknn","skip":2,"weight":1}' | \
  socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock
echo '{"cmd":"remove_model","slot":1}' | socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock
echo '{"cmd":"list_models"}' | socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Enable/disable model slot
echo '{"cmd":"enable_model","slot":1,"enable":false}' | socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock

# Debug model scheduler/runtime
echo '{"cmd":"debug_models"}' | socat - UNIX-SENDTO:/tmp/soulcam_ctrl.sock
```

Python fallback if `socat` is unavailable:
```bash
python3 -c "import socket; s=socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); s.sendto(b'{\"cmd\":\"status\"}', '/tmp/soulcam_ctrl.sock')"
```

---

## 6) Soullink CLI Options (Native C++ Module)

```bash
# Enable/disable
--soullink
--no-soullink

# Identity / discovery
--soullink-service-id <id>
--soullink-mdns <service-type>             # default: _soulcamDebug._tcp.local
--soullink-mdns-refresh <sec>              # default: 5 (periodic re-announce)
--soullink-api-port <port>                 # default: 5212

# MQTT broker / topics
--soullink-mqtt-host <host>                # default: 127.0.0.1
--soullink-mqtt-port <port>                # default: 1883
--soullink-mqtt-prefix <prefix>            # default: soulcam/debug/
--soullink-mqtt-user <username>
--soullink-mqtt-pass <password>

# Stream/sync
--soullink-stream-port <port>              # default: 1234 (TCP JPEG ingress)
--soullink-stream-index <index>            # default: 0
--soullink-sync-root <path>
--soullink-sync-state <path>

# Client ID mode
--soullink-compat-id                       # default mode: <serviceIdentifier>
--soullink-composite-id                    # mode: soulcam:<serviceIdentifier>
```

Example:
```bash
./build/soulcam --snapshot \
  --soullink \
  --soullink-service-id ubuntu-6f470995 \
  --soullink-mqtt-host 192.168.1.100 \
  --soullink-mqtt-port 1883 \
  --soullink-api-port 5212 \
  --soullink-stream-port 1234
```

---

## 7) Soullink HTTP Handshake

The native module exposes:
- `GET /debug` on `--soullink-api-port` (default `5212`)
- optional override: `GET /debug?ip=<host-ip>`

Use it to retarget device MQTT broker at runtime (auto uses requester source IP):

```bash
curl "http://192.168.1.45:5212/debug"
# response: ok
```

---

## 8) Soullink MQTT Topics

Topic prefix default: `soulcam/debug/`

- Downlink (commands from host to device):
  - `soulcam/debug/in/<serviceIdentifier>`
  - `soulcam/debug/in/<clientId>` (compat subscription)
- Uplink:
  - DP: `soulcam/debug/out/<clientId>`
  - Message/notification/progress: `soulcam/debug/m/<clientId>`
  - Stream state binary: `soulcam/debug/s/<clientId>/<streamIndex>`

---

## 9) Soullink SoulCmd Commands (Supported in Module)

Command payload shape:
```json
{"cmd": <number|string>, "data": ...}
```

### Numeric command IDs

```text
0   setDp
1   getDp
2   getDpAll
4   subStream
5   unsubStream
12  disconnectServer         (currently reports warning/not implemented)
13  syncFiles
14  soulReload               (currently reports warning/not implemented)
18  streaming
20  directorPlay             (currently reports warning/not implemented)
21  sysCmd                   (currently reports warning/not implemented)
```

### MQTT test examples

```bash
# Listen uplink
mosquitto_sub -h 192.168.1.100 -t 'soulcam/debug/out/#' -v
mosquitto_sub -h 192.168.1.100 -t 'soulcam/debug/m/#' -v
mosquitto_sub -h 192.168.1.100 -t 'soulcam/debug/s/#' -v

# setDp
mosquitto_pub -h 192.168.1.100 \
  -t 'soulcam/debug/in/ubuntu-6f470995' \
  -m '{"cmd":0,"data":[{"dp":1001,"value":1}]}'

# getDp
mosquitto_pub -h 192.168.1.100 \
  -t 'soulcam/debug/in/ubuntu-6f470995' \
  -m '{"cmd":1,"data":[{"dp":1001}]}'

# getDpAll
mosquitto_pub -h 192.168.1.100 \
  -t 'soulcam/debug/in/ubuntu-6f470995' \
  -m '{"cmd":2,"data":{}}'

# subStream / unsubStream / streaming
mosquitto_pub -h 192.168.1.100 \
  -t 'soulcam/debug/in/ubuntu-6f470995' \
  -m '{"cmd":4,"data":{"index":0}}'
mosquitto_pub -h 192.168.1.100 \
  -t 'soulcam/debug/in/ubuntu-6f470995' \
  -m '{"cmd":5,"data":{"index":0}}'
mosquitto_pub -h 192.168.1.100 \
  -t 'soulcam/debug/in/ubuntu-6f470995' \
  -m '{"cmd":18,"data":{"index":0}}'

# syncFiles
mosquitto_pub -h 192.168.1.100 \
  -t 'soulcam/debug/in/ubuntu-6f470995' \
  -m '{"cmd":13,"data":{"api":"http://192.168.1.100:3000"}}'
```

---

## 10) Soullink Health / Discovery Checks

```bash
# mDNS advertise process
pgrep -a avahi-publish-service

# API endpoint alive
curl -v "http://192.168.1.45:5212/debug"

# Soullink logs
journalctl -u soulcam -f | rg 'Soullink|MQTT|mDNS|sync'
```

---

## 11) SoulCam + Tracker (From rive-runtime CMDS, SoulCam-relevant)

```bash
# Set GPU governor (device)
echo shb084ww | sudo -S sh -c 'echo performance > /sys/devices/platform/fde60000.gpu/devfreq/fde60000.gpu/governor'

# Start SoulCam with AI person detection
cd ~/SoulCam
sudo systemctl stop soulcam
sudo ./build/soulcam --ai --model YoloV8-NPU/rk3566/yolov8n.rknn

# Start SoulCam with hand model
sudo ./build/soulcam --ai \
  --model /home/ubuntu/models/hand_yolov8n_rk3566_i8_20260301.rknn \
  --labels hand --conf 0.10
```

---

## 12) Useful One-Liners

```bash
# Show full SoulCam CLI help
./build/soulcam --help

# Check soulcam process + listening ports
pgrep -a soulcam
ss -lntp | rg '8554|5212|1234|1883'

# Quick health view from logs
journalctl -u soulcam -n 100 --no-pager
```

