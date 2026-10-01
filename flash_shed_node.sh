#!/bin/bash
# Flash the already-built Meshtastic custom firmware to a board.
# No rebuild - just writes the merged factory.bin already sitting in
# .pio/build/<environment>/ from your last successful build.
#
# Usage: ./flash_shed_node.sh <environment> <port>
# Example: ./flash_shed_node.sh heltec-v4 /dev/ttyACM0
#          ./flash_shed_node.sh heltec-v3 /dev/ttyUSB0

set -e

ENVIRONMENT="$1"
PORT="$2"

if [ -z "$ENVIRONMENT" ] || [ -z "$PORT" ]; then
    echo "Usage: $0 <environment> <port>"
    echo "Example: $0 heltec-v4 /dev/ttyACM0"
    exit 1
fi

BUILD_DIR=".pio/build/$ENVIRONMENT"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Can't find $BUILD_DIR - has this environment been built yet?"
    echo "Run this from the firmware repo root."
    exit 1
fi

# Find the merged factory.bin automatically, since the version string in the
# filename changes on every rebuild - avoids hardcoding it and having this
# script silently go stale after your next build.
FACTORY_BIN=$(ls "$BUILD_DIR"/*.factory.bin 2>/dev/null | head -n 1)

if [ -z "$FACTORY_BIN" ]; then
    echo "No *.factory.bin found in $BUILD_DIR"
    echo "Contents of that directory:"
    ls "$BUILD_DIR"
    exit 1
fi

echo "Flashing $FACTORY_BIN to $PORT (environment: $ENVIRONMENT) ..."

# The factory.bin is the pre-merged bootloader + partitions + boot_app0 + app
# image, written as a single file at offset 0x0.
esptool.py --chip esp32s3 --port "$PORT" --baud 921600 \
    write-flash 0x0 "$FACTORY_BIN"

echo "Done. Reboot the board and check the serial log for the init line."
