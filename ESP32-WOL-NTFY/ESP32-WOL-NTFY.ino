// ESP32 Wake-on-LAN bot driven by ntfy.
//
// Magic packets are link-layer broadcasts and cannot cross the internet, so
// something has to already be inside the network to emit them. This is that
// something: ntfy carries the command in from anywhere, the ESP32 turns it
// into a broadcast on the local segment.
//
// All configuration lives in NVS and is edited from the built-in web portal --
// nothing user-specific is compiled in. See README.md.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "commands.h"
#include "config.h"
#include "ntfy.h"
#include "settings.h"
#include "store.h"
#include "webconfig.h"

// What the device is currently doing. `goOnline()` used to be the only path,
// implicit in setup(); now that WiFi can come and go while the portal stays
// up, the transitions need to be explicit.
enum class Mode { SetupPortal, ApFallback, Online };
static Mode mode = Mode::SetupPortal;

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
    bool isOurs = (WiFi.SSID(i) == g_settings.wifiSsid);
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
                  g_settings.wifiSsid.c_str());
  }
}

static bool connectWifi(uint32_t timeoutMs) {
  Serial.printf("[wifi] joining %s\n", g_settings.wifiSsid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // keeps the long-lived ntfy socket responsive
  WiFi.setAutoReconnect(true);
  WiFi.begin(g_settings.wifiSsid.c_str(), g_settings.wifiPass.c_str());

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

// Pressing BOOT (GPIO0) just after a reset forces the setup portal on an
// already-provisioned device -- the way back from a forgotten admin password
// or a moved network. It has to be pressed AFTER releasing EN: GPIO0 held low
// through the reset itself puts the ESP32 into its serial bootloader instead.
static bool bootButtonPressed() {
  pinMode(0, INPUT_PULLUP);
  Serial.println("[boot] press BOOT now to open the setup portal");
  uint32_t start = millis();
  while (millis() - start < BOOT_WINDOW_MS) {
    if (digitalRead(0) == LOW) return true;
    delay(20);
  }
  return false;
}

// Portal with no WiFi behind it. Deliberately has no timeout: an unprovisioned
// device has no useful fallback state -- no credentials means no WiFi, no ntfy
// and no purpose -- so a page telling you how to fix that is the most useful
// thing it can be doing.
static void runSetupPortal() {
  mode = Mode::SetupPortal;

  // Deliberately open. This is only reached when the device is unprovisioned
  // (nothing to protect yet) or when someone pressed the BOOT button, which
  // already implies physical access -- and anyone holding the board can reflash
  // it regardless. Requiring the admin password here would turn a forgotten
  // password into a lockout with no way back except erasing flash over USB.
  webconfig::beginAP(/*requireAuth=*/false);

  Serial.println("\n=== SETUP MODE ===");
  uint32_t lastNag = 0;
  while (!webconfig::saved()) {
    webconfig::tick();
    if (millis() - lastNag > 5000) {
      lastNag = millis();
      Serial.printf("[setup] join wifi \"%s\" then open %s\n",
                    webconfig::apSsid().c_str(),
                    webconfig::settingsUrl().c_str());
    }
    delay(5);
  }
  // handleSave() restarts the device, so this is never reached.
}

static void goOnline() {
  mode = Mode::Online;
  syncClock();
  webconfig::beginSTA();
  ntfy::begin(commands::handle);

  ntfy::publish("WOL bot online\n"
                "ip: " + WiFi.localIP().toString() + "\n"
                "settings: " + webconfig::settingsUrl() + "\n" +
                String(store::count()) + " computer(s) saved. "
                "Send \"<passphrase> help\" for commands.",
                "Online");

  Serial.printf("[boot] online, heap %u bytes free\n", (unsigned)ESP.getFreeHeap());
}

// WiFi is configured but unreachable. Bring the portal up so the settings can
// be fixed, while the station side keeps retrying in the background -- a router
// coming back should not need a reboot.
static void startApFallback() {
  mode = Mode::ApFallback;
  webconfig::beginAP(/*requireAuth=*/true);
  Serial.printf("[wifi] unreachable; portal at %s while retrying in background\n",
                webconfig::settingsUrl().c_str());
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println("\n\n=== ESP32 WOL-NTFY ===");

  store::begin();
  settings_begin();

  // Only worth waiting for on a provisioned board; otherwise setup is next anyway.
  bool forced = g_settings.provisioned && bootButtonPressed();
  if (forced) Serial.println("[boot] BOOT pressed -- forcing setup portal");

  if (!g_settings.provisioned || forced) {
    runSetupPortal();
  } else if (!connectWifi(WIFI_CONNECT_TIMEOUT_MS)) {
    startApFallback();
  } else {
    goOnline();
  }
}

void loop() {
  webconfig::tick();          // the portal answers in every mode

  switch (mode) {
    case Mode::SetupPortal:
      break;                  // runSetupPortal() blocks; nothing to do here

    case Mode::ApFallback:
      // WIFI_AP_STA means the station side is still trying underneath us.
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[wifi] back up, leaving AP mode");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        goOnline();
      } else if (millis() - lastWifiAttemptMs > WIFI_RETRY_INTERVAL_MS) {
        lastWifiAttemptMs = millis();
        Serial.printf("[wifi] retrying (%s)\n", wifiStatusName(WiFi.status()));
        WiFi.begin(g_settings.wifiSsid.c_str(), g_settings.wifiPass.c_str());
      }
      break;

    case Mode::Online:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifi] link lost");
        startApFallback();
        break;
      }
      ntfy::poll();
      break;
  }

  delay(10);   // yields to the WiFi stack; nothing above blocks
}
