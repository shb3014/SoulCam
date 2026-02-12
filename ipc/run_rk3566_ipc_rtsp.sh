#!/usr/bin/env bash
set -euo pipefail

# Run on the RK3566 device.
#
# This script:
# - Programs the media graph (resolution) and sensor timing (FPS) for OV5647→RKISP
# - Starts an RTSP server that VLC can open
#
# Default stream:
#   rtsp://<device-ip>:8554/cam

DEVICE="${DEVICE:-/dev/video8}"              # rkisp mainpath

# Sensor mode presets.
# OV5647 commonly tops out around:
# - 2592x1944: ~15 fps
# - 1296x972:  ~30 fps (4:3, full FOV)
# - 1920x1080: ~30 fps (16:9, cropped vs full 4:3 sensor)
#
# Keep defaults on "quality" (stable/full-res input). If you want *true* 30fps motion,
# use SENSOR_PRESET=fast4_3 (recommended) or SENSOR_PRESET=fast16_9.
SENSOR_PRESET="${SENSOR_PRESET:-fast4_3}"   # quality | fast4_3 | fast16_9

case "${SENSOR_PRESET}" in
  quality)
    SENSOR_WIDTH="${SENSOR_WIDTH:-2592}"
    SENSOR_HEIGHT="${SENSOR_HEIGHT:-1944}"
    ;;
  fast4_3)
    SENSOR_WIDTH="${SENSOR_WIDTH:-1296}"
    SENSOR_HEIGHT="${SENSOR_HEIGHT:-972}"
    ;;
  fast16_9)
    SENSOR_WIDTH="${SENSOR_WIDTH:-1920}"
    SENSOR_HEIGHT="${SENSOR_HEIGHT:-1080}"
    ;;
  *)
    echo "ERROR: Unknown SENSOR_PRESET='${SENSOR_PRESET}' (expected: quality|fast4_3|fast16_9)" >&2
    exit 2
    ;;
esac

WIDTH="${WIDTH:-1296}"
HEIGHT="${HEIGHT:-972}"

FPS="${FPS:-30}"
# With OV5647 native mode, FPS is typically limited to 15/30 depending on mode/timing.
# Use VBLANK to tune if needed; leave default unless you observe wrong FPS.
VBLANK="${VBLANK:-24}"
BITRATE_KBPS="${BITRATE_KBPS:-4000}"
PORT="${PORT:-8554}"
MOUNT="${MOUNT:-/cam}"

# Optional RTSP pipeline flags (passed to rk3566_rtsp_server.py)
# - SOURCE_FPS_CAPS=1 requests FPS at v4l2src caps (can help achieve true 30fps; can also break negotiation)
# - NO_VIDEORATE=1 disables videorate (avoids duplicated frames; useful to validate true FPS)
SOURCE_FPS_CAPS="${SOURCE_FPS_CAPS:-0}"
NO_VIDEORATE="${NO_VIDEORATE:-0}"

echo "=== SoulCam IPC RTSP server ==="
echo "Device: ${DEVICE}"
echo "Sensor preset: ${SENSOR_PRESET} (${SENSOR_WIDTH}x${SENSOR_HEIGHT})"
echo "Mode: ${WIDTH}x${HEIGHT} @ ${FPS} fps"
echo "RTSP: rtsp://0.0.0.0:${PORT}${MOUNT}"
echo

if [[ "${SENSOR_WIDTH}x${SENSOR_HEIGHT}" == "2592x1944" && "${FPS}" -gt 20 ]]; then
  echo "NOTE: OV5647 at 2592x1944 is commonly ~15fps."
  echo "      For true 30fps motion, try: SENSOR_PRESET=fast4_3 (1296x972) or fast16_9 (1920x1080)."
  echo
fi

echo "[1/3] Configure media pipeline..."
media-ctl -d /dev/media1 --set-v4l2 "\"m00_b_ov5647 1-0036\":0[fmt:SGBRG10_1X10/${SENSOR_WIDTH}x${SENSOR_HEIGHT} field:none]"
media-ctl -d /dev/media1 --set-v4l2 "\"rockchip-csi2-dphy1\":0[fmt:SGBRG10_1X10/${SENSOR_WIDTH}x${SENSOR_HEIGHT} field:none]"
media-ctl -d /dev/media1 --set-v4l2 "\"rkisp-csi-subdev\":0[fmt:SGBRG10_1X10/${SENSOR_WIDTH}x${SENSOR_HEIGHT} field:none]"

# Important: rkisp may keep an old crop unless we set it explicitly.
media-ctl -d /dev/media1 --set-v4l2 "\"rkisp-isp-subdev\":0[fmt:SGBRG10_1X10/${SENSOR_WIDTH}x${SENSOR_HEIGHT} crop:(0,0)/${SENSOR_WIDTH}x${SENSOR_HEIGHT} field:none]"

# Set ISP output pad to the desired stream size (so /dev/video8 can be set to WIDTHxHEIGHT).
# Use YUYV on pad2; /dev/video8 can output UYVY/NV12 etc depending on consumers.
media-ctl -d /dev/media1 --set-v4l2 "\"rkisp-isp-subdev\":2[fmt:YUYV8_2X8/${WIDTH}x${HEIGHT} crop:(0,0)/${WIDTH}x${HEIGHT}]"

# IMPORTANT: /dev/video8 may keep an old V4L2 crop (commonly 640x480), which makes clients show
# only the top-left portion of the image even when the format is set to 1280x960.
# To avoid the “top-left corner only” symptom, set crop to the *full sensor bounds* and let
# the ISP scale down to WIDTHxHEIGHT.
v4l2-ctl -d "${DEVICE}" --set-fmt-video=width="${WIDTH}",height="${HEIGHT}",pixelformat=UYVY || true
v4l2-ctl -d "${DEVICE}" --set-selection=target=crop,left=0,top=0,width="${SENSOR_WIDTH}",height="${SENSOR_HEIGHT}" || true

echo "[2/3] Configure sensor timing (vertical_blanking=${VBLANK})..."
v4l2-ctl -d /dev/v4l-subdev3 --set-ctrl=vertical_blanking="${VBLANK}" || true

echo "[3/3] Start RTSP server..."
EXTRA_ARGS=()
if [[ "${SOURCE_FPS_CAPS}" == "1" ]]; then
  EXTRA_ARGS+=("--source-fps-caps")
fi
if [[ "${NO_VIDEORATE}" == "1" ]]; then
  EXTRA_ARGS+=("--no-videorate")
fi

exec python3 "$(dirname "$0")/rk3566_rtsp_server.py" \
  --device "${DEVICE}" \
  --width "${WIDTH}" \
  --height "${HEIGHT}" \
  --fps "${FPS}" \
  --bitrate-kbps "${BITRATE_KBPS}" \
  --port "${PORT}" \
  --mount "${MOUNT}" \
  "${EXTRA_ARGS[@]}"

