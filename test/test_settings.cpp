// Host-side tests for the real settings.cpp -- the NVS-backed runtime config
// that replaced secrets.h.
#include <cstdio>
#include <map>
#include <string>

#include "Arduino.h"
#include "Preferences.h"
#include "WiFi.h"
#include "settings.h"

SerialShim Serial;
FakeWiFi   WiFi;
FakeESP    ESP;
unsigned long fakeMillis = 1000;
bool g_restarted = false;
std::map<std::string, std::string> Preferences::backing;

static int failures = 0, checks = 0;
static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

int main() {
  printf("-- a fresh device is unprovisioned\n");
  settings_begin();
  ok(!g_settings.provisioned, "not provisioned before any save");
  ok(g_settings.ntfyHost == String("ntfy.sh"), "default host");
  ok(g_settings.ntfyPort == 443, "default port");
  ok(g_settings.ntfyTls, "TLS on by default");

  printf("-- save and reload across a simulated reboot\n");
  g_settings.wifiSsid  = "my-network";
  g_settings.wifiPass  = "wifi-secret";
  g_settings.ntfyHost  = "ntfy.example.com";
  g_settings.ntfyPort  = 8443;
  g_settings.ntfyTls   = true;
  g_settings.ntfyTopic = "topic-abc";
  g_settings.ntfyUser  = "esp32";
  g_settings.ntfyPass  = "ntfy-secret";
  g_settings.cmdPass   = "cmd-secret";
  g_settings.adminPass = "admin-secret";
  ok(settings_save(), "save succeeds");
  ok(g_settings.provisioned, "provisioned after save");

  g_settings = Settings();              // wipe RAM, keep NVS
  settings_begin();
  ok(g_settings.provisioned, "provisioned survives reboot");
  ok(g_settings.wifiSsid  == String("my-network"), "ssid round-trips");
  ok(g_settings.wifiPass  == String("wifi-secret"), "wifi password round-trips");
  ok(g_settings.ntfyHost  == String("ntfy.example.com"), "host round-trips");
  ok(g_settings.ntfyPort  == 8443, "non-default port round-trips");
  ok(g_settings.ntfyTopic == String("topic-abc"), "topic round-trips");
  ok(g_settings.ntfyUser  == String("esp32"), "ntfy user round-trips");
  ok(g_settings.cmdPass   == String("cmd-secret"), "command passphrase round-trips");
  ok(g_settings.adminPass == String("admin-secret"), "admin password round-trips");

  printf("-- plain-HTTP mode round-trips (bool false is not the default)\n");
  g_settings.ntfyTls = false;
  g_settings.ntfyPort = 80;
  settings_save();
  g_settings = Settings();
  settings_begin();
  ok(!g_settings.ntfyTls, "TLS off survives -- would regress if the default won");
  ok(g_settings.ntfyPort == 80, "port 80 survives");

  printf("-- an incomplete record forces setup rather than booting broken\n");
  g_settings.ntfyTopic = "";
  settings_save();
  g_settings = Settings();
  settings_begin();
  ok(!g_settings.provisioned, "missing topic re-triggers the setup portal");

  printf("-- clear\n");
  g_settings.wifiSsid = "x"; g_settings.ntfyTopic = "y";
  g_settings.cmdPass = "z"; g_settings.adminPass = "w";
  settings_save();
  settings_clear();
  ok(!g_settings.provisioned, "cleared in RAM");
  g_settings = Settings();
  settings_begin();
  ok(!g_settings.provisioned, "cleared in NVS");
  ok(g_settings.wifiSsid.isEmpty(), "credentials gone after clear");

  printf("-- clearing config leaves another namespace alone\n");
  Preferences other;
  other.begin("wolntfy", false);
  other.putString("hosts", String("[{\"n\":\"pc\",\"m\":\"aabbccddeeff\"}]"));
  other.end();
  g_settings.wifiSsid = "a"; g_settings.ntfyTopic = "b";
  g_settings.cmdPass = "c"; g_settings.adminPass = "d";
  settings_save();
  settings_clear();
  Preferences check;
  check.begin("wolntfy", false);
  ok(!check.getString("hosts", String("")).isEmpty(),
     "saved computers survive a settings reset");
  check.end();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
