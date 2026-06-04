# pl-bluetooth-feather

An embedded firmware application for the **Adafruit Feather ESP32-C6** dev board that acts as an automated **Bluetooth Low Energy (BLE) Central Client** to stream, process, and log time-series heartbeat data from a **PowerLabs Heart Rate Monitor** (or any device advertising the standard Heart Rate Service `0x180D`).

It also integrates I2C battery diagnostics and a flash-wear-protected logging file system.

---

## Key Features

### 1. Automated BLE Client Connection
* **Target Matching:** Automatically scans for and connects to BLE peripherals supporting the **GATT Heart Rate Service (`0x180D`)** or exposing names matching **`PowerLabs`** (case-insensitive search).
* **Automated Subscription:** Discovers the **Heart Rate Measurement characteristic (`0x2A37`)** and registers for notification streams.
* **Format Parsing:** Decodes incoming telemetry in real-time, supporting both standard 8-bit and 16-bit heart rate packet structures.

### 2. Time-Series Flash Logging (`LittleFS`)
* **Flash Longevity Buffer:** Writing directly to flash on every heartbeat (once per second) would rapidly exhaust the storage chip's write endurance. To protect your hardware, records are stored in a **RAM buffer** and written in batches of **10 records** to `/hr_history.csv` on internal flash.
* **Auto-Recovery:** Remaining RAM logs are flushed automatically upon BLE disconnection to guarantee zero data loss.
* **CSV Format:** Recorded in standard comma-separated format: `elapsed_ms,bpm` (perfect for Excel, Python, or data analysis tools).

### 3. Interactive Serial CLI Commands
Directly manage the device over the USB serial port at `115200` baud using the built-in terminal commands (case-insensitive):
* **`DUMP`**: Flushes any pending RAM logs to flash, opens `/hr_history.csv`, and streams the entire time-series history directly to the terminal.
* **`INFO`**: Returns storage space metrics (Total Space, Used Space, Free Space, and Log File Size) to diagnose LittleFS usage.
* **`CLEAR`**: Wipes all logged timeseries data from the internal flash filesystem and resets the RAM buffer.

### 4. Battery Monitoring (MAX17048 Gauge)
* Periodically reads cell voltage (`VCELL`), state of charge (`SOC`), and charge/discharge rate (`CRATE`) from the Feather's onboard **MAX17048 I2C fuel gauge**.
* Outputs state labels (`OK`, `LOW`, `CRITICAL`, or `CHECK`) in the telemetry logs to monitor device health.

### 5. Status LED Color Code
The built-in NeoPixel RGB LED represents connection phases with custom blinking intervals:
* **Flashing Red (1-second interval):** Actively scanning and searching for the PowerLabs monitor.
* **3 Rapid Green Flashes:** Connection successfully established and services authenticated.
* **Pulsing Green Heartbeat (every 10 seconds):** Worn and operating normally, streaming heart rate and HRV telemetry.
* **Solid Blue:** Pairing GATT parameters.

---

## Getting Started & Running

A helper script is provided to compile, flash, and open the serial monitor in a single step using the custom environment requirements of this setup.

To run the script:
```bash
./run.sh
```

This executable bash script will:
1. Load the required compiler `PATH` variables.
2. Build and upload the compiled `.bin` to `/dev/cu.usbmodem1101` using PlatformIO.
3. Open the device monitor at `115200` baud.

---

## File System Commands Reference

Once your serial monitor is open and trailing logs, type one of these commands and press `Enter` to run it:

### Retrieve Data
Command: `DUMP`
```text
[CMD] Executing action: DUMP
--- START OF TIMESERIES DATA (CSV: elapsed_ms,bpm) ---
12423,81
13425,81
14422,82
15423,82
--- END OF TIMESERIES DATA ---
```

### Storage Information
Command: `INFO`
```text
[CMD] Executing action: INFO
--- LittleFS Storage Diagnostics ---
  Total Capacity: 1441792 bytes (1408.00 KB)
  Used Storage:   2048 bytes (2.00 KB)
  Available:      1439744 bytes (1406.00 KB)
  Log File Size:  242 bytes
------------------------------------
```

### Reset Storage
Command: `CLEAR`
```text
[CMD] Executing action: CLEAR
[FS] Flash log history deleted successfully.
```

---

## Automated Log Extraction Script (`dump_logs.py`)

Rather than opening a terminal monitor and copying text manually, you can use the automated **`dump_logs.py`** Python script in the root directory.

It automates the entire process:
1. **Resolves Serial Conflicts:** Automatically scans for and terminates any background sessions (like `screen` or `pio monitor`) blocking the serial interface.
2. **Commands the Board:** Establishes a raw serial connection and sends the `DUMP` command.
3. **Parses & Extracts Data:** Listens to the response, isolates the raw time-series CSV lines, and filters out system status messages.
4. **Saves to Disk:** Stores the data inside a clean `/logs` directory on your host PC as a datestamped file: `logs/heartrate_YYYYMMDD_HHMMSS.csv`.

To extract and save your logs at any time, run:
```bash
./dump_logs.py
```

