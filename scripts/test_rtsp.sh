#!/usr/bin/env bash
set -euo pipefail
# ============================================================================
# Test the SoulCam RTSP stream
#
# Run this from your PC/WSL to verify the RTSP stream from the RK3566 device.
#
# Usage:
#   ./scripts/test_rtsp.sh                        # default device IP
#   ./scripts/test_rtsp.sh 192.168.1.100          # custom IP
#   ./scripts/test_rtsp.sh 192.168.1.100 play     # open in ffplay
#   ./scripts/test_rtsp.sh 192.168.1.100 record   # save 10s clip
#   ./scripts/test_rtsp.sh 192.168.1.100 probe    # ffprobe stream info
# ============================================================================

DEVICE_IP="${1:-192.168.1.45}"
ACTION="${2:-check}"
PORT="${3:-8554}"
MOUNT="${4:-/cam}"
URL="rtsp://${DEVICE_IP}:${PORT}${MOUNT}"
DURATION="${DURATION:-10}"

echo "=== SoulCam RTSP Test ==="
echo "URL: ${URL}"
echo "Action: ${ACTION}"
echo

case "${ACTION}" in
    check)
        echo "--- Stream check (${DURATION}s, null output) ---"
        echo "This validates the stream is working without displaying video."
        echo
        ffmpeg -rtsp_transport tcp \
            -i "${URL}" \
            -t "${DURATION}" \
            -f null - \
            2>&1 | tail -20
        echo
        echo "If you see frame counts above, the stream is working."
        ;;

    play)
        echo "--- Playing stream ---"
        echo "Press 'q' to quit."
        echo
        ffplay -rtsp_transport tcp \
            -fflags nobuffer \
            -flags low_delay \
            -framedrop \
            "${URL}"
        ;;

    record)
        OUTPUT="soulcam_$(date +%Y%m%d_%H%M%S).mp4"
        echo "--- Recording ${DURATION}s to ${OUTPUT} ---"
        ffmpeg -rtsp_transport tcp \
            -i "${URL}" \
            -t "${DURATION}" \
            -c copy \
            "${OUTPUT}"
        echo
        echo "Saved: ${OUTPUT}"
        ;;

    probe)
        echo "--- Stream info ---"
        ffprobe -rtsp_transport tcp \
            -v quiet \
            -print_format json \
            -show_streams \
            -show_format \
            "${URL}"
        ;;

    latency)
        echo "--- Latency test (frame timestamps) ---"
        ffmpeg -rtsp_transport tcp \
            -i "${URL}" \
            -t 5 \
            -vf "showinfo" \
            -f null - 2>&1 | grep "showinfo" | head -20
        ;;

    *)
        echo "Unknown action: ${ACTION}"
        echo "Valid actions: check, play, record, probe, latency"
        exit 1
        ;;
esac
