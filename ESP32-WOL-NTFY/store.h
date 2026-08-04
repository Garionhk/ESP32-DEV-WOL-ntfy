#pragma once

#include <Arduino.h>
#include <functional>

// Persistent list of computers this device can wake, kept in NVS as a single
// JSON blob. One read and one write per operation, which keeps flash wear low
// and makes the whole list trivially inspectable over serial.

namespace store {

enum class AddResult {
  Ok,
  Duplicate,   // name already taken
  Full,        // MAX_HOSTS reached
  BadName,     // empty, too long, or illegal characters
};

// Opens the NVS namespace and loads the list into RAM. Call once from setup().
bool begin();

int count();

// Looks up a host by name, case-insensitively. Writes 6 bytes to macOut.
bool find(const String& name, uint8_t macOut[6]);

AddResult add(const String& name, const uint8_t mac[6]);

// Returns false if no host by that name existed.
bool remove(const String& name);

void forEach(const std::function<void(const String& name, const uint8_t* mac)>& fn);

// ---- helpers shared with the command parser ----

// Accepts "aa:bb:cc:dd:ee:ff", "aa-bb-cc-dd-ee-ff", or bare "aabbccddeeff".
// Writes 6 bytes to out. Returns false if the text is not a valid MAC.
bool parseMac(const String& text, uint8_t out[6]);

// Renders a MAC as lowercase colon-separated text.
String formatMac(const uint8_t mac[6]);

// Lowercases and validates a nickname: [a-z0-9_-], 1..MAX_NAME_LEN chars.
// Returns an empty String if the name is not acceptable.
String normalizeName(const String& name);

}  // namespace store
