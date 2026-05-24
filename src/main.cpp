#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "wifi_config.h"

#ifndef WIFI_PASSWORD
#error "WIFI_PASSWORD must be set in .env"
#endif

#ifndef WIFI_TARGET_BSSID
#error "WIFI_TARGET_BSSID must be set in .env"
#endif

namespace {
constexpr char WIFI_PASSWORD_TEXT[] = WIFI_PASSWORD;
constexpr char TARGET_BSSID_TEXT[] = WIFI_TARGET_BSSID;
constexpr uint8_t STATUS_LED_PIN = PIN_NEOPIXEL;
constexpr uint8_t STATUS_LED_BRIGHTNESS = 1;
constexpr unsigned long HEARTBEAT_INTERVAL_MS = 10000;
constexpr unsigned long HEARTBEAT_ON_MS = 1000;
constexpr unsigned long STATUS_LOG_INTERVAL_MS = 5000;
constexpr unsigned long BATTERY_LOG_INTERVAL_MS = 10000;
constexpr unsigned long LOOP_DELAY_MS = 50;
constexpr uint8_t MAX17048_ADDRESS = 0x36;
constexpr uint8_t MAX17048_VCELL_REGISTER = 0x02;
constexpr uint8_t MAX17048_SOC_REGISTER = 0x04;
constexpr uint8_t MAX17048_CRATE_REGISTER = 0x16;

uint8_t targetBssid[6] = {0};
unsigned long lastHeartbeatAt = 0;
unsigned long lastStatusLogAt = 0;
unsigned long lastBatteryLogAt = 0;
bool heartbeatOn = false;

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
    Serial.println("Battery: MAX17048 not found");
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

  Serial.print("Battery: ");
  Serial.print(voltage, 3);
  Serial.print("V | charge ");
  Serial.print(chargePercent, 1);
  Serial.print("% | rate ");
  Serial.print(ratePercentPerHour, 1);
  Serial.print("%/hr ");
  Serial.print(direction);
  Serial.print(" | health ");
  Serial.println(batteryHealthLabel(voltage, chargePercent));
}

void showConnecting() {
  heartbeatOn = false;
  setStatusLed(STATUS_LED_BRIGHTNESS, 0, 0);
}

void showConnected() {
  heartbeatOn = true;
  lastHeartbeatAt = millis();
  setStatusLed(0, STATUS_LED_BRIGHTNESS, 0);
}

void updateConnectedHeartbeat() {
  const unsigned long now = millis();

  if (heartbeatOn && now - lastHeartbeatAt >= HEARTBEAT_ON_MS) {
    setStatusLed(0, 0, 0);
    heartbeatOn = false;
    lastHeartbeatAt = now;
    return;
  }

  if (!heartbeatOn && now - lastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) {
    setStatusLed(0, STATUS_LED_BRIGHTNESS, 0);
    heartbeatOn = true;
    lastHeartbeatAt = now;
  }
}

bool parseBssid(const char *text, uint8_t out[6]) {
  return sscanf(text, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &out[0], &out[1],
                &out[2], &out[3], &out[4], &out[5]) == 6;
}

bool bssidMatches(int networkIndex) {
  const uint8_t *found = WiFi.BSSID(networkIndex);
  if (found == nullptr) {
    return false;
  }

  for (size_t i = 0; i < 6; ++i) {
    if (found[i] != targetBssid[i]) {
      return false;
    }
  }

  return true;
}

bool connectToTargetRouter() {
  showConnecting();

  if (!parseBssid(TARGET_BSSID_TEXT, targetBssid)) {
    Serial.println("Bad target BSSID.");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(500);

  Serial.printf("Scanning for router BSSID %s...\n", TARGET_BSSID_TEXT);
  const int networkCount = WiFi.scanNetworks(false, true);
  if (networkCount <= 0) {
    Serial.println("No Wi-Fi networks found.");
    return false;
  }

  Serial.printf("Found %d networks:\n", networkCount);
  for (int i = 0; i < networkCount; ++i) {
    Serial.printf("  %2d: BSSID %s | CH %2ld | RSSI %4ld | SSID '%s'\n", i + 1,
                  WiFi.BSSIDstr(i).c_str(), static_cast<long>(WiFi.channel(i)),
                  static_cast<long>(WiFi.RSSI(i)), WiFi.SSID(i).c_str());
  }

  for (int i = 0; i < networkCount; ++i) {
    if (!bssidMatches(i)) {
      continue;
    }

    const String ssid = WiFi.SSID(i);
    const int32_t channel = WiFi.channel(i);
    Serial.printf("Found SSID '%s' on channel %ld. Connecting...\n",
                  ssid.c_str(), static_cast<long>(channel));

    WiFi.begin(ssid.c_str(), WIFI_PASSWORD_TEXT, channel, targetBssid);

    const unsigned long startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 20000) {
      Serial.print(".");
      delay(500);
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      showConnected();
      Serial.println("Wi-Fi connected.");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("Gateway: ");
      Serial.println(WiFi.gatewayIP());
      Serial.print("DNS: ");
      Serial.println(WiFi.dnsIP());
      Serial.print("MAC: ");
      Serial.println(WiFi.macAddress());
      Serial.print("BSSID: ");
      Serial.println(WiFi.BSSIDstr());
      return true;
    }

    Serial.printf("Connection failed with status %d.\n", WiFi.status());
    return false;
  }

  Serial.println("Target router BSSID was not visible.");
  return false;
}
}  // namespace

void setup() {
  initStatusLed();
  Wire.begin();
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-C6 Feather Wi-Fi bootstrap");
  printBatteryStatus();
  connectToTargetRouter();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi dropped; reconnecting...");
    connectToTargetRouter();
  } else {
    updateConnectedHeartbeat();

    if (millis() - lastStatusLogAt >= STATUS_LOG_INTERVAL_MS) {
      lastStatusLogAt = millis();
      Serial.print("Wi-Fi OK. IP: ");
      Serial.print(WiFi.localIP());
      Serial.print(" | BSSID: ");
      Serial.println(WiFi.BSSIDstr());
    }

    if (millis() - lastBatteryLogAt >= BATTERY_LOG_INTERVAL_MS) {
      lastBatteryLogAt = millis();
      printBatteryStatus();
    }
  }

  delay(LOOP_DELAY_MS);
}
