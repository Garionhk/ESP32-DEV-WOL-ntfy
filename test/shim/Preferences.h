// In-memory stand-in for the ESP32 NVS Preferences library, so the store and
// settings persistence paths can be exercised (and survive a simulated reboot)
// on the host.
#pragma once

#include <map>
#include <string>
#include "Arduino.h"

class Preferences {
 public:
  // static so a fresh Preferences instance still sees prior writes -- that is
  // what lets the tests simulate a power cycle.
  static std::map<std::string, std::string> backing;

  std::string ns;

  bool begin(const char* name, bool readOnly = false) {
    (void)readOnly;
    ns = name;
    return true;
  }
  void end() {}

  std::string k(const char* key) const { return ns + "/" + key; }

  size_t putString(const char* key, const String& value) {
    backing[k(key)] = value.s;
    return value.s.size();
  }
  String getString(const char* key, const String& fallback) {
    auto it = backing.find(k(key));
    return it == backing.end() ? fallback : String(it->second);
  }

  size_t putUInt(const char* key, uint32_t v) {
    backing[k(key)] = std::to_string(v);
    return sizeof(v);
  }
  uint32_t getUInt(const char* key, uint32_t fallback) {
    auto it = backing.find(k(key));
    return it == backing.end() ? fallback : (uint32_t)std::stoul(it->second);
  }

  size_t putBool(const char* key, bool v) {
    backing[k(key)] = v ? "1" : "0";
    return 1;
  }
  bool getBool(const char* key, bool fallback) {
    auto it = backing.find(k(key));
    return it == backing.end() ? fallback : (it->second == "1");
  }

  bool clear() {
    for (auto it = backing.begin(); it != backing.end();) {
      if (it->first.rfind(ns + "/", 0) == 0) it = backing.erase(it);
      else ++it;
    }
    return true;
  }
};
