#pragma once

#include <Arduino.h>

// Runtime configuration, held in NVS and edited from the web portal.
//
// This replaces the old compile-time secrets.h. Nothing user-specific is baked
// into the binary any more, so a build can be shared without leaking a WiFi
// password.

struct Settings {
  // WiFi
  String   wifiSsid;
  String   wifiPass;

  // ntfy server
  String   ntfyHost = "ntfy.sh";
  uint16_t ntfyPort = 443;
  bool     ntfyTls  = true;
  String   ntfyTopic;

  // ntfy login -- token wins over user/password; all blank means an open server
  String   ntfyUser;
  String   ntfyPass;
  String   ntfyToken;

  // Command passphrase: prefixes every command sent over ntfy. REQUIRED for
  // add / remove / factoryreset.
  String   cmdPass;

  // Login for the settings page. Deliberately separate from cmdPass: that one
  // travels through the ntfy topic, and it should not also unlock the config.
  String   adminPass;

  bool     provisioned = false;   // the "ok" sentinel
};

extern Settings g_settings;

// Loads from NVS into g_settings. Leaves the defaults above in place if the
// device has never been provisioned.
void settings_begin();

bool settings_save();

// Wipes the config namespace only. Saved computers live in a different
// namespace and are untouched.
void settings_clear();

// Username paired with Settings::adminPass on the settings page.
static const char* const ADMIN_USER = "admin";
