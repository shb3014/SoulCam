#!/usr/bin/env bash
set -euo pipefail
# ============================================================================
# Build SoulCam on-device (RK3566)
#
# Prerequisites (install once on device):
#   sudo apt install -y \
#       build-essential cmake pkg-config \
#       libgstreamer1.0-dev \
#       libgstreamer-plugins-base1.0-dev \
#       libgstreamer-plugins-bad1.0-dev \
#       gstreamer1.0-plugins-good \
#       gstreamer1.0-plugins-bad \
#       gstreamer1.0-rtsp \
#       libgstrtspserver-1.0-dev
#
# GStreamer RTSP server dev package names may vary:
#   - libgstrtspserver-1.0-dev  (Ubuntu/Debian)
#   - gstreamer1.0-rtsp-devel   (some Rockchip BSPs)
#
# Usage:
#   ./scripts/build.sh            # Release build
#   ./scripts/build.sh debug      # Debug build
#   ./scripts/build.sh clean      # Clean and rebuild
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC_DIR="${PROJECT_DIR}/src"
BUILD_DIR="${PROJECT_DIR}/build"

BUILD_TYPE="${1:-Release}"
case "${BUILD_TYPE,,}" in
    debug)   BUILD_TYPE="Debug"   ;;
    release) BUILD_TYPE="Release" ;;
    clean)
        echo "Cleaning build directory..."
        rm -rf "${BUILD_DIR}"
        BUILD_TYPE="Release"
        ;;
esac

echo "=== SoulCam Build ==="
echo "Source:  ${SRC_DIR}"
echo "Build:   ${BUILD_DIR}"
echo "Type:    ${BUILD_TYPE}"
echo

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake "${SRC_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DRKNN_DIR="${PROJECT_DIR}/rknn"

NPROC=$(nproc 2>/dev/null || echo 2)
cmake --build . -j"${NPROC}"

echo
echo "Build complete: ${BUILD_DIR}/soulcam"
echo
echo "Run:"
echo "  ${BUILD_DIR}/soulcam                              # RTSP only"
echo "  ${BUILD_DIR}/soulcam --ai --model rk3566/yolov8n.rknn  # RTSP + AI"
echo "  ${BUILD_DIR}/soulcam -v                            # verbose"
