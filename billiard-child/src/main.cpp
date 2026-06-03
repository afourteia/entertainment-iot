#include <Arduino.h>
#include <HardwareSerial.h>

#include "I2C_Driver.h"
#include "WS_GPIO.h"
#include "WS_Relay.h"

namespace {
constexpr uint8_t kRelayCount = 8;
constexpr uint8_t kSlaveId = 0x06;
constexpr uint32_t kRs485Baud = 9600;
constexpr uint32_t kFrameIdleMs = 12;

HardwareSerial rs485(1);

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

void sendFrame(uint8_t *frame, size_t payloadLength) {
  appendCrc(frame, payloadLength);
  rs485.write(frame, payloadLength + 2);
  rs485.flush();
}

void sendException(uint8_t functionCode, uint8_t exceptionCode) {
  uint8_t response[5] = {
      kSlaveId,
      static_cast<uint8_t>(functionCode | 0x80),
      exceptionCode,
      0x00,
      0x00,
  };
  sendFrame(response, 3);
}

void forceRelaysOff() {
  Relay_CHxs_PinState(0x00);
  for (uint8_t i = 0; i < kRelayCount; i++) {
    Relay_Flag[i] = false;
  }
}

void setRelay(uint8_t channel, bool state) {
  if (channel < 1 || channel > kRelayCount) {
    return;
  }

  if (Relay_Flag[channel - 1] != state) {
    Relay_CHx(channel, state);
    Relay_Flag[channel - 1] = state;
  }
}

void handleWriteSingleCoil(const uint8_t *request, size_t length) {
  if (length != 8) {
    sendException(0x05, 0x03);
    return;
  }

  const uint16_t address =
      (static_cast<uint16_t>(request[2]) << 8) | request[3];
  const uint16_t value = (static_cast<uint16_t>(request[4]) << 8) | request[5];

  if (address >= kRelayCount) {
    sendException(0x05, 0x02);
    return;
  }

  if (value != 0xFF00 && value != 0x0000) {
    sendException(0x05, 0x03);
    return;
  }

  setRelay(address + 1, value == 0xFF00);
  rs485.write(request, length);
  rs485.flush();
}

void handleReadCoils(const uint8_t *request, size_t length) {
  if (length != 8) {
    sendException(0x01, 0x03);
    return;
  }

  const uint16_t startAddress =
      (static_cast<uint16_t>(request[2]) << 8) | request[3];
  const uint16_t quantity =
      (static_cast<uint16_t>(request[4]) << 8) | request[5];

  if (quantity < 1 || quantity > kRelayCount) {
    sendException(0x01, 0x03);
    return;
  }

  if (startAddress >= kRelayCount || startAddress + quantity > kRelayCount) {
    sendException(0x01, 0x02);
    return;
  }

  uint8_t coilByte = 0;
  for (uint8_t i = 0; i < quantity; i++) {
    if (Relay_Flag[startAddress + i]) {
      coilByte |= 1 << i;
    }
  }

  uint8_t response[6] = {
      kSlaveId,
      0x01,
      0x01,
      coilByte,
      0x00,
      0x00,
  };
  sendFrame(response, 4);
}

void handleModbusFrame(const uint8_t *frame, size_t length) {
  if (length < 4 || frame[0] != kSlaveId || !hasValidCrc(frame, length)) {
    return;
  }

  switch (frame[1]) {
    case 0x01:
      handleReadCoils(frame, length);
      break;
    case 0x05:
      handleWriteSingleCoil(frame, length);
      break;
    default:
      sendException(frame[1], 0x01);
      break;
  }
}

void pollRs485() {
  static uint8_t buffer[32] = {0};
  static size_t length = 0;
  static uint32_t lastByteMs = 0;

  while (rs485.available() > 0 && length < sizeof(buffer)) {
    buffer[length++] = static_cast<uint8_t>(rs485.read());
    lastByteMs = millis();
  }

  if (length > 0 && millis() - lastByteMs >= kFrameIdleMs) {
    handleModbusFrame(buffer, length);
    memset(buffer, 0, sizeof(buffer));
    length = 0;
  }

  if (length == sizeof(buffer)) {
    memset(buffer, 0, sizeof(buffer));
    length = 0;
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  GPIO_Init();
  I2C_Init();
  Relay_Init();
  forceRelaysOff();
  rs485.begin(kRs485Baud, SERIAL_8N1, RXD1, TXD1);
  printf("Billiard child Modbus RTU slave started\r\n");
}

void loop() {
  pollRs485();
  delay(1);
}
