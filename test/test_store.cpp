// Host-side tests for the real store.cpp (compiled in, not copied).
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Preferences.h"
#include "config.h"
#include "store.h"

SerialShim Serial;
std::map<std::string, std::string> Preferences::backing;

static int failures = 0;
static int checks = 0;

static void ok(bool cond, const char* what) {
  checks++;
  if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static String mac2str(const uint8_t m[6]) { return store::formatMac(m); }

int main() {
  uint8_t mac[6];

  printf("-- parseMac accepts the formats people type\n");
  ok(store::parseMac("aa:bb:cc:dd:ee:ff", mac) && mac2str(mac) == String("aa:bb:cc:dd:ee:ff"), "colon form");
  ok(store::parseMac("AA-BB-CC-DD-EE-FF", mac) && mac2str(mac) == String("aa:bb:cc:dd:ee:ff"), "dash form, uppercase");
  ok(store::parseMac("aabbccddeeff", mac) && mac2str(mac) == String("aa:bb:cc:dd:ee:ff"), "bare hex");
  ok(store::parseMac("00:11:22:33:44:55", mac) && mac[0] == 0x00 && mac[5] == 0x55, "byte values");

  printf("-- parseMac rejects malformed input\n");
  ok(!store::parseMac("", mac), "empty");
  ok(!store::parseMac("aa:bb:cc:dd:ee", mac), "too short");
  ok(!store::parseMac("aa:bb:cc:dd:ee:ff:00", mac), "too long");
  ok(!store::parseMac("gg:bb:cc:dd:ee:ff", mac), "non-hex digit");
  ok(!store::parseMac("aa:bb:cc:dd:ee:fz", mac), "trailing non-hex");

  printf("-- normalizeName\n");
  ok(store::normalizeName("Desktop") == String("desktop"), "lowercased");
  ok(store::normalizeName("  nas  ") == String("nas"), "trimmed");
  ok(store::normalizeName("my-pc_2") == String("my-pc_2"), "dash and underscore allowed");
  ok(store::normalizeName("").isEmpty(), "empty rejected");
  ok(store::normalizeName("has space").isEmpty(), "space rejected");
  ok(store::normalizeName("bad!").isEmpty(), "punctuation rejected");
  ok(store::normalizeName("123456789012345678901").isEmpty(), "21 chars rejected");
  ok(store::normalizeName("12345678901234567890") == String("12345678901234567890"), "20 chars ok");

  printf("-- add / find / remove\n");
  store::begin();
  ok(store::count() == 0, "starts empty");

  store::parseMac("aa:bb:cc:dd:ee:ff", mac);
  ok(store::add("Desktop", mac) == store::AddResult::Ok, "add ok");
  ok(store::count() == 1, "count is 1");
  ok(store::add("desktop", mac) == store::AddResult::Duplicate, "duplicate rejected case-insensitively");
  ok(store::add("bad name!", mac) == store::AddResult::BadName, "bad name rejected");

  uint8_t got[6];
  ok(store::find("DESKTOP", got) && mac2str(got) == String("aa:bb:cc:dd:ee:ff"), "find is case-insensitive");
  ok(!store::find("nope", got), "missing host not found");

  printf("-- capacity\n");
  for (int i = 1; i < MAX_HOSTS; i++) {
    char n[16]; snprintf(n, sizeof n, "host%d", i);
    ok(store::add(n, mac) == store::AddResult::Ok, "fill to capacity");
  }
  ok(store::count() == MAX_HOSTS, "at MAX_HOSTS");
  ok(store::add("overflow", mac) == store::AddResult::Full, "rejects past MAX_HOSTS");

  printf("-- persistence across a simulated reboot\n");
  store::begin();   // re-reads the backing store
  ok(store::count() == MAX_HOSTS, "list survived reboot");
  ok(store::find("desktop", got) && mac2str(got) == String("aa:bb:cc:dd:ee:ff"), "entry intact after reboot");

  printf("-- remove\n");
  ok(store::remove("desktop"), "remove existing");
  ok(!store::find("desktop", got), "gone after remove");
  ok(store::count() == MAX_HOSTS - 1, "count decremented");
  ok(!store::remove("desktop"), "removing twice fails");
  ok(store::find("host1", got), "neighbour survived the shift");
  ok(store::find("host15", got), "last entry survived the shift");

  store::begin();
  ok(store::count() == MAX_HOSTS - 1, "removal persisted");

  printf("-- corrupt blob does not wedge the device\n");
  Preferences::backing["hosts"] = "{not json at all";
  store::begin();
  ok(store::count() == 0, "recovers to empty list");
  store::parseMac("11:22:33:44:55:66", mac);
  ok(store::add("after", mac) == store::AddResult::Ok, "still usable after corruption");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
