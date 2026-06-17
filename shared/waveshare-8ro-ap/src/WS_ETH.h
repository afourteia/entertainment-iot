#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// Initialize Ethernet and block until connected (prints status to Serial).
// Optional: pass hostname to set via Ethernet interface.
void WS_ETH_Init(const char* hostname = nullptr);

// Returns the local IP address assigned to the Ethernet interface.
IPAddress WS_ETH_localIP();

// Non-blocking connected check
bool WS_ETH_connected();
