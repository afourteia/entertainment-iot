#include <Arduino.h>

#include "I2C_Driver.h"
#include "WS_GPIO.h"
#include "WS_RTC.h"
#include "WS_Relay.h"
#include "WS_WIFI.h"

void setup() {
  GPIO_Init();
  I2C_Init();
  RTC_Init();
  Relay_Init();
  WIFI_Init();

  printf("Connect to WiFi network \"ESP32-S3-POE-ETH-8DI-8RO\" with password \"waveshare\".\r\n");
  printf("Then open http://192.168.4.1/ in a browser.\r\n");
}

void loop() {}
