#include <Arduino.h>
#include <WiFi.h>

#include "commands.h"
#include "config.h"
#include "ntfy.h"
#include "settings.h"
#include "store.h"
#include "webconfig.h"
#include "wol.h"

namespace commands {
namespace {

const int MAX_TOKENS = 6;

int tokenize(const String& input, String out[MAX_TOKENS]) {
  int    n = 0;
  String current;

  for (unsigned int i = 0; i <= input.length() && n < MAX_TOKENS; i++) {
    char c = (i < input.length()) ? input[i] : ' ';
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (current.length() > 0) {
        out[n++] = current;
        current  = "";
      }
    } else {
      current += c;
    }
  }
  return n;
}

// Compares without an early exit, so response time does not leak how many
// leading characters were right. Case-insensitive, so phone
// auto-capitalisation doesn't lock you out.
bool passphraseOk(const String& given, const String& expected) {
  String a = given, b = expected;
  a.toLowerCase();
  b.toLowerCase();

  uint32_t diff = (uint32_t)(a.length() ^ b.length());
  for (unsigned int i = 0; i < b.length(); i++) {
    char g = (i < a.length()) ? a[i] : '\0';
    diff |= (uint32_t)(g ^ b[i]);
  }
  return diff == 0;
}

// add / remove / factoryreset ALWAYS need a passphrase to be set, even though
// the gate in handle() lets everything through when none is -- ntfy.sh topics
// are public, so an ungated topic stays read-only.
bool requireSecret(const String& secret, const char* what, const char* title) {
  if (!secret.isEmpty()) return true;
  ntfy::publish(String("Set a command passphrase before ") + what + ".", title);
  return false;
}

String uptimeStr() {
  uint32_t s = millis() / 1000;
  uint32_t d = s / 86400;
  uint32_t h = (s % 86400) / 3600;
  uint32_t m = (s % 3600) / 60;

  String out;
  if (d > 0) out += String(d) + "d ";
  if (d > 0 || h > 0) out += String(h) + "h ";
  out += String(m) + "m";
  return out;
}

void replyHelp() {
  String help =
      "WOL bot -- commands\n"
      "\n"
      "Start every command with your passphrase:\n"
      "<passphrase> <command>\n"
      "\n"
      "list               show saved computers\n"
      "wake <name>        send magic packet (\"wake all\" for every host)\n"
      "add <name> <mac>   remember a computer\n"
      "remove <name>      forget a computer\n"
      "status             uptime, IP, wifi, free memory\n"
      "factoryreset       wipe settings, reboot into setup\n"
      "                   (\"factoryreset all\" also forgets computers)\n"
      "help               this message\n"
      "\n"
      "MAC accepts aa:bb:cc:dd:ee:ff, aa-bb-cc-dd-ee-ff, or aabbccddeeff.\n"
      "Names are case-insensitive, up to " + String(MAX_NAME_LEN) +
      " chars of a-z 0-9 _ -";
  ntfy::publish(help, "Help");
}

void replyList() {
  if (store::count() == 0) {
    ntfy::publish("No computers saved yet. Add one with:\n"
                  "<passphrase> add <name> <mac>",
                  "Saved computers");
    return;
  }

  String out = "Saved computers (" + String(store::count()) + "/" +
               String(MAX_HOSTS) + ")\n";
  store::forEach([&out](const String& name, const uint8_t* mac) {
    out += "\n" + name + "  " + store::formatMac(mac);
  });
  ntfy::publish(out, "Saved computers");
}

void replyStatus() {
  String out =
      "up " + uptimeStr() + "\n" +
      "ip " + WiFi.localIP().toString() + "\n" +
      "settings " + webconfig::settingsUrl() + "\n" +
      "wifi " + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)\n" +
      "ntfy " + String(ntfy::connected() ? "connected" : "reconnecting") + "\n" +
      "hosts " + String(store::count()) + "/" + String(MAX_HOSTS) + "\n" +
      "heap " + String(ESP.getFreeHeap() / 1024) + " KB free";
  ntfy::publish(out, "Status");
}

void doWake(const String& target) {
  if (target.equalsIgnoreCase("all")) {
    if (store::count() == 0) {
      ntfy::publish("Nothing to wake -- no computers saved.", "Wake");
      return;
    }
    String out = "Waking all " + String(store::count()) + ":";
    store::forEach([&out](const String& name, const uint8_t* mac) {
      bool ok = wol::wake(mac);
      out += "\n" + String(ok ? "sent " : "failed ") + name;
    });
    ntfy::publish(out, "Wake");
    return;
  }

  uint8_t mac[6];
  if (!store::find(target, mac)) {
    ntfy::publish("No computer named \"" + target + "\". Send \"list\" to see saved ones.",
                  "Wake");
    return;
  }

  bool ok = wol::wake(mac);
  ntfy::publish(ok ? "Magic packet sent to " + target + " (" + store::formatMac(mac) + ")"
                   : "Could not send the magic packet to " + target + ".",
                "Wake");
}

// add <name> <mac>
void doAdd(const String& secret, String tokens[MAX_TOKENS], int count) {
  if (!requireSecret(secret, "adding or removing computers", "Add")) return;
  if (count < 3) {
    ntfy::publish("Usage: <passphrase> add <name> <mac>", "Add");
    return;
  }

  uint8_t mac[6];
  if (!store::parseMac(tokens[2], mac)) {
    ntfy::publish("\"" + tokens[2] + "\" is not a valid MAC address.", "Add");
    return;
  }

  switch (store::add(tokens[1], mac)) {
    case store::AddResult::Ok:
      ntfy::publish("Saved " + store::normalizeName(tokens[1]) + " -> " +
                        store::formatMac(mac),
                    "Add");
      break;
    case store::AddResult::Duplicate:
      ntfy::publish("There is already a computer named \"" + tokens[1] +
                        "\". Remove it first.",
                    "Add");
      break;
    case store::AddResult::Full:
      ntfy::publish("Storage is full (" + String(MAX_HOSTS) + " computers).", "Add");
      break;
    case store::AddResult::BadName:
      ntfy::publish("Bad name. Use up to " + String(MAX_NAME_LEN) +
                        " characters of a-z, 0-9, _ or -.",
                    "Add");
      break;
  }
}

// remove <name>
void doRemove(const String& secret, String tokens[MAX_TOKENS], int count) {
  if (!requireSecret(secret, "adding or removing computers", "Remove")) return;
  if (count < 2) {
    ntfy::publish("Usage: <passphrase> remove <name>", "Remove");
    return;
  }

  if (store::remove(tokens[1])) {
    ntfy::publish("Forgot " + store::normalizeName(tokens[1]) + ".", "Remove");
  } else {
    ntfy::publish("No computer named \"" + tokens[1] + "\".", "Remove");
  }
}

// factoryreset [all]
void doFactoryReset(const String& secret, String tokens[MAX_TOKENS], int count) {
  if (!requireSecret(secret, "using factoryreset", "Factory reset")) return;

  bool wipeHosts = (count >= 2 && tokens[1].equalsIgnoreCase("all"));

  ntfy::publish(String("Clearing settings") +
                    (wipeHosts ? " and saved computers" : "") +
                    ". Rebooting into setup mode -- join the \"" +
                    webconfig::apSsid() + "\" network to reconfigure.",
                "Factory reset");

  if (wipeHosts) {
    while (store::count() > 0) {
      bool removedAny = false;
      store::forEach([&](const String& name, const uint8_t*) {
        if (!removedAny) { store::remove(name); removedAny = true; }
      });
      if (!removedAny) break;
    }
  }

  settings_clear();
  delay(1500);          // let the reply actually reach the server
  ESP.restart();
}

}  // namespace

void handle(const String& message) {
  String tokens[MAX_TOKENS];
  int    n = tokenize(message, tokens);
  if (n == 0) return;

  // --- passphrase gate --------------------------------------------------
  // The passphrase prefixes every command. Anything else is ignored without a
  // reply, so a stranger on the topic gets no hint the device is listening.
  String secret = g_settings.cmdPass;
  secret.trim();
  if (!secret.isEmpty()) {
    if (!passphraseOk(tokens[0], secret)) return;
    for (int i = 1; i < n; i++) tokens[i - 1] = tokens[i];
    tokens[--n] = "";
    if (n == 0) return;
  }

  String verb = tokens[0];
  verb.toLowerCase();

  if (verb == "help" || verb == "?" || verb == "commands") {
    replyHelp();

  } else if (verb == "list" || verb == "ls") {
    replyList();

  } else if (verb == "status" || verb == "stat") {
    replyStatus();

  } else if (verb == "wake" || verb == "wol") {
    if (n < 2) {
      ntfy::publish("Usage: <passphrase> wake <name>   (or \"... wake all\")", "Wake");
      return;
    }
    doWake(tokens[1]);

  } else if (verb == "add" || verb == "save") {
    doAdd(secret, tokens, n);

  } else if (verb == "rm" || verb == "del" || verb == "delete" ||
             verb == "remove" || verb == "forget") {
    doRemove(secret, tokens, n);

  } else if (verb == "factoryreset") {
    doFactoryReset(secret, tokens, n);

  } else {
    ntfy::publish("Unknown command \"" + tokens[0] + "\". Send \"<passphrase> help\" "
                  "for the list.",
                  "Unknown command");
  }
}

}  // namespace commands
