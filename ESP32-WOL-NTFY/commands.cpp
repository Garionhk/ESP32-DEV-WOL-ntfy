#include <Arduino.h>
#include <WiFi.h>

#include "commands.h"
#include "config.h"
#include "ntfy.h"
#include "secrets.h"
#include "store.h"
#include "wol.h"

namespace commands {
namespace {

const int MAX_TOKENS = 6;

uint8_t  authFailures  = 0;
uint32_t lockoutUntil  = 0;

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
// leading characters were right.
bool passphraseOk(const String& given) {
  const char* expected = COMMAND_PASSPHRASE;
  size_t expectedLen   = strlen(expected);

  uint32_t diff = (uint32_t)(given.length() ^ expectedLen);
  for (size_t i = 0; i < expectedLen; i++) {
    char g = (i < given.length()) ? given[i] : '\0';
    diff |= (uint32_t)(g ^ expected[i]);
  }
  return diff == 0;
}

// Gate for add/rm. Handles the lockout counter and replies on failure, so
// callers only need to check the return value.
bool authorize(const String& given) {
  if (lockoutUntil != 0 && (int32_t)(millis() - lockoutUntil) < 0) {
    uint32_t remaining = (lockoutUntil - millis() + 999) / 1000;
    ntfy::publish("Locked out. Try again in " + String(remaining) + "s.",
                  "Unauthorized");
    return false;
  }

  if (passphraseOk(given)) {
    authFailures = 0;
    lockoutUntil = 0;
    return true;
  }

  authFailures++;
  if (authFailures >= AUTH_MAX_FAILURES) {
    lockoutUntil = millis() + AUTH_LOCKOUT_MS;
    authFailures = 0;
    ntfy::publish("Unauthorized. Too many attempts -- locked for " +
                      String(AUTH_LOCKOUT_MS / 1000) + "s.",
                  "Unauthorized");
  } else {
    ntfy::publish("Unauthorized.", "Unauthorized");
  }
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
      "list                     show saved computers\n"
      "wake <name>              send magic packet (\"wake all\" for every host)\n"
      "add <pass> <name> <mac>  remember a computer\n"
      "remove <pass> <name>     forget a computer\n"
      "status                   uptime, IP, wifi, free memory\n"
      "help                     this message\n"
      "\n"
      "MAC accepts aa:bb:cc:dd:ee:ff, aa-bb-cc-dd-ee-ff, or aabbccddeeff.\n"
      "Names are case-insensitive, up to " + String(MAX_NAME_LEN) +
      " chars of a-z 0-9 _ -";
  ntfy::publish(help, "Help");
}

void replyList() {
  if (store::count() == 0) {
    ntfy::publish("No computers saved yet. Add one with:\n"
                  "add <passphrase> <name> <mac>",
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

// add <pass> <name> <mac>
void doAdd(String tokens[MAX_TOKENS], int count) {
  if (count < 4) {
    ntfy::publish("Usage: add <passphrase> <name> <mac>", "Add");
    return;
  }
  if (!authorize(tokens[1])) return;

  uint8_t mac[6];
  if (!store::parseMac(tokens[3], mac)) {
    ntfy::publish("\"" + tokens[3] + "\" is not a valid MAC address.", "Add");
    return;
  }

  switch (store::add(tokens[2], mac)) {
    case store::AddResult::Ok:
      ntfy::publish("Saved " + store::normalizeName(tokens[2]) + " -> " +
                        store::formatMac(mac),
                    "Add");
      break;
    case store::AddResult::Duplicate:
      ntfy::publish("There is already a computer named \"" + tokens[2] +
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

// remove <pass> <name>
void doRemove(String tokens[MAX_TOKENS], int count) {
  if (count < 3) {
    ntfy::publish("Usage: remove <passphrase> <name>", "Remove");
    return;
  }
  if (!authorize(tokens[1])) return;

  if (store::remove(tokens[2])) {
    ntfy::publish("Forgot " + store::normalizeName(tokens[2]) + ".", "Remove");
  } else {
    ntfy::publish("No computer named \"" + tokens[2] + "\".", "Remove");
  }
}

}  // namespace

void handle(const String& message) {
  String tokens[MAX_TOKENS];
  int    n = tokenize(message, tokens);
  if (n == 0) return;

  String verb = tokens[0];
  verb.toLowerCase();

  if (verb == "help" || verb == "?" || verb == "commands") {
    replyHelp();

  } else if (verb == "list" || verb == "ls") {
    replyList();

  } else if (verb == "status" || verb == "stat") {
    replyStatus();

  } else if (verb == "wake" || verb == "wol") {
#if REQUIRE_PASS_FOR_WAKE
    // Passphrase first, matching add/remove.
    if (n < 3) {
      ntfy::publish("Usage: wake <passphrase> <name>   (or \"... all\")", "Wake");
      return;
    }
    if (!authorize(tokens[1])) return;
    doWake(tokens[2]);
#else
    if (n < 2) {
      ntfy::publish("Usage: wake <name>   (or \"wake all\")", "Wake");
      return;
    }
    doWake(tokens[1]);
#endif

  } else if (verb == "add" || verb == "save") {
    doAdd(tokens, n);

  } else if (verb == "rm" || verb == "del" || verb == "delete" ||
             verb == "remove" || verb == "forget") {
    doRemove(tokens, n);

  } else {
    ntfy::publish("Unknown command \"" + tokens[0] + "\". Send \"help\" for the list.",
                  "Unknown command");
  }
}

}  // namespace commands
