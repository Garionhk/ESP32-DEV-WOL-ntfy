#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "config.h"
#include "store.h"

namespace store {
namespace {

struct Host {
  String  name;       // already normalized: lowercase, validated
  uint8_t mac[6];
};

Preferences prefs;
Host        hosts[MAX_HOSTS];
int         hostCount = 0;
bool        ready     = false;

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// MACs are stored as 12 bare hex characters; colons are presentation only.
String macToHex(const uint8_t mac[6]) {
  char buf[13];
  for (int i = 0; i < 6; i++) snprintf(buf + i * 2, 3, "%02x", mac[i]);
  buf[12] = '\0';
  return String(buf);
}

int indexOf(const String& normalized) {
  for (int i = 0; i < hostCount; i++) {
    if (hosts[i].name == normalized) return i;
  }
  return -1;
}

// Serializes the in-RAM list back to NVS. Called after every mutation.
bool persist() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < hostCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["n"] = hosts[i].name;
    o["m"] = macToHex(hosts[i].mac);
  }

  String out;
  serializeJson(doc, out);

  size_t written = prefs.putString(NVS_KEY_HOSTS, out);
  if (written == 0 && out.length() > 2) {
    Serial.println("[store] NVS write failed");
    return false;
  }
  return true;
}

void load() {
  hostCount = 0;

  String raw = prefs.getString(NVS_KEY_HOSTS, "[]");
  if (raw.isEmpty()) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err) {
    // A corrupt blob would otherwise wedge the device on every boot, so drop
    // it and carry on with an empty list.
    Serial.printf("[store] stored list is unreadable (%s), starting empty\n",
                  err.c_str());
    return;
  }

  for (JsonObject o : doc.as<JsonArray>()) {
    if (hostCount >= MAX_HOSTS) break;

    String name = normalizeName(o["n"] | "");
    uint8_t mac[6];
    if (name.isEmpty() || !parseMac(o["m"] | "", mac)) continue;

    hosts[hostCount].name = name;
    memcpy(hosts[hostCount].mac, mac, 6);
    hostCount++;
  }

  Serial.printf("[store] loaded %d host(s)\n", hostCount);
}

}  // namespace

bool begin() {
  if (!prefs.begin(NVS_NAMESPACE, /*readOnly=*/false)) {
    Serial.println("[store] could not open NVS namespace");
    return false;
  }
  ready = true;
  load();
  return true;
}

int count() { return hostCount; }

bool find(const String& name, uint8_t macOut[6]) {
  int i = indexOf(normalizeName(name));
  if (i < 0) return false;
  memcpy(macOut, hosts[i].mac, 6);
  return true;
}

AddResult add(const String& name, const uint8_t mac[6]) {
  String normalized = normalizeName(name);
  if (normalized.isEmpty())    return AddResult::BadName;
  if (indexOf(normalized) >= 0) return AddResult::Duplicate;
  if (hostCount >= MAX_HOSTS)   return AddResult::Full;

  hosts[hostCount].name = normalized;
  memcpy(hosts[hostCount].mac, mac, 6);
  hostCount++;

  persist();
  return AddResult::Ok;
}

bool remove(const String& name) {
  int i = indexOf(normalizeName(name));
  if (i < 0) return false;

  for (int j = i; j < hostCount - 1; j++) hosts[j] = hosts[j + 1];
  hostCount--;
  hosts[hostCount].name = "";

  persist();
  return true;
}

void forEach(const std::function<void(const String&, const uint8_t*)>& fn) {
  for (int i = 0; i < hostCount; i++) fn(hosts[i].name, hosts[i].mac);
}

bool parseMac(const String& text, uint8_t out[6]) {
  // Strip the separators people actually type, then require exactly 12 hex
  // digits. This accepts aa:bb:.., aa-bb-.., and bare aabbccddeeff.
  String hex;
  hex.reserve(12);
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == ':' || c == '-' || c == '.' || c == ' ') continue;
    if (hexVal(c) < 0) return false;
    hex += c;
    if (hex.length() > 12) return false;
  }
  if (hex.length() != 12) return false;

  for (int i = 0; i < 6; i++) {
    out[i] = (uint8_t)((hexVal(hex[i * 2]) << 4) | hexVal(hex[i * 2 + 1]));
  }
  return true;
}

String formatMac(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

String normalizeName(const String& name) {
  String trimmed = name;
  trimmed.trim();
  if (trimmed.isEmpty() || trimmed.length() > MAX_NAME_LEN) return String();

  String out;
  out.reserve(trimmed.length());
  for (unsigned int i = 0; i < trimmed.length(); i++) {
    char c = tolower(trimmed[i]);
    bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '-';
    if (!allowed) return String();
    out += c;
  }
  return out;
}

}  // namespace store
