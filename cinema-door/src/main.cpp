#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "I2C_Driver.h"
#include "WS_GPIO.h"
#include "WS_Relay.h"

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

namespace {
constexpr uint8_t kDoorRelayChannel = 1;
constexpr uint32_t kDefaultOpenSeconds = 10;
constexpr uint32_t kMaxOpenSeconds = 120;

WebServer server(80);
bool doorOpen = false;
uint32_t closeAtMs = 0;

void forceRelaysOff() {
  Relay_CHxs_PinState(0x00);
  for (uint8_t i = 0; i < 8; i++) {
    Relay_Flag[i] = false;
  }
}

void setDoorRelay(bool state) {
  if (Relay_Flag[kDoorRelayChannel - 1] != state) {
    Relay_CHx(kDoorRelayChannel, state);
    Relay_Flag[kDoorRelayChannel - 1] = state;
  }
  doorOpen = state;
}

void sendJson(int code, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void sendError(int code, const String &message) {
  sendJson(code, "{\"error\":\"" + message + "\"}");
}

bool parseSeconds(uint32_t *seconds) {
  if (!server.hasArg("seconds")) {
    *seconds = kDefaultOpenSeconds;
    return true;
  }

  String raw = server.arg("seconds");
  raw.trim();
  if (raw.length() == 0) {
    return false;
  }

  for (size_t i = 0; i < raw.length(); i++) {
    if (!isDigit(raw[i])) {
      return false;
    }
  }

  const int parsed = raw.toInt();
  if (parsed < 1 || parsed > static_cast<int>(kMaxOpenSeconds)) {
    return false;
  }

  *seconds = static_cast<uint32_t>(parsed);
  return true;
}

void handleOpen() {
  uint32_t seconds = 0;
  if (!parseSeconds(&seconds)) {
    sendError(400, "seconds must be between 1 and 120");
    return;
  }

  setDoorRelay(true);
  closeAtMs = millis() + seconds * 1000UL;

  String json = "{";
  json += "\"relay\":1,\"open\":true,\"seconds\":";
  json += String(seconds);
  json += "}";
  sendJson(200, json);
}

void handleStatus() {
  String json = "{";
  json += "\"relay\":1,\"open\":";
  json += doorOpen ? "true" : "false";
  json += "}";
  sendJson(200, json);
}

void handleNotFound() { sendError(404, "Not found"); }

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  if (String(WIFI_PASSWORD).length() == 0) {
    WiFi.begin(WIFI_SSID);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  printf("Connecting to WiFi SSID \"%s\"", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    printf(".");
  }
  printf("\r\nWiFi connected: %s\r\n", WiFi.localIP().toString().c_str());
}

void configureHttpServer() {
  server.on("/", HTTP_GET, handleStatus);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/open", HTTP_GET, handleOpen);
  server.onNotFound(handleNotFound);
  server.begin();
  printf("HTTP server started on port 80\r\n");
}

void tickDoorAutoClose() {
  if (doorOpen && static_cast<int32_t>(millis() - closeAtMs) >= 0) {
    setDoorRelay(false);
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  GPIO_Init();
  I2C_Init();
  Relay_Init();
  forceRelaysOff();
  connectWiFi();
  configureHttpServer();
}

void loop() {
  server.handleClient();
  tickDoorAutoClose();
  delay(5);
}
