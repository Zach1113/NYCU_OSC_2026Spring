#!/bin/bash

# Lab 2 deployment script for OrangePi RV2.
# Copies an existing FIT image to SD (build separately via make fit).

set -euo pipefail

MOUNT_POINT="${MOUNT_POINT:-/mnt/sdboot}"
PARTITION="${PARTITION:-/dev/sda1}"
FIT_IMAGE="${FIT_IMAGE:-kernel.fit}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$SCRIPT_DIR"

if [ ! -f "$FIT_IMAGE" ]; then
    echo "Error: $FIT_IMAGE not found in $SCRIPT_DIR"
    echo "Hint: run 'make fit' first, then run this script"
    exit 1
fi

echo "[1/4] Preparing mount point: $MOUNT_POINT"
sudo mkdir -p "$MOUNT_POINT"

echo "[2/4] Mounting $PARTITION -> $MOUNT_POINT"
if ! sudo mount "$PARTITION" "$MOUNT_POINT"; then
    echo "Error: Failed to mount $PARTITION"
    echo "Hint: run lsblk to check your SD partition path"
    exit 1
fi

cleanup() {
    echo "[4/4] Syncing and unmounting"
    sudo sync
    sudo umount "$MOUNT_POINT"
}
trap cleanup EXIT

echo "[3/4] Copying $FIT_IMAGE to SD boot partition"
sudo cp "$FIT_IMAGE" "$MOUNT_POINT/kernel.fit"

echo "------------------------------------------------"
echo "Success: kernel.fit has been deployed to $PARTITION"
echo "------------------------------------------------"
