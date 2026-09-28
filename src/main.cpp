#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include "wifi_config.h"

namespace {
constexpr unsigned long kHeartbeatIntervalMs = 5000;
constexpr unsigned long kWifiReconnectIntervalMs = 10000;
constexpr unsigned long kMqttReconnectIntervalMs = 5000;
constexpr unsigned long kStatusLedBlinkIntervalMs = 500;
constexpr uint32_t kPwmFrequency = 5000;
constexpr uint8_t kPwmResolution = 8;
constexpr uint8_t kRedChannel = 0;
constexpr uint8_t kGreenChannel = 1;
constexpr uint8_t kBlueChannel = 2;

WiFiClient networkClient;
PubSubClient mqttClient(networkClient);

unsigned long lastHeartbeatMs = 0;
unsigned long lastWifiReconnectAttemptMs = 0;
unsigned long lastMqttReconnectAttemptMs = 0;
unsigned long lastStatusLedToggleMs = 0;
unsigned long heartbeat = 0;
bool statusLedOn = false;
char currentColor[] = "#000000";

const char* wifiDisconnectReason(uint8_t reason) {
  switch (reason) {
    case 200:
      return "beacon timeout";
    case 201:
      return "network not found";
    case 202:
      return "authentication failed";
    case 203:
      return "association failed";
    case 204:
      return "handshake timed out";
    case 205:
      return "connection failed";
    default:
      return "other";
  }
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    return;
  }

  const uint8_t reason = info.wifi_sta_disconnected.reason;
  Serial.printf("Wi-Fi event: disconnected, reason %u (%s)\n", reason,
                wifiDisconnectReason(reason));
}

uint8_t pwmValue(uint8_t component) {
  return wifi_config::kCommonAnode ? 255 - component : component;
}

void setRgb(uint8_t red, uint8_t green, uint8_t blue) {
  ledcWrite(kRedChannel, pwmValue(red));
  ledcWrite(kGreenChannel, pwmValue(green));
  ledcWrite(kBlueChannel, pwmValue(blue));
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return -1;
}

bool parseColor(const byte* payload, unsigned int length, uint8_t& red,
                uint8_t& green, uint8_t& blue) {
  if (length != 7 || payload[0] != '#') {
    return false;
  }

  int values[6];
  for (size_t index = 0; index < 6; ++index) {
    values[index] = hexNibble(static_cast<char>(payload[index + 1]));
    if (values[index] < 0) {
      return false;
    }
  }

  red = static_cast<uint8_t>((values[0] << 4) | values[1]);
  green = static_cast<uint8_t>((values[2] << 4) | values[3]);
  blue = static_cast<uint8_t>((values[4] << 4) | values[5]);
  return true;
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, wifi_config::kMqttTopic) != 0) {
    return;
  }

  uint8_t red;
  uint8_t green;
  uint8_t blue;
  if (!parseColor(payload, length, red, green, blue)) {
    Serial.println("MQTT ignored invalid color; expected #RRGGBB");
    return;
  }

  memcpy(currentColor, payload, 7);
  currentColor[7] = '\0';
  setRgb(red, green, blue);
  Serial.printf("MQTT color received: %s (R=%u G=%u B=%u)\n", currentColor,
                red, green, blue);
}

void configureRgbOutput() {
  ledcSetup(kRedChannel, kPwmFrequency, kPwmResolution);
  ledcSetup(kGreenChannel, kPwmFrequency, kPwmResolution);
  ledcSetup(kBlueChannel, kPwmFrequency, kPwmResolution);
  ledcAttachPin(wifi_config::kRedPin, kRedChannel);
  ledcAttachPin(wifi_config::kGreenPin, kGreenChannel);
  ledcAttachPin(wifi_config::kBluePin, kBlueChannel);
  setRgb(0, 0, 0);

  Serial.printf("RGB PWM pins: R=%u G=%u B=%u (%s)\n",
                wifi_config::kRedPin, wifi_config::kGreenPin,
                wifi_config::kBluePin,
                wifi_config::kCommonAnode ? "common anode" : "common cathode");
}

void setStatusLed(bool on) {
  statusLedOn = on;
  const bool pinHigh = wifi_config::kStatusLedActiveLow ? !on : on;
  digitalWrite(wifi_config::kStatusLedPin, pinHigh ? HIGH : LOW);
}

void configureStatusLed() {
  pinMode(wifi_config::kStatusLedPin, OUTPUT);
  setStatusLed(false);
  Serial.printf("Wi-Fi status LED: GPIO %u (%s)\n",
                wifi_config::kStatusLedPin,
                wifi_config::kStatusLedActiveLow ? "active low" : "active high");
}

void updateStatusLed(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) {
    setStatusLed(true);
    return;
  }

  if (now - lastStatusLedToggleMs >= kStatusLedBlinkIntervalMs) {
    lastStatusLedToggleMs = now;
    setStatusLed(!statusLedOn);
  }
}

void connectToWifi() {
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(wifi_config::kSsid);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(wifi_config::kSsid, wifi_config::kPassword);
  lastWifiReconnectAttemptMs = millis();
}

void connectToMqtt() {
  char clientId[32];
  const uint64_t chipId = ESP.getEfuseMac();
  snprintf(clientId, sizeof(clientId), "esp32-rgb-%08lX",
           static_cast<unsigned long>(chipId & 0xFFFFFFFF));

  Serial.printf("Connecting to MQTT at %s:%u...\n", wifi_config::kMqttHost,
                wifi_config::kMqttPort);
  if (!mqttClient.connect(clientId)) {
    Serial.printf("MQTT connection failed, state %d\n", mqttClient.state());
    return;
  }

  if (!mqttClient.subscribe(wifi_config::kMqttTopic)) {
    Serial.println("MQTT connected but subscription failed");
    mqttClient.disconnect();
    return;
  }

  Serial.print("MQTT subscribed: ");
  Serial.println(wifi_config::kMqttTopic);
}

void printStatus() {
  Serial.printf("heartbeat %lu - Wi-Fi %s, MQTT %s, color %s",
                heartbeat++, WiFi.status() == WL_CONNECTED ? "connected" : "down",
                mqttClient.connected() ? "connected" : "down", currentColor);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(", IP ");
    Serial.print(WiFi.localIP());
    Serial.printf(", RSSI %d dBm", WiFi.RSSI());
  }
  Serial.println();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("ESP32 RGB MQTT firmware started");

  configureStatusLed();
  configureRgbOutput();
  WiFi.onEvent(onWifiEvent);

  mqttClient.setServer(wifi_config::kMqttHost, wifi_config::kMqttPort);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(5);

  connectToWifi();
}

void loop() {
  const unsigned long now = millis();

  updateStatusLed(now);

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiReconnectAttemptMs >= kWifiReconnectIntervalMs) {
      lastWifiReconnectAttemptMs = now;
      Serial.println("Retrying Wi-Fi connection...");
      WiFi.reconnect();
    }
  } else {
    if (!mqttClient.connected() &&
        now - lastMqttReconnectAttemptMs >= kMqttReconnectIntervalMs) {
      lastMqttReconnectAttemptMs = now;
      connectToMqtt();
    }
    mqttClient.loop();
  }

  if (now - lastHeartbeatMs >= kHeartbeatIntervalMs) {
    lastHeartbeatMs = now;
    printStatus();
  }

  delay(10);
}
