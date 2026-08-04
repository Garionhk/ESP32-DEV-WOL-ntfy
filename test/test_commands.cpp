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
#include "secrets.h"
#include "store.h"

SerialShim Serial;
FakeWiFi   WiFi;
FakeESP    ESP;
unsigned long fakeMillis = 1000;
std::map<std::string, std::string> Preferences::backing;

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

int main() {
  store::begin();

  printf("-- help\n");
  send("help");
  ok(replyHas("wake <name>") && replyHas("add <pass> <name> <mac>") &&
         replyHas("remove <pass> <name>"),
     "help lists the commands in the documented order");
  send("?");
  ok(replyHas("WOL bot"), "? is an alias for help");
  send("commands");
  ok(replyHas("WOL bot"), "commands is an alias for help");
  send("HELP");
  ok(replyHas("WOL bot"), "verb is case-insensitive");
  send("stat");
  ok(replyHas("ip "), "stat is an alias for status");

  printf("-- unknown command\n");
  send("frobnicate");
  ok(replyHas("Unknown command") && replyHas("help"), "points at help");

  printf("-- empty and whitespace input\n");
  sent.clear(); commands::handle(String("   "));
  ok(sent.empty(), "whitespace-only produces no reply");
  sent.clear(); commands::handle(String(""));
  ok(sent.empty(), "empty produces no reply");

  printf("-- list when empty\n");
  send("list");
  ok(replyHas("No computers saved"), "empty list message");

  printf("-- add requires the passphrase\n");
  send("add wrong-pass desktop aa:bb:cc:dd:ee:ff");
  ok(replyHas("Unauthorized"), "wrong passphrase rejected");
  ok(store::count() == 0, "nothing stored on bad passphrase");

  send("add " COMMAND_PASSPHRASE " desktop aa:bb:cc:dd:ee:ff");
  ok(replyHas("Saved") && replyHas("aa:bb:cc:dd:ee:ff"), "add succeeds");
  ok(store::count() == 1, "stored");

  printf("-- replies never echo the passphrase\n");
  bool leaked = false;
  for (auto& m : sent) if (m.find(COMMAND_PASSPHRASE) != std::string::npos) leaked = true;
  ok(!leaked, "add reply does not contain the passphrase");

  send("add " COMMAND_PASSPHRASE " other 11:22:33:44:55:66");
  leaked = false;
  for (auto& m : sent) if (m.find(COMMAND_PASSPHRASE) != std::string::npos) leaked = true;
  ok(!leaked, "second add reply does not contain the passphrase");

  printf("-- add validation\n");
  send("add");
  ok(replyHas("Usage"), "add with no args shows usage");
  send("add " COMMAND_PASSPHRASE " name-only");
  ok(replyHas("Usage"), "add with too few args shows usage");
  send("add " COMMAND_PASSPHRASE " dupe not-a-mac");
  ok(replyHas("not a valid MAC"), "bad MAC rejected");
  send("add " COMMAND_PASSPHRASE " desktop 99:88:77:66:55:44");
  ok(replyHas("already a computer"), "duplicate rejected");

  printf("-- list with entries\n");
  send("list");
  ok(replyHas("desktop") && replyHas("aa:bb:cc:dd:ee:ff"), "list shows the entry");
  ok(replyHas("other") && replyHas("11:22:33:44:55:66"), "list shows both entries");

  printf("-- wake\n");
  send("wake desktop");
  ok(wakeCalls == 1 && replyHas("Magic packet sent"), "wake by name");
  send("wake DESKTOP");
  ok(wakeCalls == 1, "wake is case-insensitive");
  send("wol desktop");
  ok(wakeCalls == 1, "wol is an alias");
  send("wake nosuchhost");
  ok(wakeCalls == 0 && replyHas("No computer named"), "unknown host");
  send("wake");
  ok(wakeCalls == 0 && replyHas("Usage"), "wake with no target");
  send("wake all");
  ok(wakeCalls == 2 && replyHas("Waking all"), "wake all hits every host");

  printf("-- status\n");
  send("status");
  ok(replyHas("192.168.1.50") && replyHas("test-net") && replyHas("heap"),
     "status reports the essentials");

  printf("-- remove\n");
  send("remove wrong other");
  ok(replyHas("Unauthorized") && store::count() == 2, "wrong passphrase does not remove");
  send("remove " COMMAND_PASSPHRASE " other");
  ok(replyHas("Forgot") && store::count() == 1, "removes with the passphrase");
  send("remove " COMMAND_PASSPHRASE " ghost");
  ok(replyHas("No computer named"), "removing a missing host");
  send("remove");
  ok(replyHas("Usage"), "rm with no args");

  printf("-- lockout after repeated failures\n");
  // A clean slate: the successful rm above reset the failure counter.
  for (int i = 0; i < AUTH_MAX_FAILURES - 1; i++) {
    send("remove nope desktop");
    ok(replyHas("Unauthorized") && !replyHas("locked"), "failure below threshold");
  }
  send("remove nope desktop");
  ok(replyHas("locked"), "threshold trips the lockout");

  send("remove " COMMAND_PASSPHRASE " desktop");
  ok(replyHas("Locked out") && store::count() == 1,
     "correct passphrase is refused while locked out");

  fakeMillis += AUTH_LOCKOUT_MS + 1000;
  send("remove " COMMAND_PASSPHRASE " desktop");
  ok(replyHas("Forgot") && store::count() == 0, "works again once the lockout expires");

  printf("-- remaining aliases for add and remove\n");
  send("save " COMMAND_PASSPHRASE " viasave aa:bb:cc:dd:ee:02");
  ok(replyHas("Saved"), "save is an alias for add");
  send("delete " COMMAND_PASSPHRASE " viasave");
  ok(replyHas("Forgot"), "delete is an alias for remove");
  send("save " COMMAND_PASSPHRASE " viaforget aa:bb:cc:dd:ee:03");
  send("forget " COMMAND_PASSPHRASE " viaforget");
  ok(replyHas("Forgot"), "forget is an alias for remove");
  send("save " COMMAND_PASSPHRASE " viarm aa:bb:cc:dd:ee:04");
  send("rm " COMMAND_PASSPHRASE " viarm");
  ok(replyHas("Forgot"), "rm is an alias for remove");

  printf("-- argument order is actually pass-first\n");
  // The old order (name first) must now fail: "desktop" is not the passphrase.
  send("save desktop " COMMAND_PASSPHRASE " aa:bb:cc:dd:ee:05");
  ok(replyHas("Unauthorized"), "old add order is rejected");

  printf("-- extra whitespace is tolerated\n");
  send("  add   " COMMAND_PASSPHRASE "   spaced   aa:bb:cc:dd:ee:01  ");
  ok(replyHas("Saved"), "leading/inner/trailing whitespace");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
