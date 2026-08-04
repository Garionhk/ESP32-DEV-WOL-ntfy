// In-memory stand-in for the ESP32 NVS Preferences library, so the store's
// persistence path can be exercised (and survive a simulated reboot) on host.
#pragma once

#include <map>
#include <string>
#include "Arduino.h"

class Preferences {
 public:
  // static so a fresh Preferences instance still sees prior writes -- that is
  // what lets the test simulate a power cycle.
  static std::map<std::string, std::string> backing;

  bool begin(const char* ns, bool readOnly = false) { (void)ns; (void)readOnly; return true; }

  size_t putString(const char* key, const String& value) {
    backing[key] = value.s;
    return value.s.size();
  }

  String getString(const char* key, const String& fallback) {
    auto it = backing.find(key);
    return it == backing.end() ? fallback : String(it->second);
  }
};
