#include <Arduino.h>
#include <Preferences.h>

#include "config.h"
#include "settings.h"

Settings g_settings;

namespace {

Preferences prefs;

// Deliberately not the namespace store.cpp uses. Keeping config and the saved
// computer list apart means a config reset need not destroy the host list.
const char* NS = "wolcfg";

// All well inside the 15-character NVS key limit.
const char* K_SSID  = "ssid";
const char* K_PASS  = "pass";
const char* K_HOST  = "host";
const char* K_PORT  = "port";
const char* K_TLS   = "tls";
const char* K_TOPIC = "topic";
const char* K_USER  = "user";
const char* K_NPASS = "npass";
const char* K_TOKEN = "token";
const char* K_CMD   = "cmdpass";
const char* K_ADMIN = "admpass";
const char* K_OK    = "ok";

}  // namespace

void settings_begin() {
  // Opened read-write even though this only reads: on a never-written
  // namespace a read-only open fails, which is the same first-boot quirk
  // store.cpp works around.
  if (!prefs.begin(NS, /*readOnly=*/false)) {
    Serial.println("[settings] could not open NVS, using defaults");
    return;
  }

  g_settings.provisioned = prefs.getBool(K_OK, false);
  if (!g_settings.provisioned) {
    Serial.println("[settings] no stored config -- first boot");
    prefs.end();
    return;
  }

  g_settings.wifiSsid  = prefs.getString(K_SSID, "");
  g_settings.wifiPass  = prefs.getString(K_PASS, "");
  g_settings.ntfyHost  = prefs.getString(K_HOST, "ntfy.sh");
  g_settings.ntfyPort  = (uint16_t)prefs.getUInt(K_PORT, 443);
  g_settings.ntfyTls   = prefs.getBool(K_TLS, true);
  g_settings.ntfyTopic = prefs.getString(K_TOPIC, "");
  g_settings.ntfyUser  = prefs.getString(K_USER, "");
  g_settings.ntfyPass  = prefs.getString(K_NPASS, "");
  g_settings.ntfyToken = prefs.getString(K_TOKEN, "");
  g_settings.cmdPass   = prefs.getString(K_CMD, "");
  g_settings.adminPass = prefs.getString(K_ADMIN, "");

  prefs.end();

  // A record flagged complete but missing essentials is worse than no record:
  // it would skip the setup portal and then fail forever.
  if (g_settings.wifiSsid.isEmpty() || g_settings.ntfyTopic.isEmpty()) {
    Serial.println("[settings] stored config is incomplete -- forcing setup");
    g_settings.provisioned = false;
    return;
  }

  Serial.printf("[settings] loaded: ssid=%s host=%s:%u tls=%d topic set=%d\n",
                g_settings.wifiSsid.c_str(), g_settings.ntfyHost.c_str(),
                (unsigned)g_settings.ntfyPort, g_settings.ntfyTls ? 1 : 0,
                g_settings.ntfyTopic.isEmpty() ? 0 : 1);
}

bool settings_save() {
  if (!prefs.begin(NS, /*readOnly=*/false)) {
    Serial.println("[settings] could not open NVS for write");
    return false;
  }

  // Cleared first and set last, so a power cut mid-save reads back as
  // unprovisioned rather than as a mix of old and new values.
  prefs.putBool(K_OK, false);
  prefs.putString(K_SSID,  g_settings.wifiSsid);
  prefs.putString(K_PASS,  g_settings.wifiPass);
  prefs.putString(K_HOST,  g_settings.ntfyHost);
  prefs.putUInt(K_PORT,    g_settings.ntfyPort);
  prefs.putBool(K_TLS,     g_settings.ntfyTls);
  prefs.putString(K_TOPIC, g_settings.ntfyTopic);
  prefs.putString(K_USER,  g_settings.ntfyUser);
  prefs.putString(K_NPASS, g_settings.ntfyPass);
  prefs.putString(K_TOKEN, g_settings.ntfyToken);
  prefs.putString(K_CMD,   g_settings.cmdPass);
  prefs.putString(K_ADMIN, g_settings.adminPass);

  bool ok = prefs.putBool(K_OK, true) == 1;
  prefs.end();
  if (!ok) {
    Serial.println("[settings] write failed");
    return false;
  }

  g_settings.provisioned = true;
  Serial.println("[settings] saved");
  return true;
}

void settings_clear() {
  if (!prefs.begin(NS, /*readOnly=*/false)) return;
  prefs.clear();
  prefs.end();
  g_settings = Settings();   // back to compiled defaults
  Serial.println("[settings] cleared");
}
