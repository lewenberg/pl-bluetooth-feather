#include <Arduino.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <algorithm>
#include <cctype>

namespace {
// BLE Service and Characteristic UUIDs (Standard Heart Rate & Battery Services)
static BLEUUID serviceUUID("180d");
static BLEUUID charUUID("2a37");
static BLEUUID batteryServiceUUID("180f");
static BLEUUID batteryCharUUID("2a19");

// BLE connection control states
bool doConnect = false;
bool connected = false;
bool doScan = false;

BLEAdvertisedDevice* myDevice = nullptr;
BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
BLEClient* pClient = nullptr;

// BLE Server state variables
BLEServer* pServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
bool bleDeviceConnected = false;
bool oldBleDeviceConnected = false;

// Telemetry state variables
uint16_t currentHeartRate = 0;
uint16_t currentRrInterval = 0;
uint8_t currentStrapBattery = 100; // Initialize strap battery at 100%
uint32_t currentSequence = 1;

unsigned long lastHeartRateUpdateMs = 0;
unsigned long lastStrapBatteryReadMs = 0;

// Logging & Heartbeat timing
constexpr uint8_t STATUS_LED_PIN = PIN_NEOPIXEL;
constexpr uint8_t STATUS_LED_BRIGHTNESS = 4; 
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 10000;
constexpr unsigned long HEARTBEAT_ON_MS = 1000;
constexpr unsigned long STATUS_LOG_INTERVAL_MS = 5000;
constexpr unsigned long BATTERY_LOG_INTERVAL_MS = 10000;
constexpr unsigned long STRAP_BATTERY_POLL_INTERVAL_MS = 30000; // Poll strap battery every 30 seconds
constexpr unsigned long LOOP_DELAY_MS = 50;

unsigned long lastHeartbeatAt = 0;
unsigned long lastStatusLogAt = 0;
unsigned long lastBatteryLogAt = 0;
bool heartbeatOn = false;

// MAX17048 Fuel Gauge registers
constexpr uint8_t MAX17048_ADDRESS = 0x36;
constexpr uint8_t MAX17048_VCELL_REGISTER = 0x02;
constexpr uint8_t MAX17048_SOC_REGISTER = 0x04;
constexpr uint8_t MAX17048_CRATE_REGISTER = 0x16;

// LittleFS Time-Series parameters
#define FILE_PATH "/hr_history.csv"

struct LogRecord {
  unsigned long timestamp;
  uint16_t heartRate;
  uint16_t rrInterval;
  uint8_t strapBattery;
  uint32_t sequence;
};

constexpr size_t LOG_BUFFER_SIZE = 10;
LogRecord logBuffer[LOG_BUFFER_SIZE];
size_t logBufferIndex = 0;

// Forward declarations
void readStrapBattery();

// System functions
void setStatusLed(uint8_t red, uint8_t green, uint8_t blue) {
  rgbLedWrite(STATUS_LED_PIN, red, green, blue);
}

void initStatusLed() {
#ifdef NEOPIXEL_I2C_POWER
  pinMode(NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_I2C_POWER, HIGH);
  delay(10);
#endif
  setStatusLed(0, 0, 0);
}

void flashGreenQuickly() {
  for (int i = 0; i < 3; i++) {
    setStatusLed(0, STATUS_LED_BRIGHTNESS, 0); // Green ON
    delay(150);
    setStatusLed(0, 0, 0); // Green OFF
    delay(150);
  }
}

bool readMax17048Register(uint8_t reg, uint16_t &value) {
  Wire.beginTransmission(MAX17048_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(MAX17048_ADDRESS, static_cast<uint8_t>(2)) != 2) {
    return false;
  }

  value = (static_cast<uint16_t>(Wire.read()) << 8) | Wire.read();
  return true;
}

const char *batteryHealthLabel(float voltage, float chargePercent) {
  if (voltage < 3.20f || chargePercent < 5.0f) {
    return "CRITICAL";
  }
  if (voltage < 3.50f || chargePercent < 20.0f) {
    return "LOW";
  }
  if (voltage > 4.25f) {
    return "CHECK";
  }
  return "OK";
}

void printBatteryStatus() {
  uint16_t vcellRaw = 0;
  uint16_t socRaw = 0;
  uint16_t crateRaw = 0;

  if (!readMax17048Register(MAX17048_VCELL_REGISTER, vcellRaw) ||
      !readMax17048Register(MAX17048_SOC_REGISTER, socRaw) ||
      !readMax17048Register(MAX17048_CRATE_REGISTER, crateRaw)) {
    Serial.println("[Battery] MAX17048 fuel gauge not found over I2C");
    return;
  }

  const float voltage = static_cast<float>(vcellRaw) * 0.000078125f;
  const float chargePercent = static_cast<float>(socRaw) / 256.0f;
  const float ratePercentPerHour =
      static_cast<float>(static_cast<int16_t>(crateRaw)) * 0.208f;
  const char *direction = "idle";
  if (ratePercentPerHour > 1.0f) {
    direction = "charging";
  } else if (ratePercentPerHour < -1.0f) {
    direction = "discharging";
  }

  Serial.print("[Battery] ");
  Serial.print(voltage, 3);
  Serial.print("V | Charge ");
  Serial.print(chargePercent, 1);
  Serial.print("% | Rate ");
  Serial.print(ratePercentPerHour, 1);
  Serial.print("%/hr ");
  Serial.print(direction);
  Serial.print(" | Health ");
  Serial.println(batteryHealthLabel(voltage, chargePercent));
}

// Visual heartbeat feedback when connected
void updateConnectedHeartbeat() {
  const unsigned long now = millis();

  if (heartbeatOn && now - lastHeartbeatAt >= HEARTBEAT_ON_MS) {
    setStatusLed(0, 0, 0);
    heartbeatOn = false;
    lastHeartbeatAt = now;
    return;
  }

  if (!heartbeatOn && now - lastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) {
    // Pulse Green when connected
    setStatusLed(0, STATUS_LED_BRIGHTNESS, 0);
    heartbeatOn = true;
    lastHeartbeatAt = now;
  }
}

// Backward-seeking function to read last sequence number from flash
uint32_t readLastSequenceFromFlash() {
  if (!LittleFS.exists(FILE_PATH)) {
    return 0;
  }
  File file = LittleFS.open(FILE_PATH, FILE_READ);
  if (!file) {
    return 0;
  }
  size_t size = file.size();
  if (size == 0) {
    file.close();
    return 0;
  }

  // Seek backwards to find the last line
  size_t pos = size - 1;
  if (size > 1) {
    file.seek(pos);
    char lastChar = file.read();
    if (lastChar == '\n' || lastChar == '\r') {
      if (pos > 0) pos--;
    }
  }

  bool foundStart = false;
  while (pos > 0) {
    file.seek(pos);
    char c = file.read();
    if (c == '\n' || c == '\r') {
      file.seek(pos + 1);
      foundStart = true;
      break;
    }
    pos--;
  }
  if (!foundStart) {
    file.seek(0);
  }

  String lastLine = file.readStringUntil('\n');
  file.close();

  lastLine.trim();
  if (lastLine.length() == 0) {
    return 0;
  }

  // Parse the last line (comma-separated: elapsed_ms,bpm,rr_interval_ms,strap_battery,sequence)
  int commaIndex = -1;
  int commaCount = 0;
  for (int i = 0; i < lastLine.length(); i++) {
    if (lastLine[i] == ',') {
      commaCount++;
      if (commaCount == 4) {
        commaIndex = i;
        break;
      }
    }
  }

  if (commaIndex != -1 && commaIndex < lastLine.length() - 1) {
    String seqStr = lastLine.substring(commaIndex + 1);
    seqStr.trim();
    return (uint32_t)seqStr.toInt();
  }

  return 0;
}

// LittleFS operation functions
void initFileSystem() {
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Error: Failed to mount LittleFS. Formatting...");
    if (LittleFS.format()) {
      Serial.println("[FS] LittleFS formatted successfully.");
      if (!LittleFS.begin()) {
        Serial.println("[FS] Fatal: Failed to mount LittleFS even after format.");
      }
    } else {
      Serial.println("[FS] Error: Formatting failed.");
    }
  } else {
    Serial.println("[FS] LittleFS filesystem mounted successfully.");
  }
}

void flushBufferToFlash() {
  if (logBufferIndex == 0) return;

  File file = LittleFS.open(FILE_PATH, FILE_APPEND);
  if (!file) {
    Serial.println("[FS] Error: Failed to open CSV log for appending!");
    return;
  }

  for (size_t i = 0; i < logBufferIndex; i++) {
    // CSV format: elapsed_ms,bpm,rr_interval_ms,strap_battery,sequence
    file.printf("%lu,%u,%u,%u,%u\n", 
                logBuffer[i].timestamp, 
                logBuffer[i].heartRate, 
                logBuffer[i].rrInterval,
                logBuffer[i].strapBattery,
                logBuffer[i].sequence);
  }

  file.close();
  Serial.printf("[FS] Flash Wear Reduction: Flushed %d readings to flash storage.\n", logBufferIndex);
  logBufferIndex = 0;
}

void printFileSystemInfo() {
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();

  Serial.println("--- LittleFS Storage Diagnostics ---");
  Serial.printf("  Total Capacity: %d bytes (%.2f KB)\n", totalBytes, (float)totalBytes / 1024.0);
  Serial.printf("  Used Storage:   %d bytes (%.2f KB)\n", usedBytes, (float)usedBytes / 1024.0);
  Serial.printf("  Available:      %d bytes (%.2f KB)\n", totalBytes - usedBytes, (float)(totalBytes - usedBytes) / 1024.0);
  
  if (LittleFS.exists(FILE_PATH)) {
    File file = LittleFS.open(FILE_PATH, FILE_READ);
    if (file) {
      Serial.printf("  Log File Size:  %d bytes\n", file.size());
      file.close();
    }
  } else {
    Serial.println("  Log File Size:  0 bytes (does not exist)");
  }
  Serial.println("------------------------------------");
}

void sendBleMessage(const String& msg) {
  if (pTxCharacteristic != nullptr && bleDeviceConnected) {
    pTxCharacteristic->setValue(msg.c_str());
    pTxCharacteristic->notify();
    delay(15); // Small delay to let the BLE stack send the packet safely
  }
}

void dumpLogsToBle() {
  flushBufferToFlash();

  if (!LittleFS.exists(FILE_PATH)) {
    sendBleMessage("[FS] Log file does not exist yet. No timeseries history.\n");
    return;
  }

  File file = LittleFS.open(FILE_PATH, FILE_READ);
  if (!file) {
    sendBleMessage("[FS] Error: Failed to open CSV log file for reading.\n");
    return;
  }

  sendBleMessage("--- START OF TIMESERIES DATA (CSV: elapsed_ms,bpm,rr_interval_ms,strap_battery,sequence) ---\n");
  
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line += "\n";
    sendBleMessage(line);
  }
  
  sendBleMessage("--- END OF TIMESERIES DATA ---\n");
  file.close();
}

void clearLogsToBle() {
  logBufferIndex = 0; // Empty the active RAM buffer
  currentSequence = 1; // Reset sequence counter

  if (LittleFS.exists(FILE_PATH)) {
    if (LittleFS.remove(FILE_PATH)) {
      sendBleMessage("[FS] Flash log history deleted successfully.\n");
      Serial.println("[FS] Flash log history deleted successfully via BLE.");
    } else {
      sendBleMessage("[FS] Error: Failed to delete log file.\n");
      Serial.println("[FS] Error: Failed to delete log file via BLE.");
    }
  } else {
    sendBleMessage("[FS] Log file already empty.\n");
    Serial.println("[FS] Log file already empty.");
  }
}

void sendFileSystemInfoToBle() {
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  
  char buf[256];
  snprintf(buf, sizeof(buf), "--- LittleFS Storage Diagnostics ---\n"
                             "  Total Capacity: %d bytes (%.2f KB)\n"
                             "  Used Storage:   %d bytes (%.2f KB)\n"
                             "  Available:      %d bytes (%.2f KB)\n",
           totalBytes, (float)totalBytes / 1024.0,
           usedBytes, (float)usedBytes / 1024.0,
           totalBytes - usedBytes, (float)(totalBytes - usedBytes) / 1024.0);
  sendBleMessage(buf);
  
  if (LittleFS.exists(FILE_PATH)) {
    File file = LittleFS.open(FILE_PATH, FILE_READ);
    if (file) {
      snprintf(buf, sizeof(buf), "  Log File Size:  %d bytes\n", file.size());
      sendBleMessage(buf);
      file.close();
    }
  } else {
    sendBleMessage("  Log File Size:  0 bytes (does not exist)\n");
  }
  sendBleMessage("------------------------------------\n");
}

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    bleDeviceConnected = true;
    Serial.println("[BLE Server] Android client connected!");
  }

  void onDisconnect(BLEServer* pServer) override {
    bleDeviceConnected = false;
    Serial.println("[BLE Server] Android client disconnected.");
  }
};

class MyRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    String rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      String cmd = rxValue;
      cmd.trim();
      cmd.toUpperCase();
      
      Serial.printf("[BLE Server CMD] Received: %s\n", cmd.c_str());
      
      if (cmd == "DUMP") {
        dumpLogsToBle();
      } else if (cmd == "CLEAR") {
        clearLogsToBle();
      } else if (cmd == "INFO") {
        sendFileSystemInfoToBle();
      } else {
        sendBleMessage("[BLE Server CMD] Invalid command. Supported: DUMP, CLEAR, INFO\n");
      }
    }
  }
};

void handleSerialCommands() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toUpperCase();

  if (cmd.length() == 0) return;

  Serial.print("[CMD] Executing action: ");
  Serial.println(cmd);

  if (cmd == "DUMP") {
    // Flush current RAM buffer to flash first to ensure completeness
    flushBufferToFlash();

    if (!LittleFS.exists(FILE_PATH)) {
      Serial.println("[FS] Log file does not exist yet. No timeseries history.");
      return;
    }

    File file = LittleFS.open(FILE_PATH, FILE_READ);
    if (!file) {
      Serial.println("[FS] Error: Failed to open CSV log file for reading.");
      return;
    }

    Serial.println("--- START OF TIMESERIES DATA (CSV: elapsed_ms,bpm,rr_interval_ms,strap_battery) ---");
    while (file.available()) {
      Serial.write(file.read());
    }
    Serial.println("--- END OF TIMESERIES DATA ---");
    file.close();

  } else if (cmd == "CLEAR") {
    logBufferIndex = 0; // Empty the active RAM buffer
    currentSequence = 1; // Reset sequence counter

    if (LittleFS.exists(FILE_PATH)) {
      if (LittleFS.remove(FILE_PATH)) {
        Serial.println("[FS] Flash log history deleted successfully.");
      } else {
        Serial.println("[FS] Error: Failed to delete log file.");
      }
    } else {
      Serial.println("[FS] Log file already empty.");
    }

  } else if (cmd == "INFO") {
    printFileSystemInfo();

  } else {
    Serial.println("[CMD] Invalid action. Supported CLI: DUMP, CLEAR, INFO");
  }
}

// Case-insensitive check for PowerLabs-like BLE names
bool isPowerlabsDevice(const std::string& name) {
  if (name.empty()) return false;
  std::string lowerName = name;
  std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) {
    return std::tolower(c);
  });

  return (lowerName.find("powerlabs") != std::string::npos ||
          lowerName.find("pwrlabs") != std::string::npos ||
          lowerName.find("powrlabs") != std::string::npos ||
          lowerName.find("pwr-labs") != std::string::npos ||
          lowerName.find("pwr_labs") != std::string::npos ||
          lowerName.find("power labs") != std::string::npos ||
          lowerName.find("powr labs") != std::string::npos ||
          lowerName.find("pwr labs") != std::string::npos);
}

// Notification callback for Heart Rate Measurement characteristic
void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic,
                    uint8_t* pData, size_t length, bool isNotify) {
  if (length < 2) return;

  uint8_t flags = pData[0];
  size_t offset = 1;

  // 1. Parse Heart Rate Value Format (0 = UINT8, 1 = UINT16)
  uint16_t heartRate = 0;
  if (flags & 0x01) {
    if (length >= offset + 2) {
      heartRate = pData[offset] | (pData[offset + 1] << 8);
      offset += 2;
    }
  } else {
    if (length >= offset + 1) {
      heartRate = pData[offset];
      offset += 1;
    }
  }

  // 2. Parse Energy Expended if present (Bit 3)
  if (flags & 0x08) {
    offset += 2; // Skip 2 bytes of Energy Expended
  }

  // 3. Parse RR-Intervals if present (Bit 4)
  uint16_t rrInterval = 0;
  if (flags & 0x10) {
    if (length >= offset + 2) {
      // Decode first raw little-endian RR Interval
      uint16_t rrRaw = pData[offset] | (pData[offset + 1] << 8);
      // Convert to milliseconds: rrRaw * 1000 / 1024
      rrInterval = (uint16_t)((float)rrRaw / 1.024f);
    }
  }

  if (heartRate > 0 && heartRate < 240) {
    currentHeartRate = heartRate;
    currentRrInterval = rrInterval;
    lastHeartRateUpdateMs = millis();

    // Log to RAM memory buffer
    if (logBufferIndex < LOG_BUFFER_SIZE) {
      logBuffer[logBufferIndex++] = { 
        lastHeartRateUpdateMs, 
        heartRate, 
        rrInterval,
        currentStrapBattery,
        currentSequence
      };
    }

    // Flush RAM buffer to Flash filesystem if full
    if (logBufferIndex >= LOG_BUFFER_SIZE) {
      flushBufferToFlash();
    }
  }
}

void readStrapBattery() {
  if (!connected || pClient == nullptr) return;

  try {
    BLERemoteService* pBatteryService = pClient->getService(batteryServiceUUID);
    if (pBatteryService == nullptr) return;

    BLERemoteCharacteristic* pBatteryChar = pBatteryService->getCharacteristic(batteryCharUUID);
    if (pBatteryChar == nullptr) return;

    if (pBatteryChar->canRead()) {
      String value = pBatteryChar->readValue();
      if (value.length() > 0) {
        currentStrapBattery = (uint8_t)value[0];
        Serial.printf("[BLE] Polled PowerLabs Strap Battery: %u%%\n", currentStrapBattery);
      }
    }
  } catch (...) {
    // Suppress any background GATT reading faults to avoid device crashes
  }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) override {
    // Managed during connectToServer execution flow
  }

  void onDisconnect(BLEClient* pclient) override {
    connected = false;
    Serial.println("[BLE] Disconnected from BLE Heart Rate Monitor.");
    // Flush remaining buffer immediately so we do not lose recent trailing readings
    flushBufferToFlash();
  }
};

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    bool isMatch = false;

    // Matches standard Heart Rate Service or PowerLabs naming convention
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(serviceUUID)) {
      isMatch = true;
    } else if (advertisedDevice.haveName() && isPowerlabsDevice(advertisedDevice.getName().c_str())) {
      isMatch = true;
    }

    if (isMatch) {
      Serial.printf("[BLE] Matching device discovered: %s [%s]\n",
                    advertisedDevice.getName().c_str(),
                    advertisedDevice.getAddress().toString().c_str());

      // Stop active scan immediately
      BLEDevice::getScan()->stop();

      if (myDevice != nullptr) {
        delete myDevice;
      }
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = false;
    }
  }
};

bool connectToServer() {
  if (myDevice == nullptr) return false;

  Serial.printf("[BLE] Establishing connection to %s...\n", myDevice->getAddress().toString().c_str());

  if (pClient == nullptr) {
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());
  }

  if (!pClient->connect(myDevice)) {
    Serial.println("[BLE] Connection attempt failed.");
    return false;
  }

  Serial.println("[BLE] Connected! Retrieving Heart Rate service...");
  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.printf("[BLE] Service not found: %s\n", serviceUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }

  Serial.println("[BLE] Service resolved. Retrieving Heart Rate Measurement characteristic...");
  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) {
    Serial.printf("[BLE] Characteristic not found: %s\n", charUUID.toString().c_str());
    pClient->disconnect();
    return false;
  }

  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
    Serial.println("[BLE] Subscribed to Heart Rate notifications successfully.");
    
    // Read the PowerLabs strap battery once upon connection
    readStrapBattery();
    lastStrapBatteryReadMs = millis();
  } else {
    Serial.println("[BLE] Error: Characteristic does not support notifications.");
    pClient->disconnect();
    return false;
  }

  connected = true;
  return true;
}
} // namespace

void setup() {
  initStatusLed();
  Wire.begin();
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("================================================");
  Serial.println("ESP32-C6 PowerLabs Bluetooth Heart Rate Monitor");
  Serial.println("================================================");

  // Initialize LittleFS Flash filesystem
  initFileSystem();

  // Read the last sequence number from flash log if it exists
  uint32_t lastSeq = readLastSequenceFromFlash();
  currentSequence = lastSeq + 1;
  Serial.printf("[FS] Initialized sequence numbering at: %u\n", currentSequence);

  printBatteryStatus();

  // Initialize ESP32-C6 BLE hardware
  BLEDevice::init("ESP32-C6-LogDumper");
  
  // Print local Bluetooth MAC Address
  Serial.printf("[BLE] Local Bluetooth Address: %s\n", BLEDevice::getAddress().toString().c_str());

  // Setup BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create standard Nordic UART Service
  BLEService* pService = pServer->createService("6e400001-b5a3-f393-e0a9-e50e24dcca9e");

  // Create TX Characteristic (Notify)
  pTxCharacteristic = pService->createCharacteristic(
                        "6e400003-b5a3-f393-e0a9-e50e24dcca9e",
                        BLECharacteristic::PROPERTY_NOTIFY
                      );

  // Create RX Characteristic (Write)
  BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
                                           "6e400002-b5a3-f393-e0a9-e50e24dcca9e",
                                           BLECharacteristic::PROPERTY_WRITE
                                         );
  pRxCharacteristic->setCallbacks(new MyRxCallbacks());

  // Start the service
  pService->start();

  // Configure advertising
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[BLE Server] Advertising Nordic UART Service (NUS) active.");

  // Setup BLE Client (Central Scan)
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);

  Serial.println("[BLE] Bluetooth hardware initialized.");
  Serial.println("[CLI] Diagnostic terminal active. Supported: DUMP, CLEAR, INFO");
}

void loop() {
  const unsigned long now = millis();

  // Check and process Serial interactive CLI utilities
  handleSerialCommands();

  // Handle BLE Server reconnection and advertising restart
  if (!bleDeviceConnected && oldBleDeviceConnected) {
    delay(500); // give the bluetooth stack a moment
    pServer->startAdvertising(); // restart advertising
    Serial.println("[BLE Server] Advertising restarted.");
    oldBleDeviceConnected = bleDeviceConnected;
  }
  if (bleDeviceConnected && !oldBleDeviceConnected) {
    oldBleDeviceConnected = bleDeviceConnected;
  }

  if (connected) {
    updateConnectedHeartbeat();

    // Periodically poll strap battery percentage (every 30 seconds)
    if (now - lastStrapBatteryReadMs >= STRAP_BATTERY_POLL_INTERVAL_MS) {
      lastStrapBatteryReadMs = now;
      readStrapBattery();
    }

    // Log connection stats periodically
    if (now - lastStatusLogAt >= STATUS_LOG_INTERVAL_MS) {
      lastStatusLogAt = now;
      if (now - lastHeartRateUpdateMs < 5000) {
        Serial.printf("[BLE] Link OK | Heart Rate: %u bpm | R-R: %u ms | Strap Battery: %u%%\n", 
                      currentHeartRate, currentRrInterval, currentStrapBattery);
      } else {
        Serial.println("[BLE] Link OK | Connected, waiting for heart rate telemetry...");
      }
    }

    if (now - lastBatteryLogAt >= BATTERY_LOG_INTERVAL_MS) {
      lastBatteryLogAt = now;
      printBatteryStatus();
    }

  } else if (doConnect) {
    // Transition status color to constant Blue while pairing
    setStatusLed(0, 0, STATUS_LED_BRIGHTNESS);
    if (connectToServer()) {
      connected = true;
      flashGreenQuickly();
      lastHeartbeatAt = millis();
      heartbeatOn = false;
    } else {
      connected = false;
      doScan = true;
    }
    doConnect = false;

  } else {
    // If disconnected and not currently establishing a connection, scan for devices
    Serial.println("[BLE] Scanning for PowerLabs monitor...");

    if (myDevice != nullptr) {
      delete myDevice;
      myDevice = nullptr;
    }

    // Flash Red: 500ms ON, then OFF during 1-second active scan
    setStatusLed(STATUS_LED_BRIGHTNESS, 0, 0); // Red ON
    delay(500);
    setStatusLed(0, 0, 0); // Red OFF

    BLEDevice::getScan()->start(1, false); // Synchronously scan for 1 second

    if (now - lastBatteryLogAt >= BATTERY_LOG_INTERVAL_MS) {
      lastBatteryLogAt = now;
      printBatteryStatus();
    }
  }

  delay(LOOP_DELAY_MS);
}
