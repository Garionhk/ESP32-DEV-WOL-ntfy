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

extern bool g_restarted;

struct FakeESP {
  unsigned getFreeHeap() const { return 220 * 1024; }
  // Records the reboot instead of taking one, so factoryreset is testable.
  void restart() const { g_restarted = true; }
};
extern FakeESP ESP;

inline void delay(unsigned long) {}

// Test-controlled clock so the auth lockout can be advanced without sleeping.
extern unsigned long fakeMillis;
inline unsigned long millis() { return fakeMillis; }
