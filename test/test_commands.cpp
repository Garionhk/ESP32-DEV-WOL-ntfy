// Host-side tests for the real commands.cpp. ntfy and wol are stubbed so the
// replies can be inspected; store.cpp is the genuine article.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Preferences.h"
#include "WiFi.h"
#include "commands.h"
#include "config.h"
#include "settings.h"
#include "store.h"
#include "webconfig.h"

// Configuration is runtime now, so the harness sets it directly instead of
// pulling in a compile-time secrets header.
#define COMMAND_PASSPHRASE "test-passphrase"

SerialShim Serial;
FakeWiFi   WiFi;
FakeESP    ESP;
unsigned long fakeMillis = 1000;
bool g_restarted = false;
std::map<std::string, std::string> Preferences::backing;

// The portal is not under test here; commands.cpp only asks it for display
// strings.
namespace webconfig {
String settingsUrl() { return String("http://192.168.1.50"); }
String apSsid()      { return String("WOL-Setup-TEST"); }
}  // namespace webconfig

// ---- stubs standing in for the transport and the radio ----
static std::vector<std::string> sent;
static int wakeCalls = 0;

namespace ntfy {
bool publish(const String& message, const String& title) {
  sent.push_back(std::string(title.s) + "|" + message.s);
  return true;
}
bool connected() { return true; }
}  // namespace ntfy

namespace wol {
bool wake(const uint8_t mac[6]) { (void)mac; wakeCalls++; return true; }
}  // namespace wol

// ---- harness ----
static int failures = 0, checks = 0;

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static std::string lastReply() { return sent.empty() ? "" : sent.back(); }

static bool replyHas(const char* needle) {
  return lastReply().find(needle) != std::string::npos;
}

static void send(const char* msg) {
  sent.clear();
  wakeCalls = 0;
  commands::handle(String(msg));
}

#define P COMMAND_PASSPHRASE " "

static bool anyReplyLeaks() {
  for (auto& m : sent) if (m.find(COMMAND_PASSPHRASE) != std::string::npos) return true;
  return false;
}

int main() {
  store::begin();
  g_settings.cmdPass     = COMMAND_PASSPHRASE;
  g_settings.provisioned = true;

  printf("-- the passphrase gates every command\n");
  const char* ungated[] = {"help", "list", "status", "wake desktop",
                           "add desktop aa:bb:cc:dd:ee:ff", "remove desktop",
                           "factoryreset", "frobnicate"};
  for (const char* m : ungated) {
    g_restarted = false;
    send(m);
    ok(sent.empty() && wakeCalls == 0 && !g_restarted,
       "a command without the passphrase gets no reply at all");
  }
  send("wrong-pass help");
  ok(sent.empty(), "a wrong passphrase gets no reply");
  send("wrong-pass add desktop aa:bb:cc:dd:ee:ff");
  ok(sent.empty() && store::count() == 0, "a wrong passphrase stores nothing");
  send(COMMAND_PASSPHRASE "x help");
  ok(sent.empty(), "a passphrase with a suffix does not match");
  send("test-pass help");
  ok(sent.empty(), "a prefix of the passphrase does not match");
  send(COMMAND_PASSPHRASE);
  ok(sent.empty(), "the passphrase alone produces no reply");
  send("help " COMMAND_PASSPHRASE);
  ok(sent.empty(), "the passphrase must come first");

  printf("-- passphrase is case-insensitive\n");
  send("TEST-Passphrase help");
  ok(replyHas("WOL bot"), "auto-capitalised passphrase still matches");

  printf("-- help\n");
  send(P "help");
  ok(replyHas("<passphrase> <command>") && replyHas("wake <name>") &&
         replyHas("add <name> <mac>") && replyHas("remove <name>"),
     "help lists the commands behind the passphrase");
  send(P "?");
  ok(replyHas("WOL bot"), "? is an alias for help");
  send(P "commands");
  ok(replyHas("WOL bot"), "commands is an alias for help");
  send(P "HELP");
  ok(replyHas("WOL bot"), "verb is case-insensitive");
  send(P "stat");
  ok(replyHas("ip "), "stat is an alias for status");

  printf("-- unknown command\n");
  send(P "frobnicate");
  ok(replyHas("Unknown command") && replyHas("help"), "points at help");

  printf("-- empty and whitespace input\n");
  send("   ");
  ok(sent.empty(), "whitespace-only produces no reply");
  send("");
  ok(sent.empty(), "empty produces no reply");

  printf("-- list when empty\n");
  send(P "list");
  ok(replyHas("No computers saved") && replyHas("<passphrase> add"),
     "empty list message shows the new syntax");

  printf("-- add\n");
  send(P "add desktop aa:bb:cc:dd:ee:ff");
  ok(replyHas("Saved") && replyHas("aa:bb:cc:dd:ee:ff"), "add succeeds");
  ok(store::count() == 1, "stored");
  ok(!anyReplyLeaks(), "add reply does not contain the passphrase");

  send(P "add other 11:22:33:44:55:66");
  ok(!anyReplyLeaks(), "second add reply does not contain the passphrase");

  printf("-- add validation\n");
  send(P "add");
  ok(replyHas("Usage"), "add with no args shows usage");
  send(P "add name-only");
  ok(replyHas("Usage"), "add with too few args shows usage");
  send(P "add dupe not-a-mac");
  ok(replyHas("not a valid MAC"), "bad MAC rejected");
  send(P "add desktop 99:88:77:66:55:44");
  ok(replyHas("already a computer"), "duplicate rejected");

  printf("-- list with entries\n");
  send(P "list");
  ok(replyHas("desktop") && replyHas("aa:bb:cc:dd:ee:ff"), "list shows the entry");
  ok(replyHas("other") && replyHas("11:22:33:44:55:66"), "list shows both entries");

  printf("-- wake\n");
  send(P "wake desktop");
  ok(wakeCalls == 1 && replyHas("Magic packet sent"), "wake by name");
  send(P "wake DESKTOP");
  ok(wakeCalls == 1, "wake is case-insensitive");
  send(P "wol desktop");
  ok(wakeCalls == 1, "wol is an alias");
  send(P "wake nosuchhost");
  ok(wakeCalls == 0 && replyHas("No computer named"), "unknown host");
  send(P "wake");
  ok(wakeCalls == 0 && replyHas("Usage"), "wake with no target");
  send(P "wake all");
  ok(wakeCalls == 2 && replyHas("Waking all"), "wake all hits every host");

  printf("-- status\n");
  send(P "status");
  ok(replyHas("192.168.1.50") && replyHas("test-net") && replyHas("heap"),
     "status reports the essentials");

  printf("-- remove\n");
  send(P "remove other");
  ok(replyHas("Forgot") && store::count() == 1, "removes with the passphrase");
  send(P "remove ghost");
  ok(replyHas("No computer named"), "removing a missing host");
  send(P "remove");
  ok(replyHas("Usage"), "rm with no args");

  printf("-- remaining aliases for add and remove\n");
  send(P "save viasave aa:bb:cc:dd:ee:02");
  ok(replyHas("Saved"), "save is an alias for add");
  send(P "delete viasave");
  ok(replyHas("Forgot"), "delete is an alias for remove");
  send(P "save viaforget aa:bb:cc:dd:ee:03");
  send(P "forget viaforget");
  ok(replyHas("Forgot"), "forget is an alias for remove");
  send(P "save viarm aa:bb:cc:dd:ee:04");
  send(P "rm viarm");
  ok(replyHas("Forgot"), "rm is an alias for remove");
  send(P "save viadel aa:bb:cc:dd:ee:06");
  send(P "del viadel");
  ok(replyHas("Forgot"), "del is an alias for remove");

  printf("-- extra whitespace is tolerated\n");
  send("  " COMMAND_PASSPHRASE "   add   spaced   aa:bb:cc:dd:ee:01  ");
  ok(replyHas("Saved"), "leading/inner/trailing whitespace");

  printf("-- factoryreset\n");
  int before = store::count();
  g_restarted = false;
  send(P "factoryreset");
  ok(g_restarted, "passphrase + factoryreset reboots");
  ok(store::count() == before, "saved computers kept without \"all\"");
  ok(!anyReplyLeaks(), "factoryreset reply does not contain the passphrase");

  g_settings.cmdPass = COMMAND_PASSPHRASE;   // factoryreset cleared it
  g_restarted = false;
  send(P "factoryreset all");
  ok(g_restarted && store::count() == 0, "\"all\" also forgets saved computers");

  printf("-- with no passphrase set, the topic stays read-only\n");
  g_settings = Settings();
  g_settings.provisioned = true;
  const uint8_t pcMac[6] = {1, 2, 3, 4, 5, 6};
  store::add(String("pc"), pcMac);
  send("list");
  ok(replyHas("pc"), "queries answer without a prefix");
  send("wake pc");
  ok(wakeCalls == 1, "wake works without a prefix");
  send("add newpc aa:bb:cc:dd:ee:09");
  ok(replyHas("Set a command passphrase") && store::count() == 1,
     "add refused while no passphrase is set");
  send("remove pc");
  ok(replyHas("Set a command passphrase") && store::count() == 1,
     "remove refused while no passphrase is set");
  g_restarted = false;
  send("factoryreset");
  ok(replyHas("Set a command passphrase") && !g_restarted,
     "factoryreset refused while no passphrase is set");
  g_settings.cmdPass = "   ";
  send("add newpc aa:bb:cc:dd:ee:09");
  ok(replyHas("Set a command passphrase"), "a whitespace-only passphrase counts as unset");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
