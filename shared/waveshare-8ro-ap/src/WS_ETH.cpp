#include "WS_ETH.h"
#include <SPI.h>

#ifndef ETH_PHY_TYPE
#define ETH_PHY_TYPE ETH_PHY_W5500
#define ETH_PHY_ADDR 1
#define ETH_PHY_CS   16
#define ETH_PHY_IRQ  12
#define ETH_PHY_RST  39
#endif

#define ETH_SPI_SCK  15
#define ETH_SPI_MISO 14
#define ETH_SPI_MOSI 13

#include <ETH.h>

#ifndef ETH_PHY_W5500
#error "This Waveshare W5500 Ethernet module requires Arduino-ESP32 3.x or newer. Use the pioarduino espressif32 platform in platformio.ini."
#endif

static bool s_eth_connected = false;
static IPAddress s_eth_ip;
static const char* s_hostname = nullptr;

static void onEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      if (s_hostname) {
        ETH.setHostname(s_hostname);
      }
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      s_eth_ip = ETH.localIP();
      Serial.print("ETH Got IP: ");
      Serial.println(s_eth_ip);
      s_eth_connected = true;
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Disconnected");
      s_eth_connected = false;
      break;
    default:
      break;
  }
}

void WS_ETH_Init(const char* hostname) {
  s_hostname = hostname;
  s_eth_connected = false;

  Serial.println("WS_ETH: Starting Ethernet");
  Network.onEvent(onEvent);
  SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_CS, ETH_PHY_IRQ, ETH_PHY_RST, SPI);

  // Block until connected (with safe timeout)
  const unsigned long start = millis();
  const unsigned long timeoutMs = 15000; // 15s
  while (!s_eth_connected && (millis() - start) < timeoutMs) {
    delay(50);
  }

  if (!s_eth_connected) {
    Serial.println("WS_ETH: Failed to get IP within timeout");
  }
}

IPAddress WS_ETH_localIP() {
  return s_eth_ip;
}

bool WS_ETH_connected() {
  return s_eth_connected;
}
