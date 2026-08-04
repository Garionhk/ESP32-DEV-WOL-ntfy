// ESP32 Wake-on-LAN bot driven by ntfy.
//
// Magic packets are link-layer broadcasts and cannot cross the internet, so
// something has to already be inside the network to emit them. This is that
// something: ntfy carries the command in from anywhere, the ESP32 turns it
// into a broadcast on the local segment.
//
// See README.md for setup. Credentials go in secrets.h.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "commands.h"
#include "config.h"
#include "ntfy.h"
#include "secrets.h"
#include "store.h"

static uint32_t lastWifiAttemptMs = 0;

static const char* wifiStatusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (network not seen)";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (usually a wrong password)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
  }
}

// Runs when the join fails. The ESP32 is 2.4 GHz only, so the single most
// useful fact is whether it can even see the configured SSID -- a 5 GHz-only
// network is invisible here and looks identical to a typo.
static void diagnoseWifi() {
  Serial.println("[wifi] scanning to see what is actually in range...");

  // The radio must be idle to scan. Scanning while the station is still trying
  // to associate returns 0 networks and looks exactly like a dead antenna.
  WiFi.disconnect(false, false);
  delay(200);

  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.printf("[wifi] scan returned %d networks. If this persists, suspect "
                  "the antenna or an underpowered USB port rather than the "
                  "credentials.\n", n);
    return;
  }

  bool found = false;
  Serial.printf("[wifi] %d network(s) visible on 2.4 GHz:\n", n);
  for (int i = 0; i < n; i++) {
    bool isOurs = (WiFi.SSID(i) == String(WIFI_SSID));
    if (isOurs) found = true;
    Serial.printf("  %-32s ch%-3d %4d dBm  %s%s\n",
                  WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured",
                  isOurs ? "   <== configured SSID" : "");
  }
  WiFi.scanDelete();

  if (found) {
    Serial.println("[wifi] SSID is in range, so the password is the likely problem");
  } else {
    Serial.printf("[wifi] \"%s\" was NOT found. It is probably 5 GHz-only "
                  "(the ESP32 cannot see 5 GHz), hidden, or misspelled.\n",
                  WIFI_SSID);
  }
}

static bool connectWifi(uint32_t timeoutMs) {
  Serial.printf("[wifi] joining %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // keeps the long-lived ntfy socket responsive
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[wifi] failed to join: %s\n", wifiStatusName(WiFi.status()));
    diagnoseWifi();
    return false;
  }

  Serial.printf("[wifi] ip %s  mask %s  rssi %d dBm\n",
                WiFi.localIP().toString().c_str(),
                WiFi.subnetMask().toString().c_str(),
                WiFi.RSSI());
  return true;
}

// TLS certificate validation compares against the clock, so the device has to
// know roughly what year it is before its first HTTPS connection. Skipping
// this is the classic cause of handshakes that fail for no visible reason.
static bool syncClock() {
  Serial.print("[time] syncing");
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);

  uint32_t start = millis();
  while (millis() - start < NTP_SYNC_TIMEOUT_MS) {
    if (time(nullptr) > NTP_MIN_VALID_EPOCH) {
      time_t now = time(nullptr);
      Serial.printf("\n[time] %s", ctime(&now));
      return true;
    }
    delay(250);
    Serial.print(".");
  }

  Serial.println("\n[time] NTP sync failed -- TLS will likely reject certs");
  return false;
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println("\n\n=== ESP32 WOL-NTFY ===");

  store::begin();

  if (connectWifi(WIFI_CONNECT_TIMEOUT_MS)) {
    syncClock();
  }

  ntfy::begin(commands::handle);

  ntfy::publish("WOL bot online at " + WiFi.localIP().toString() + "\n" +
                    String(store::count()) + " computer(s) saved. Send \"help\" for commands.",
                "Online");

  Serial.printf("[boot] ready, heap %u bytes free\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiAttemptMs > WIFI_RETRY_INTERVAL_MS) {
      lastWifiAttemptMs = millis();
      Serial.printf("[wifi] link down (%s), retrying\n",
                    wifiStatusName(WiFi.status()));
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    delay(100);
    return;
  }

  ntfy::poll();
  delay(10);   // yields to the WiFi stack; the poll path itself never blocks
}
