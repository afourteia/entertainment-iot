#include <Arduino.h>
#include <HardwareSerial.h>
#include <WebServer.h>
#include "WS_ETH.h"

#include "I2C_Driver.h"
#include "WS_GPIO.h"
#include "WS_Relay.h"

#ifndef ETH_HOSTNAME
#define ETH_HOSTNAME "billiard-iot"
#endif

namespace {
constexpr uint8_t kLocalRelayCount = 8;
constexpr uint8_t kTotalRelayCount = 16;
constexpr uint8_t kChildSlaveId = 0x06;
constexpr uint32_t kRs485Baud = 9600;
constexpr uint32_t kPresenceIntervalMs = 3000;
constexpr uint32_t kReapplyIntervalMs = 5000;
constexpr uint8_t kOfflineMissLimit = 3;

HardwareSerial rs485(1);
WebServer server(80);

bool desiredRelayState[kTotalRelayCount] = {false};
bool childOnline = false;
uint8_t missedPresenceCount = kOfflineMissLimit;
uint32_t lastChildSeenMs = 0;
uint32_t lastPresencePingMs = 0;
uint32_t lastReapplyMs = 0;

uint16_t modbusCrc(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void appendCrc(uint8_t *frame, size_t payloadLength) {
  const uint16_t crc = modbusCrc(frame, payloadLength);
  frame[payloadLength] = crc & 0xFF;
  frame[payloadLength + 1] = (crc >> 8) & 0xFF;
}

bool hasValidCrc(const uint8_t *frame, size_t length) {
  if (length < 3) {
    return false;
  }

  const uint16_t expected =
      frame[length - 2] | static_cast<uint16_t>(frame[length - 1]) << 8;
  return modbusCrc(frame, length - 2) == expected;
}

void clearRs485Input() {
  while (rs485.available() > 0) {
    rs485.read();
  }
}

bool readExactRs485(uint8_t *response, size_t length, uint32_t timeoutMs) {
  const uint32_t start = millis();
  size_t offset = 0;

  while (offset < length && millis() - start < timeoutMs) {
    if (rs485.available() > 0) {
      response[offset++] = static_cast<uint8_t>(rs485.read());
    } else {
      delay(1);
    }
  }

  return offset == length;
}

bool writeChildCoil(uint8_t coilAddress, bool state) {
  if (coilAddress >= kLocalRelayCount) {
    return false;
  }

  uint8_t request[8] = {
      kChildSlaveId,
      0x05,
      0x00,
      coilAddress,
      static_cast<uint8_t>(state ? 0xFF : 0x00),
      0x00,
      0x00,
      0x00,
  };
  appendCrc(request, 6);

  clearRs485Input();
  rs485.write(request, sizeof(request));
  rs485.flush();

  uint8_t response[8] = {0};
  if (!readExactRs485(response, sizeof(response), 150)) {
    return false;
  }

  for (size_t i = 0; i < sizeof(request); i++) {
    if (response[i] != request[i]) {
      return false;
    }
  }

  return hasValidCrc(response, sizeof(response));
}

bool readChildPresence() {
  uint8_t request[8] = {
      kChildSlaveId,
      0x01,
      0x00,
      0x00,
      0x00,
      kLocalRelayCount,
      0x00,
      0x00,
  };
  appendCrc(request, 6);

  clearRs485Input();
  rs485.write(request, sizeof(request));
  rs485.flush();

  uint8_t response[6] = {0};
  if (!readExactRs485(response, sizeof(response), 150)) {
    return false;
  }

  return response[0] == kChildSlaveId && response[1] == 0x01 &&
         response[2] == 0x01 && hasValidCrc(response, sizeof(response));
}

void markChildPresence(bool present) {
  if (present) {
    childOnline = true;
    missedPresenceCount = 0;
    lastChildSeenMs = millis();
    return;
  }

  if (missedPresenceCount < 255) {
    missedPresenceCount++;
  }

  if (missedPresenceCount >= kOfflineMissLimit) {
    childOnline = false;
  }
}

void setLocalRelay(uint8_t channel, bool state) {
  if (channel < 1 || channel > kLocalRelayCount) {
    return;
  }

  if (Relay_Flag[channel - 1] != state) {
    Relay_CHx(channel, state);
    Relay_Flag[channel - 1] = state;
  }
}

void forceLocalRelaysOff() {
  Relay_CHxs_PinState(0x00);
  for (uint8_t i = 0; i < kLocalRelayCount; i++) {
    Relay_Flag[i] = false;
  }
}

void applyDesiredRelay(uint8_t channel) {
  if (channel < 1 || channel > kTotalRelayCount) {
    return;
  }

  const bool state = desiredRelayState[channel - 1];
  if (channel <= kLocalRelayCount) {
    setLocalRelay(channel, state);
    return;
  }

  writeChildCoil(channel - kLocalRelayCount - 1, state);
}

void reapplyDesiredStates() {
  for (uint8_t channel = 1; channel <= kTotalRelayCount; channel++) {
    applyDesiredRelay(channel);
  }
}

String trimCopy(String value) {
  value.trim();
  return value;
}

bool parseState(const String &rawValue, bool *state) {
  String value = rawValue;
  value.trim();
  value.toLowerCase();

  if (value == "on" || value == "1" || value == "true" || value == "open") {
    *state = true;
    return true;
  }

  if (value == "off" || value == "0" || value == "false" ||
      value == "close" || value == "closed") {
    *state = false;
    return true;
  }

  return false;
}

bool parseChannel(const String &rawValue, uint8_t *channel) {
  String value = rawValue;
  value.trim();
  if (value.length() == 0) {
    return false;
  }

  for (size_t i = 0; i < value.length(); i++) {
    if (!isDigit(value[i])) {
      return false;
    }
  }

  const int parsed = value.toInt();
  if (parsed < 1 || parsed > kTotalRelayCount) {
    return false;
  }

  *channel = static_cast<uint8_t>(parsed);
  return true;
}

bool addChannelAssignment(uint8_t channel, bool state, bool *seen,
                          bool *newStates, String *error) {
  const uint8_t index = channel - 1;
  if (seen[index] && newStates[index] != state) {
    *error = "Conflicting duplicate channel " + String(channel);
    return false;
  }

  seen[index] = true;
  newStates[index] = state;
  return true;
}

bool parseChannelList(const String &list, bool state, bool *seen,
                      bool *newStates, String *error) {
  int start = 0;
  bool parsedAny = false;

  while (start <= list.length()) {
    int comma = list.indexOf(',', start);
    if (comma < 0) {
      comma = list.length();
    }

    const String item = trimCopy(list.substring(start, comma));
    uint8_t channel = 0;
    if (!parseChannel(item, &channel)) {
      *error = "Invalid channel list";
      return false;
    }

    if (!addChannelAssignment(channel, state, seen, newStates, error)) {
      return false;
    }

    parsedAny = true;
    start = comma + 1;
  }

  if (!parsedAny) {
    *error = "Empty channel list";
    return false;
  }

  return true;
}

void sendJson(int code, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void sendError(int code, const String &message) {
  sendJson(code, "{\"error\":\"" + message + "\"}");
}

String statusJson() {
  String json = "{";
  json += "\"relays\":[";
  for (uint8_t i = 0; i < kTotalRelayCount; i++) {
    if (i > 0) {
      json += ",";
    }
    json += "{\"channel\":";
    json += String(i + 1);
    json += ",\"desired\":";
    json += desiredRelayState[i] ? "true" : "false";
    json += "}";
  }
  json += "],\"childOnline\":";
  json += childOnline ? "true" : "false";
  json += ",\"lastChildSeenMs\":";
  json += String(lastChildSeenMs);
  json += ",\"missedPresenceCount\":";
  json += String(missedPresenceCount);
  json += "}";
  return json;
}

void handleStatus() { sendJson(200, statusJson()); }

void handleRelayCommand() {
  bool seen[kTotalRelayCount] = {false};
  bool newStates[kTotalRelayCount] = {false};
  String error;

  if (server.hasArg("channel") || server.hasArg("channels")) {
    if (!server.hasArg("state")) {
      sendError(400, "Missing state");
      return;
    }

    bool state = false;
    if (!parseState(server.arg("state"), &state)) {
      sendError(400, "Invalid state");
      return;
    }

    if (server.hasArg("channel")) {
      uint8_t channel = 0;
      if (!parseChannel(server.arg("channel"), &channel) ||
          !addChannelAssignment(channel, state, seen, newStates, &error)) {
        sendError(400, error.length() ? error : "Invalid channel");
        return;
      }
    }

    if (server.hasArg("channels") &&
        !parseChannelList(server.arg("channels"), state, seen, newStates,
                          &error)) {
      sendError(400, error);
      return;
    }
  }

  if (server.hasArg("on") &&
      !parseChannelList(server.arg("on"), true, seen, newStates, &error)) {
    sendError(400, error);
    return;
  }

  if (server.hasArg("off") &&
      !parseChannelList(server.arg("off"), false, seen, newStates, &error)) {
    sendError(400, error);
    return;
  }

  bool hasCommand = false;
  for (uint8_t i = 0; i < kTotalRelayCount; i++) {
    if (seen[i]) {
      hasCommand = true;
      desiredRelayState[i] = newStates[i];
      applyDesiredRelay(i + 1);
    }
  }

  if (!hasCommand) {
    sendError(400, "No relay command provided");
    return;
  }

  sendJson(200, statusJson());
}

void handleNotFound() { sendError(404, "Not found"); }

void connectEthernet() {
  WS_ETH_Init(ETH_HOSTNAME);
  if (WS_ETH_connected()) {
    IPAddress ip = WS_ETH_localIP();
    printf("Ethernet connected: %d.%d.%d.%d\r\n", ip[0], ip[1], ip[2], ip[3]);
  } else {
    printf("Ethernet did not get an IP\r\n");
  }
}

void configureHttpServer() {
  server.on("/", HTTP_GET, handleStatus);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/relay", HTTP_GET, handleRelayCommand);
  server.onNotFound(handleNotFound);
  server.begin();
  printf("HTTP server started on port 80\r\n");
}

void setupRs485() { rs485.begin(kRs485Baud, SERIAL_8N1, RXD1, TXD1); }

void tickPresence() {
  if (millis() - lastPresencePingMs < kPresenceIntervalMs) {
    return;
  }

  lastPresencePingMs = millis();
  const bool wasOnline = childOnline;
  markChildPresence(readChildPresence());
  if (!wasOnline && childOnline) {
    for (uint8_t channel = 9; channel <= kTotalRelayCount; channel++) {
      applyDesiredRelay(channel);
    }
  }
}

void tickReapply() {
  if (millis() - lastReapplyMs < kReapplyIntervalMs) {
    return;
  }

  lastReapplyMs = millis();
  reapplyDesiredStates();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  GPIO_Init();
  I2C_Init();
  Relay_Init();
  forceLocalRelaysOff();
  setupRs485();
  connectEthernet();
  configureHttpServer();
}

void loop() {
  server.handleClient();
  tickPresence();
  tickReapply();
  delay(5);
}
