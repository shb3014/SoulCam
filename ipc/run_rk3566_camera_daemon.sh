#!/usr/bin/env bash
set -euo pipefail

# Option C launcher:
# - Configure OV5647 -> RKISP media graph (same as the RTSP script)
# - Start a capture daemon that publishes shared-memory raw feeds:
#     * /tmp/soulcam_stream.sock : NV12 @ stream resolution (for RTSP)
#     * /tmp/soulcam_cv.sock     : NV12 640x480 (for CV workers)
#   NOTE: RKISP NV12/NV21/NV16 planar outputs are currently broken (Y=0),
#   so we capture packed UYVY and convert to NV12 in the daemon.
# - Start RTSP server that reads from the shm "stream" feed (so RTSP does NOT touch /dev/video8)
#
# Default RTSP:
#   rtsp://<device-ip>:8554/cam

DEVICE="${DEVICE:-/dev/video8}"

SENSOR_PRESET="${SENSOR_PRESET:-fast4_3}" # quality | fast4_3 | fast16_9
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

STREAM_WIDTH="${STREAM_WIDTH:-1296}"
STREAM_HEIGHT="${STREAM_HEIGHT:-972}"
SOURCE_FORMAT="${SOURCE_FORMAT:-UYVY}"
STREAM_FORMAT="${STREAM_FORMAT:-NV12}"
USE_RGA="${USE_RGA:-0}"
FPS="${FPS:-30}"
VBLANK="${VBLANK:-24}"
SOURCE_FORMAT="${SOURCE_FORMAT^^}"
STREAM_FORMAT="${STREAM_FORMAT^^}"
if [[ "${SOURCE_FORMAT}" == "NV12" ]]; then
  if [[ "${STREAM_FORMAT}" != "NV12" ]]; then
    echo "WARN: SOURCE_FORMAT=NV12 but STREAM_FORMAT=${STREAM_FORMAT}; forcing STREAM_FORMAT=NV12"
    STREAM_FORMAT="NV12"
  fi
  if [[ "${USE_RGA}" == "1" ]]; then
    echo "WARN: SOURCE_FORMAT=NV12; disabling RGA to avoid color issues"
    USE_RGA="0"
  fi
fi
if [[ "${USE_RGA}" == "1" ]]; then
  export GST_PLUGIN_PATH="/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0:${GST_PLUGIN_PATH:-}"
  echo "RGA: enabled (rgaconvert)"
fi

USE_RGA_FLAG=""
if [[ "${USE_RGA}" == "1" ]]; then
  USE_RGA_FLAG="--use-rga"
fi

CV_WIDTH="${CV_WIDTH:-640}"
CV_HEIGHT="${CV_HEIGHT:-480}"

STREAM_SOCK="${STREAM_SOCK:-/tmp/soulcam_stream.sock}"
CV_SOCK="${CV_SOCK:-/tmp/soulcam_cv.sock}"
DEBUG_SOCK="${DEBUG_SOCK:-/tmp/soulcam_debug.sock}"
DEBUG_SCENE_SOCK="${DEBUG_SCENE_SOCK:-/tmp/soulcam_debug_scene.sock}"
DEBUG_SRC_SOCK="${DEBUG_SRC_SOCK:-/tmp/soulcam_cv_debug.sock}"
DEBUG_WIDTH="${DEBUG_WIDTH:-640}"
DEBUG_HEIGHT="${DEBUG_HEIGHT:-480}"
DEBUG_FPS="${DEBUG_FPS:-15}"
DAEMONIZE="${DAEMONIZE:-0}"
CAPTURE_TEST="${CAPTURE_TEST:-0}"
CAPTURE_COUNT="${CAPTURE_COUNT:-5}"
CAPTURE_DIR="${CAPTURE_DIR:-/dev/shm}"

BITRATE_KBPS="${BITRATE_KBPS:-4000}"
RTSP_ENCODER="${RTSP_ENCODER:-mpp}"
RTSP_SWAP_UV="${RTSP_SWAP_UV:-0}"
PORT="${PORT:-8554}"
MOUNT="${MOUNT:-/cam}"

SOURCE_FPS_CAPS="${SOURCE_FPS_CAPS:-0}"
NO_VIDEORATE="${NO_VIDEORATE:-0}"
DEBUG_OVERLAY="${DEBUG_OVERLAY:-0}"
START_RTSP="${START_RTSP:-1}"
RTSP_PID=""
DEBUG_PID=""

echo "=== SoulCam camera daemon (Option C) ==="
echo "Device: ${DEVICE}"
echo "Sensor preset: ${SENSOR_PRESET} (${SENSOR_WIDTH}x${SENSOR_HEIGHT})"
echo "Stream feed: ${STREAM_FORMAT} ${STREAM_WIDTH}x${STREAM_HEIGHT}@${FPS} -> ${STREAM_SOCK}"
echo "Source format: ${SOURCE_FORMAT}"
echo "CV feed:     NV12 ${CV_WIDTH}x${CV_HEIGHT}@${FPS} -> ${CV_SOCK} (${SOURCE_FORMAT}->NV12)"
if [[ "${DEBUG_OVERLAY}" == "1" ]]; then
  echo "Debug feed:  NV12 ${DEBUG_WIDTH}x${DEBUG_HEIGHT}@${DEBUG_FPS} -> ${DEBUG_SOCK}"
  echo "Debug src:   NV12 ${CV_WIDTH}x${CV_HEIGHT}@${FPS} -> ${DEBUG_SRC_SOCK}"
fi
if [[ "${START_RTSP}" == "1" ]]; then
  echo "RTSP: rtsp://0.0.0.0:${PORT}${MOUNT}"
else
  echo "RTSP: disabled (START_RTSP=0)"
fi
if [[ "${CAPTURE_TEST}" == "1" ]]; then
  echo "Capture test: enabled (CAPTURE_COUNT=${CAPTURE_COUNT}, CAPTURE_DIR=${CAPTURE_DIR})"
fi
echo

echo "Preflight: free ${DEVICE} (best-effort)..."
sudo -n fuser -k "${DEVICE}" >/dev/null 2>&1 || true
echo "Preflight: free RTSP port ${PORT} (best-effort)..."
sudo -n fuser -k "${PORT}/tcp" >/dev/null 2>&1 || true
pkill -f rk3566_rtsp_server.py >/dev/null 2>&1 || true

echo "[1/3] Configure media pipeline..."
media-ctl -d /dev/media1 --set-v4l2 "\"m00_b_ov5647 1-0036\":0[fmt:SGBRG10_1X10/${SENSOR_WIDTH}x${SENSOR_HEIGHT} field:none]" || true
media-ctl -d /dev/media1 --set-v4l2 "\"rockchip-csi2-dphy0\":0[fmt:SGBRG10_1X10/${SENSOR_WIDTH}x${SENSOR_HEIGHT} field:none]" || true
media-ctl -d /dev/media1 --set-v4l2 "\"rkisp-csi-subdev\":0[fmt:SGBRG10_1X10/${SENSOR_WIDTH}x${SENSOR_HEIGHT} field:none]" || true
media-ctl -d /dev/media1 --set-v4l2 "\"rkisp-isp-subdev\":0[fmt:SGBRG10_1X10/${SENSOR_WIDTH}x${SENSOR_HEIGHT} crop:(0,0)/${SENSOR_WIDTH}x${SENSOR_HEIGHT} field:none]" || true
ISP_OUT_FMT="YUYV8_2X8"
V4L2_OUT_FMT="UYVY"
if [[ "${SOURCE_FORMAT}" == "NV12" ]]; then
  ISP_OUT_FMT="NV12"
  V4L2_OUT_FMT="NV12"
fi
media-ctl -d /dev/media1 --set-v4l2 "\"rkisp-isp-subdev\":2[fmt:${ISP_OUT_FMT}/${STREAM_WIDTH}x${STREAM_HEIGHT} crop:(0,0)/${STREAM_WIDTH}x${STREAM_HEIGHT}]" || true

v4l2-ctl -d "${DEVICE}" --set-fmt-video=width="${STREAM_WIDTH}",height="${STREAM_HEIGHT}",pixelformat="${V4L2_OUT_FMT}" || true
v4l2-ctl -d "${DEVICE}" --set-selection=target=crop,left=0,top=0,width="${SENSOR_WIDTH}",height="${SENSOR_HEIGHT}" || true

echo "[2/3] Configure sensor timing (vertical_blanking=${VBLANK})..."
v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=vertical_blanking="${VBLANK}" || true

if [[ "${CAPTURE_TEST}" == "1" ]]; then
  echo "[2.5/3] Capture test: ISP UYVY + RGA NV12 (last frame only)..."
  ISP_RAW="${CAPTURE_DIR}/isp_uyvy.raw"
  ISP_LAST_RAW="${CAPTURE_DIR}/isp_uyvy_last.raw"
  ISP_PNG="${CAPTURE_DIR}/isp_uyvy.png"
  RGA_RAW="${CAPTURE_DIR}/rga_nv12.raw"
  RGA_LAST_RAW="${CAPTURE_DIR}/rga_nv12_last.raw"
  RGA_PNG="${CAPTURE_DIR}/rga_nv12.png"

  v4l2-ctl -d "${DEVICE}" \
    --set-fmt-video=width="${STREAM_WIDTH}",height="${STREAM_HEIGHT}",pixelformat=UYVY \
    --stream-mmap --stream-count="${CAPTURE_COUNT}" --stream-to="${ISP_RAW}"

  python3 - <<'PY'
import os
W,H=int(os.environ["STREAM_WIDTH"]),int(os.environ["STREAM_HEIGHT"])
frame_size=W*H*2
src=os.environ["ISP_RAW"]
out=os.environ["ISP_LAST_RAW"]
data=open(src,'rb').read()
frames=len(data)//frame_size
start=(frames-1)*frame_size
open(out,'wb').write(data[start:start+frame_size])
print("ISP frames",frames,"->",out)
PY

  ffmpeg -y -f rawvideo -pix_fmt uyvy422 -s "${STREAM_WIDTH}x${STREAM_HEIGHT}" \
    -i "${ISP_LAST_RAW}" -frames:v 1 "${ISP_PNG}"

  GST_PLUGIN_PATH="/usr/local/lib/aarch64-linux-gnu/gstreamer-1.0" \
    gst-launch-1.0 -v v4l2src device="${DEVICE}" io-mode=2 num-buffers="${CAPTURE_COUNT}" \
    ! video/x-raw,format=UYVY,width="${STREAM_WIDTH}",height="${STREAM_HEIGHT}",framerate="${FPS}/1" \
    ! rgaconvert ! video/x-raw,format=NV12,width="${STREAM_WIDTH}",height="${STREAM_HEIGHT}" \
    ! filesink location="${RGA_RAW}"

  python3 - <<'PY'
import os
W,H=int(os.environ["STREAM_WIDTH"]),int(os.environ["STREAM_HEIGHT"])
frame_size=W*H*3//2
src=os.environ["RGA_RAW"]
out=os.environ["RGA_LAST_RAW"]
data=open(src,'rb').read()
frames=len(data)//frame_size
start=(frames-1)*frame_size
open(out,'wb').write(data[start:start+frame_size])
print("RGA frames",frames,"->",out)
PY

  ffmpeg -y -f rawvideo -pix_fmt nv12 -s "${STREAM_WIDTH}x${STREAM_HEIGHT}" \
    -i "${RGA_LAST_RAW}" -frames:v 1 "${RGA_PNG}"

  echo "Capture test outputs:"
  echo "  ISP PNG: ${ISP_PNG}"
  echo "  RGA PNG: ${RGA_PNG}"
  exit 0
fi

echo "[3/3] Start capture daemon..."

CAPTURE_LOG="${CAPTURE_LOG:-/tmp/soulcam_capture.log}"
RTSP_LOG="${RTSP_LOG:-/tmp/soulcam_rtsp.log}"

python3 "$(dirname "$0")/rk3566_capture_shm.py" \
  --device "${DEVICE}" \
  --fps "${FPS}" \
  --stream-width "${STREAM_WIDTH}" \
  --stream-height "${STREAM_HEIGHT}" \
  --source-format "${SOURCE_FORMAT}" \
  --stream-format "${STREAM_FORMAT}" \
  ${USE_RGA_FLAG} \
  --stream-socket "${STREAM_SOCK}" \
  --cv-width "${CV_WIDTH}" \
  --cv-height "${CV_HEIGHT}" \
  --cv-socket "${CV_SOCK}" \
  ${DEBUG_OVERLAY:+--cv2-socket "${DEBUG_SRC_SOCK}"} \
  > "${CAPTURE_LOG}" 2>&1 &
CAPTURE_PID=$!

cleanup() {
  set +e
  if [[ -n "${RTSP_PID:-}" ]]; then
    kill "${RTSP_PID}" 2>/dev/null || true
  fi
  if [[ -n "${DEBUG_PID:-}" ]]; then
    kill "${DEBUG_PID}" 2>/dev/null || true
  fi
  if [[ -n "${CAPTURE_PID:-}" ]]; then
    kill "${CAPTURE_PID}" 2>/dev/null || true
  fi
}
if [[ "${DAEMONIZE}" != "1" ]]; then
  trap cleanup EXIT INT TERM
fi

echo "Waiting for shm sockets..."
for _i in $(seq 1 50); do
  if [[ -S "${STREAM_SOCK}" && -S "${CV_SOCK}" ]]; then
    break
  fi
  if ! kill -0 "${CAPTURE_PID}" >/dev/null 2>&1; then
    echo "ERROR: capture daemon exited early. Tail of ${CAPTURE_LOG}:" >&2
    tail -n 80 "${CAPTURE_LOG}" >&2 || true
    exit 1
  fi
  sleep 0.1
done
if [[ ! -S "${STREAM_SOCK}" || ! -S "${CV_SOCK}" ]]; then
  echo "ERROR: shm sockets not created in time." >&2
  echo "Tail of ${CAPTURE_LOG}:" >&2
  tail -n 80 "${CAPTURE_LOG}" >&2 || true
  exit 1
fi
if [[ "${DEBUG_OVERLAY}" == "1" ]]; then
  echo "Waiting for debug source socket..."
  for _i in $(seq 1 50); do
    if [[ -S "${DEBUG_SRC_SOCK}" ]]; then
      break
    fi
    if ! kill -0 "${CAPTURE_PID}" >/dev/null 2>&1; then
      echo "ERROR: capture daemon exited early. Tail of ${CAPTURE_LOG}:" >&2
      tail -n 80 "${CAPTURE_LOG}" >&2 || true
      exit 1
    fi
    sleep 0.1
  done
  if [[ ! -S "${DEBUG_SRC_SOCK}" ]]; then
    echo "ERROR: debug source socket not created in time." >&2
    tail -n 80 "${CAPTURE_LOG}" >&2 || true
    exit 1
  fi
fi

if [[ "${DEBUG_OVERLAY}" == "1" ]]; then
  echo "Starting debug overlay..."
  python3 "$(dirname "$0")/rk3566_debug_overlay.py" \
    --cv-socket "${DEBUG_SRC_SOCK}" \
    --cv-width "${CV_WIDTH}" \
    --cv-height "${CV_HEIGHT}" \
    --fps "${FPS}" \
    --stream-width "${DEBUG_WIDTH}" \
    --stream-height "${DEBUG_HEIGHT}" \
    --out-fps "${DEBUG_FPS}" \
    --out-socket "${DEBUG_SOCK}" \
    --scene-sock "${DEBUG_SCENE_SOCK}" \
    > /tmp/soulcam_debug_overlay.log 2>&1 &
  DEBUG_PID=$!
  echo "Waiting for debug shm socket..."
  for _i in $(seq 1 120); do
    if [[ -S "${DEBUG_SOCK}" ]]; then
      break
    fi
    if [[ -n "${DEBUG_PID}" && ! -z "${DEBUG_PID}" ]]; then
      if ! kill -0 "${DEBUG_PID}" >/dev/null 2>&1; then
        echo "ERROR: debug overlay exited early. Tail of /tmp/soulcam_debug_overlay.log:" >&2
        tail -n 80 /tmp/soulcam_debug_overlay.log >&2 || true
        exit 1
      fi
    fi
    sleep 0.1
  done
  if [[ ! -S "${DEBUG_SOCK}" ]]; then
    echo "WARN: debug shm socket not created in time. Continuing without debug overlay." >&2
    tail -n 80 /tmp/soulcam_debug_overlay.log >&2 || true
    DEBUG_PID=""
  fi
fi

if [[ "${START_RTSP}" == "1" ]]; then
  EXTRA_ARGS=()
  if [[ "${SOURCE_FPS_CAPS}" == "1" ]]; then
    EXTRA_ARGS+=("--source-fps-caps")
  fi
  if [[ "${NO_VIDEORATE}" == "1" ]]; then
    EXTRA_ARGS+=("--no-videorate")
  fi

  # When debug overlay is enabled, use the debug feed as the main RTSP stream.
  RTSP_STREAM_SOCK="${STREAM_SOCK}"
  RTSP_WIDTH="${STREAM_WIDTH}"
  RTSP_HEIGHT="${STREAM_HEIGHT}"
  RTSP_FPS="${FPS}"
  RTSP_SHM_FORMAT="${RTSP_SHM_FORMAT:-${STREAM_FORMAT}}"
  if [[ "${DEBUG_OVERLAY}" == "1" ]]; then
    RTSP_STREAM_SOCK="${DEBUG_SOCK}"
    RTSP_WIDTH="${DEBUG_WIDTH}"
    RTSP_HEIGHT="${DEBUG_HEIGHT}"
    RTSP_FPS="${DEBUG_FPS}"
    RTSP_SHM_FORMAT="NV12"
  fi

  python3 "$(dirname "$0")/rk3566_rtsp_server.py" \
    --source shm \
    --shm-socket "${RTSP_STREAM_SOCK}" \
    --shm-format "${RTSP_SHM_FORMAT}" \
    --width "${RTSP_WIDTH}" \
    --height "${RTSP_HEIGHT}" \
    --fps "${RTSP_FPS}" \
    --bitrate-kbps "${BITRATE_KBPS}" \
    --encoder "${RTSP_ENCODER}" \
    ${RTSP_SWAP_UV:+--swap-uv} \
    --port "${PORT}" \
    --mount "${MOUNT}" \
    "${EXTRA_ARGS[@]}" \
    > "${RTSP_LOG}" 2>&1 &
  RTSP_PID=$!

  echo "Waiting for RTSP listen on :${PORT}..."
  for _i in $(seq 1 50); do
    if ss -lntp 2>/dev/null | grep -q ":${PORT}"; then
      break
    fi
    if ! kill -0 "${RTSP_PID}" >/dev/null 2>&1; then
      echo "ERROR: rtsp server exited early. Tail of ${RTSP_LOG}:" >&2
      tail -n 120 "${RTSP_LOG}" >&2 || true
      exit 1
    fi
    sleep 0.1
  done
  if ! ss -lntp 2>/dev/null | grep -q ":${PORT}"; then
    echo "ERROR: RTSP server did not bind to :${PORT} in time." >&2
    echo "Tail of ${RTSP_LOG}:" >&2
    tail -n 120 "${RTSP_LOG}" >&2 || true
    exit 1
  fi
fi

echo
echo "Started:"
echo "- capture daemon pid: ${CAPTURE_PID} (log: ${CAPTURE_LOG})"
if [[ "${START_RTSP}" == "1" ]]; then
  echo "- rtsp server   pid: ${RTSP_PID} (log: ${RTSP_LOG})"
fi
if [[ -n "${DEBUG_PID}" ]]; then
  echo "- debug overlay pid: ${DEBUG_PID} (log: /tmp/soulcam_debug_overlay.log)"
fi
echo
if [[ "${START_RTSP}" == "1" ]]; then
  echo "RTSP URL: rtsp://<device-ip>:${PORT}${MOUNT}"
fi
echo "CV socket: ${CV_SOCK}"
echo
echo "Stop:"
if [[ "${START_RTSP}" == "1" ]]; then
  echo "  kill ${CAPTURE_PID} ${RTSP_PID}"
else
  echo "  kill ${CAPTURE_PID}"
fi

if [[ "${DAEMONIZE}" == "1" ]]; then
  echo "Daemonized: leaving processes running."
  exit 0
fi

if [[ "${START_RTSP}" == "1" ]]; then
  wait "${CAPTURE_PID}" "${RTSP_PID}"
else
  wait "${CAPTURE_PID}"
fi

