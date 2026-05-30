#include <Arduino.h>
#include <Wire.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <algorithm>
#include <cctype>

namespace {
// BLE Service and Characteristic UUIDs (Standard Heart Rate Service)
static BLEUUID serviceUUID("180d");
static BLEUUID charUUID("2a37");

// BLE connection control states
bool doConnect = false;
bool connected = false;
bool doScan = false;

BLEAdvertisedDevice* myDevice = nullptr;
BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
BLEClient* pClient = nullptr;

// Heart rate tracking variables
uint16_t currentHeartRate = 0;
unsigned long lastHeartRateUpdateMs = 0;

// Logging & Heartbeat timing
constexpr uint8_t STATUS_LED_PIN = PIN_NEOPIXEL;
constexpr uint8_t STATUS_LED_BRIGHTNESS = 4; // Slightly brighter for visibility
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 10000;
constexpr unsigned long HEARTBEAT_ON_MS = 1000;
constexpr unsigned long STATUS_LOG_INTERVAL_MS = 5000;
constexpr unsigned long BATTERY_LOG_INTERVAL_MS = 10000;
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
  uint16_t heartRate = 0;

  // Flag bit 0: Heart Rate Value Format (0 = UINT8, 1 = UINT16)
  if (flags & 0x01) {
    if (length >= 3) {
      heartRate = pData[1] | (pData[2] << 8);
    }
  } else {
    heartRate = pData[1];
  }

  if (heartRate > 0 && heartRate < 240) {
    currentHeartRate = heartRate;
    lastHeartRateUpdateMs = millis();
  }
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) override {
    // Managed during connectToServer execution flow
  }

  void onDisconnect(BLEClient* pclient) override {
    connected = false;
    Serial.println("[BLE] Disconnected from BLE Heart Rate Monitor.");
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

  printBatteryStatus();

  // Initialize ESP32-C6 BLE hardware as a Central Client
  BLEDevice::init("ESP32-C6-Feather-Client");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);

  Serial.println("[BLE] Bluetooth hardware initialized.");
}

void loop() {
  const unsigned long now = millis();

  if (connected) {
    updateConnectedHeartbeat();

    // Log connection stats periodically
    if (now - lastStatusLogAt >= STATUS_LOG_INTERVAL_MS) {
      lastStatusLogAt = now;
      if (now - lastHeartRateUpdateMs < 5000) {
        Serial.printf("[BLE] Link OK | Heart Rate: %u bpm\n", currentHeartRate);
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

    // Pulse LED Blue while scanning
    setStatusLed(0, 0, STATUS_LED_BRIGHTNESS);

    if (myDevice != nullptr) {
      delete myDevice;
      myDevice = nullptr;
    }

    BLEDevice::getScan()->start(4, false); // Synchronously scan for 4 seconds

    setStatusLed(0, 0, 0); // Brief dark LED between scans

    if (now - lastBatteryLogAt >= BATTERY_LOG_INTERVAL_MS) {
      lastBatteryLogAt = now;
      printBatteryStatus();
    }
  }

  delay(LOOP_DELAY_MS);
}
