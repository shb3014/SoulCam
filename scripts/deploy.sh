#!/usr/bin/env bash
set -euo pipefail
# ============================================================================
# Deploy SoulCam to RK3566 device
#
# Syncs the project and builds on-device via SSH.
#
# Usage:
#   ./scripts/deploy.sh                        # default device
#   ./scripts/deploy.sh 192.168.1.100          # custom IP
#   ./scripts/deploy.sh 192.168.1.100 ubuntu   # custom user
# ============================================================================

DEVICE_IP="${1:-192.168.1.45}"
DEVICE_USER="${2:-ubuntu}"
REMOTE_DIR="/home/${DEVICE_USER}/SoulCam"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=== Deploy SoulCam ==="
echo "Local:  ${PROJECT_DIR}"
echo "Remote: ${DEVICE_USER}@${DEVICE_IP}:${REMOTE_DIR}"
echo

# Sync source files (exclude large/irrelevant dirs)
echo "[1/3] Syncing files..."
rsync -avz --progress \
    --exclude='build/' \
    --exclude='kernel_*/' \
    --exclude='ubuntu-rockchip-*/' \
    --exclude='device_backups/' \
    --exclude='old/' \
    --exclude='.git/' \
    --exclude='__pycache__/' \
    --exclude='*.o' \
    --exclude='*.so' \
    "${PROJECT_DIR}/" \
    "${DEVICE_USER}@${DEVICE_IP}:${REMOTE_DIR}/"

# Build on device
echo
echo "[2/3] Building on device..."
ssh "${DEVICE_USER}@${DEVICE_IP}" "cd ${REMOTE_DIR} && bash scripts/build.sh"

# Show how to run
echo
echo "[3/3] Deploy complete!"
echo
echo "To run on device:"
echo "  ssh ${DEVICE_USER}@${DEVICE_IP}"
echo "  cd ${REMOTE_DIR}"
echo "  ./build/soulcam"
echo
echo "To test from this machine:"
echo "  ./scripts/test_rtsp.sh ${DEVICE_IP}"
