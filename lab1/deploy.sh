#!/bin/bash

# Lab 1 Deployment Script for OrangePi RV2
# Automates copying the kernel.fit to the SD card

MOUNT_POINT="/mnt/sdboot"
PARTITION="/dev/sda1"
KERNEL_FIT="kernel.fit"

# 1. Ensure we are in the lab1 directory or kernel.fit is present
if [ ! -f "$KERNEL_FIT" ]; then
    echo "Error: $KERNEL_FIT not found in the current directory."
    echo "Please run this script from the lab1/ directory after running 'make fit'."
    exit 1
fi

# 2. Create mount point if it doesn't exist
echo "Preparing mount point..."
sudo mkdir -p "$MOUNT_POINT"

# 3. Mount the SD card (USB card reader)
echo "Mounting $PARTITION to $MOUNT_POINT..."
if ! sudo mount "$PARTITION" "$MOUNT_POINT"; then
    echo "Error: Failed to mount $PARTITION."
    echo "Check if the SD card reader is plugged in and recognized as $PARTITION (lsblk)."
    exit 1
fi

# 4. Copy the kernel
echo "Copying $KERNEL_FIT..."
sudo cp "$KERNEL_FIT" "$MOUNT_POINT/kernel.fit"

# 5. Sync and Clean up
echo "Syncing and unmounting..."
sudo sync
sudo umount "$MOUNT_POINT"

echo "------------------------------------------------"
echo "✅ Success! kernel.fit copied to SD card."
echo "You can now safely remove the card reader."
echo "------------------------------------------------"
