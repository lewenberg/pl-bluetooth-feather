#!/bin/bash

# Set custom toolchain environment PATH required by PlatformIO
export PATH="/Users/abti/.platformio/packages/toolchain-riscv32-esp/bin:/Users/abti/.platformio/tools/toolchain-riscv32-esp/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

PIO_BIN="/Users/abti/.platformio/penv/bin/pio"
# Auto-detect the serial port dynamically
detect_port() {
    local detected_port
    detected_port=$($PIO_BIN device list --json-output 2>/dev/null | python3 -c '
import sys, json
try:
    devices = json.load(sys.stdin)
    # 1. Search for Espressif Vendor ID (303A)
    for d in devices:
        if "303A" in d.get("hwid", "").upper():
            print(d["port"])
            sys.exit(0)
    # 2. Fallback to any usbmodem or usbserial
    for d in devices:
        port = d.get("port", "")
        if "usbmodem" in port or "usbserial" in port:
            print(port)
            sys.exit(0)
except Exception:
    pass
sys.exit(1)
' 2>/dev/null)

    if [ -n "$detected_port" ]; then
        echo "$detected_port"
        return
    fi

    # Fallback 1: Check filesystem /dev/cu.usbmodem* or /dev/cu.usbserial*
    for p in /dev/cu.usbmodem* /dev/cu.usbserial*; do
        if [ -e "$p" ]; then
            echo "$p"
            return
        fi
    done

    # Fallback 2: Default to /dev/cu.usbmodem101
    echo "/dev/cu.usbmodem101"
}

PORT=$(detect_port)
BAUD="115200"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Function to display premium CLI usage guide
show_help() {
    echo "=================================================================="
    echo " ⚡ ESP32-C6 PowerLabs Firmware CLI Manager ⚡"
    echo "=================================================================="
    echo "Usage: ./run.sh [COMMAND]"
    echo ""
    echo "Commands:"
    echo "  run      Builds the codebase, uploads (flashes) firmware to the"
    echo "           Feather, and trails real-time serial telemetry."
    echo "  flash    Builds and uploads firmware without starting the serial"
    echo "           monitor (useful for headless programming)."
    echo "  log      Automatically clears serial locks and runs dump_logs.py"
    echo "           to dump flash time-series data locally as a CSV."
    echo "  clear    Wipes timeseries log history from the board's flash memory"
    echo "           and deletes local CSV logs."
    echo "  help     Displays this help guide."
    echo "=================================================================="
}

# If no argument is provided, show the help guide
if [ -z "$1" ]; then
    show_help
    exit 0
fi

# Route commands
case "$1" in
    run)
        echo "========================================================"
        echo " 🛠️  Building & Uploading Firmware to Adafruit Feather..."
        echo "========================================================"
        $PIO_BIN run --target upload
        
        echo ""
        echo "========================================================"
        echo " 🔌 Starting Serial Monitor (Trailing Logs)...           "
        echo "========================================================"
        $PIO_BIN device monitor --port "$PORT" --baud "$BAUD"
        ;;
        
    flash)
        echo "========================================================"
        echo " 🛠️  Building & Uploading Firmware (Flash Only)...        "
        echo "========================================================"
        $PIO_BIN run --target upload
        echo " 🎉 SUCCESS! Upload completed successfully."
        ;;
        
    log)
        echo "========================================================"
        echo " 📥 Initiating Automated Log Extraction to Disk...      "
        echo "========================================================"
        # Execute the local python dumper script using PlatformIO's Python interpreter
        "/Users/abti/.platformio/penv/bin/python" "$SCRIPT_DIR/dump_logs.py"
        ;;
        
    clear)
        echo "========================================================"
        echo " 🧼  Wiping ESP32-C6 Flash Logs & Local History...        "
        echo "========================================================"
        "/Users/abti/.platformio/penv/bin/python" "$SCRIPT_DIR/dump_logs.py" --clear
        ;;
        
    help|--help|-h)
        show_help
        ;;
        
    *)
        echo "❌ Error: Invalid command '$1'"
        echo ""
        show_help
        exit 1
        ;;
esac
