#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# Set custom toolchain environment PATH required by PlatformIO
export PATH="/Users/abti/.platformio/packages/toolchain-riscv32-esp/bin:/Users/abti/.platformio/tools/toolchain-riscv32-esp/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

PIO_BIN="/Users/abti/.platformio/penv/bin/pio"
PORT="/dev/cu.usbmodem1101"
BAUD="115200"

echo "========================================================"
echo " 🛠️  Building and Uploading Firmware to Adafruit Feather..."
echo "========================================================"
$PIO_BIN run --target upload

echo ""
echo "========================================================"
echo " 🔌 Starting Serial Monitor (Trailing Logs)...           "
echo "========================================================"
$PIO_BIN device monitor --port "$PORT" --baud "$BAUD"
