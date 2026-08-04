// Enough of the WiFi/ESP surface for commands.cpp to build on the host.
#pragma once

#include "Arduino.h"

struct FakeIP {
  String toString() const { return String("192.168.1.50"); }
};

struct FakeWiFi {
  FakeIP localIP() const { return FakeIP(); }
  String SSID() const { return String("test-net"); }
  int    RSSI() const { return -55; }
};
extern FakeWiFi WiFi;

struct FakeESP {
  unsigned getFreeHeap() const { return 220 * 1024; }
};
extern FakeESP ESP;

// Test-controlled clock so the auth lockout can be advanced without sleeping.
extern unsigned long fakeMillis;
inline unsigned long millis() { return fakeMillis; }
