#!/usr/bin/env python3
import serial
import time
import datetime
import subprocess
import os
import sys

def detect_port():
    import serial.tools.list_ports
    ports = list(serial.tools.list_ports.comports())
    
    # 1. Search for Espressif Vendor ID (303A)
    for p in ports:
        if p.vid == 0x303a or (p.hwid and "303a" in p.hwid.lower()):
            return p.device
            
    # 2. Search for any usbmodem or usbserial
    for p in ports:
        desc = (p.description or "").lower()
        device = (p.device or "").lower()
        hwid = (p.hwid or "").lower()
        if any(x in desc or x in device or x in hwid for x in ["usbmodem", "usbserial"]):
            return p.device
            
    # Fallback to checking typical filesystem paths
    for path in ['/dev/cu.usbmodem101', '/dev/cu.usbmodem1101']:
        if os.path.exists(path):
            return path
            
    return '/dev/cu.usbmodem101'

PORT = detect_port()
BAUD = 115200

def kill_locking_processes():
    """Finds and kills any processes (like screen or pio monitor) locking the serial port."""
    try:
        # Run lsof to find processes using the serial port
        output = subprocess.check_output(f"lsof | grep {os.path.basename(PORT)}", shell=True).decode('utf-8')
        pids = []
        for line in output.strip().split('\n'):
            parts = line.split()
            if len(parts) > 1:
                pid = parts[1]
                name = parts[0]
                pids.append((pid, name))
        
        if pids:
            print("⚠️  Found serial port locked by existing processes:")
            for pid, name in pids:
                print(f"   - Killing process '{name}' (PID: {pid})...")
                subprocess.call(["kill", "-9", pid])
            time.sleep(1.0) # Settle port
    except subprocess.CalledProcessError:
        # lsof returned non-zero (no processes found using the port), which is good!
        pass
    except Exception as e:
        print(f"ℹ️  Could not check/kill locking processes: {e}")

def dump_logs_to_disk():
    # 1. Clear any serial locks automatically
    kill_locking_processes()

    print(f"🔌 Connecting to ESP32-C6 on {PORT} at {BAUD} baud...")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=3)
        time.sleep(1.5) # Wait for interface to settle
        ser.reset_input_buffer()
    except Exception as e:
        print(f"❌ Error opening serial port {PORT}: {e}")
        sys.exit(1)

    # 2. Trigger the DUMP CLI command on the board
    print("📡 Sending 'DUMP' command...")
    ser.write(b"DUMP\n")
    ser.flush()

    # 3. Read and parse incoming serial output
    print("📥 Extracting timeseries data...")
    recording = False
    csv_rows = []
    
    start_time = time.time()
    while time.time() - start_time < 5.0: # 5-second timeout window
        if ser.in_waiting > 0:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            
            if "--- START OF TIMESERIES DATA" in line:
                recording = True
                continue
            elif "--- END OF TIMESERIES DATA" in line:
                recording = False
                break
            
            if recording and line:
                csv_rows.append(line)
        else:
            time.sleep(0.05)

    ser.close()

    if not csv_rows:
        print("❌ Error: No timeseries data captured. Make sure the board is powered on and connected!")
        sys.exit(1)

    # 4. Save cleanly to disk
    os.makedirs('logs', exist_ok=True)
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    filepath = f"logs/heartrate_{timestamp}.csv"
    
    with open(filepath, 'w') as f:
        # Write CSV Header
        f.write("elapsed_ms,bpm,rr_interval_ms,strap_battery,sequence\n")
        # Write Data
        for row in csv_rows:
            f.write(f"{row}\n")

    print("\n========================================================")
    print(f" 🎉 SUCCESS! Saved {len(csv_rows)} records to disk.")
    print(f" 📂 File Path: {os.path.abspath(filepath)}")
    print("========================================================")

def clear_logs_on_board_and_disk():
    # 1. Clear any serial locks automatically
    kill_locking_processes()

    # 2. Clear local CSV logs in logs/ folder
    print("🧹 Cleaning local CSV files in logs/ directory...")
    cleared_count = 0
    if os.path.exists('logs'):
        for filename in os.listdir('logs'):
            if filename.endswith('.csv'):
                try:
                    os.remove(os.path.join('logs', filename))
                    cleared_count += 1
                except Exception as e:
                    print(f"   ⚠️ Could not delete local file '{filename}': {e}")
    if cleared_count > 0:
        print(f"   ✅ Successfully deleted {cleared_count} local log file(s).")
    else:
        print("   ℹ️ No local CSV files to delete.")

    # 3. Connect to the board and send CLEAR
    print(f"🔌 Connecting to ESP32-C6 on {PORT} at {BAUD} baud...")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=3)
        time.sleep(1.5) # Wait for interface to settle
        ser.reset_input_buffer()
    except Exception as e:
        print(f"❌ Error opening serial port {PORT}: {e}")
        sys.exit(1)

    print("📡 Sending 'CLEAR' command to board flash...")
    ser.write(b"CLEAR\n")
    ser.flush()

    # 4. Read response lines until timeout
    print("📥 Waiting for board confirmation...")
    start_time = time.time()
    while time.time() - start_time < 3.0:  # 3-second timeout window
        if ser.in_waiting > 0:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                print(f"   [Board] {line}")
        else:
            time.sleep(0.05)

    ser.close()

    print("\n========================================================")
    print(" 🎉 SUCCESS! Logs cleared on board flash and local disk.")
    print("========================================================")

if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--clear':
        clear_logs_on_board_and_disk()
    else:
        dump_logs_to_disk()
