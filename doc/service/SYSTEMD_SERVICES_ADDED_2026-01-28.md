# Systemd Services Added (RK3566 / OV5647)

Target: `192.168.1.45` (user: `ubuntu`)

## rkaiq-3a.service
- Purpose: keep AIQ 3A server running for auto exposure/awb/gain.
- Unit file: `/etc/systemd/system/rkaiq-3a.service`
- Command: `/usr/local/bin/rkaiq_3A_server --silent`
- Status: `systemctl is-active rkaiq-3a.service` should be `active`.
- Notes:
  - Recent builds skip `/dev/media0` and only bind the first valid media node.
  - Set `RKAIQ_MEDIA_ALL=1` to scan all `/dev/media*` nodes.

## rkisp-auto-controls.service
- Purpose: apply auto exposure/gain/awb controls at boot.
- Unit file: `/etc/systemd/system/rkisp-auto-controls.service`
- Script: `/usr/local/bin/rkisp-auto-controls.sh`
- Command: `v4l2-ctl -d /dev/video8 --set-ctrl=auto_exposure=0 --set-ctrl=gain_automatic=1 --set-ctrl=white_balance_automatic=1`
- Status: one‑shot (expected to be `inactive (dead)` after success).

## soulcam.service (planned)
- Purpose: start the SoulCam C++ RTSP server at boot (after ISP is ready).
- Binary: `/home/ubuntu/SoulCam/build/soulcam`
- Depends on: `rkaiq-3a.service`, `rkisp-media-setup.service`
- Example unit file:
  ```ini
  [Unit]
  Description=SoulCam IP Camera
  After=rkaiq-3a.service rkisp-media-setup.service
  Wants=rkaiq-3a.service

  [Service]
  Type=simple
  Environment=GST_PLUGIN_PATH=/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0
  ExecStart=/home/ubuntu/SoulCam/build/soulcam
  Restart=on-failure
  RestartSec=3

  [Install]
  WantedBy=multi-user.target
  ```
- Status: not yet created (to be added when framework is stable).
