#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <base64.h>

#include "config.h"
#include "ntfy.h"
#include "secrets.h"

#if NTFY_USE_TLS
#include <WiFiClientSecure.h>
#include "certs.h"
// TLS and plain HTTP need different client classes, and the choice is fixed at
// compile time by secrets.h -- so alias it rather than pay for virtual dispatch
// and a second code path at runtime.
typedef WiFiClientSecure NtfyClient;
#define NTFY_SCHEME "https://"
#else
typedef WiFiClient NtfyClient;
#define NTFY_SCHEME "http://"
#endif

namespace ntfy {
namespace {

NtfyClient     stream;
MessageHandler onMessage;

// "Bearer tk_..." or "Basic base64(user:password)", or empty for an open
// server. Built once -- the inputs are compile-time constants.
const String& authHeader() {
  static String cached;
  static bool   built = false;
  if (!built) {
    if (strlen(NTFY_TOKEN) > 0) {
      cached = "Bearer " + String(NTFY_TOKEN);
    } else if (strlen(NTFY_USER) > 0) {
      // This core's libb64 does not wrap lines, so the result is header-safe.
      cached = "Basic " +
               base64::encode(String(NTFY_USER) + ":" + String(NTFY_PASSWORD));
    }
    built = true;
  }
  return cached;
}

enum class State { Disconnected, Streaming };
State state = State::Disconnected;

String   lineBuf;
uint32_t lastDataMs      = 0;
uint32_t nextAttemptMs   = 0;
uint32_t backoffMs       = NTFY_RECONNECT_MIN_MS;

// Last message ID seen, used as ?since= on reconnect so the gap while we were
// away gets replayed. Deliberately empty on cold boot -- see connect().
String lastId;

// Ring of recently handled IDs. A reconnect can legitimately redeliver a
// message we already acted on; without this, a wake or an add could run twice.
String   seenIds[NTFY_DEDUP_SLOTS];
uint8_t  seenNext = 0;

bool alreadySeen(const String& id) {
  if (id.isEmpty()) return false;
  for (uint8_t i = 0; i < NTFY_DEDUP_SLOTS; i++) {
    if (seenIds[i] == id) return true;
  }
  seenIds[seenNext] = id;
  seenNext = (seenNext + 1) % NTFY_DEDUP_SLOTS;
  return false;
}

void applyTlsConfig(NtfyClient& client) {
#if NTFY_USE_TLS
#if defined(NTFY_TLS_INSECURE)
  client.setInsecure();
#elif defined(NTFY_CUSTOM_CA)
  client.setCACert(NTFY_CUSTOM_CA);   // private or self-signed CA
#else
  client.setCACert(NTFY_ROOT_CAS);    // Let's Encrypt / ISRG roots
#endif
#else
  (void)client;                       // plain HTTP: nothing to configure
#endif
}

void disconnect(const char* why) {
  if (state != State::Disconnected || stream.connected()) {
    Serial.printf("[ntfy] disconnecting: %s\n", why);
  }
  stream.stop();
  state = State::Disconnected;
  lineBuf = "";

  nextAttemptMs = millis() + backoffMs;
  backoffMs = min(backoffMs * 2, (uint32_t)NTFY_RECONNECT_MAX_MS);
}

bool connect() {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.printf("[ntfy] connecting to %s (heap %u)\n", NTFY_HOST,
                (unsigned)ESP.getFreeHeap());

  applyTlsConfig(stream);
  stream.setTimeout(NTFY_SOCKET_TIMEOUT_MS);

  if (!stream.connect(NTFY_HOST, NTFY_PORT)) {
    Serial.println("[ntfy] TCP/TLS connect failed");
    return false;
  }

  // On a cold boot lastId is empty and no ?since= is sent, so only genuinely
  // new messages arrive. Replaying the whole topic history at boot would
  // re-execute every command ever sent.
  String path = "/" + String(NTFY_TOPIC) + "/json";
  if (!lastId.isEmpty()) path += "?since=" + lastId;

  String req = "GET " + path + " HTTP/1.1\r\n"
               "Host: " + String(NTFY_HOST) + "\r\n"
               "User-Agent: esp32-wol-ntfy\r\n"
               "Accept: application/x-ndjson\r\n";
  if (!authHeader().isEmpty()) {
    req += "Authorization: " + authHeader() + "\r\n";
  }
  req += "Connection: keep-alive\r\n\r\n";

  stream.print(req);

  // Status line, then headers up to the blank line.
  String status = stream.readStringUntil('\n');
  status.trim();
  if (status.indexOf("200") < 0) {
    Serial.printf("[ntfy] unexpected response: %s\n", status.c_str());
    if (status.indexOf("401") >= 0 || status.indexOf("403") >= 0) {
      Serial.println("[ntfy] the server rejected the login -- check NTFY_USER / "
                     "NTFY_PASSWORD / NTFY_TOKEN and this user's access to the "
                     "topic");
    }
    stream.stop();
    return false;
  }

  while (stream.connected()) {
    String header = stream.readStringUntil('\n');
    header.trim();
    if (header.isEmpty()) break;   // end of headers
  }

  Serial.println("[ntfy] subscribed");
  state       = State::Streaming;
  lastDataMs  = millis();
  backoffMs   = NTFY_RECONNECT_MIN_MS;
  lineBuf     = "";
  return true;
}

// One newline-delimited JSON object from the stream.
void handleLine(const String& line) {
  if (line.length() < 2) return;   // blank keepalive padding

  // Keep only the fields we use, so an unexpectedly chatty message cannot
  // blow up the heap.
  JsonDocument filter;
  filter["id"]      = true;
  filter["event"]   = true;
  filter["message"] = true;
  filter["tags"]    = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, line, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[ntfy] could not parse line (%s)\n", err.c_str());
    return;
  }

  const char* event = doc["event"] | "";

  if (strcmp(event, "message") != 0) {
    // "open", "keepalive", "poll_request" -- nothing to do, but they prove the
    // connection is alive, which poll()'s watchdog already recorded.
    return;
  }

  // Only real messages advance the cursor. Keepalive events carry IDs too, and
  // feeding one back as ?since= would ask the server for a message it has
  // never heard of.
  String id = doc["id"] | "";
  if (!id.isEmpty()) lastId = id;

  // Commands and replies share one topic, so skip anything this device sent.
  for (JsonVariant tag : doc["tags"].as<JsonArray>()) {
    const char* t = tag.as<const char*>();
    if (t != nullptr && strcmp(t, NTFY_SELF_TAG) == 0) return;
  }

  if (alreadySeen(id)) {
    Serial.printf("[ntfy] ignoring duplicate %s\n", id.c_str());
    return;
  }

  String message = doc["message"] | "";
  if (message.isEmpty()) return;

  Serial.printf("[ntfy] << %s\n", message.c_str());
  if (onMessage) onMessage(message);
}

}  // namespace

void begin(MessageHandler handler) {
  onMessage = handler;
  lineBuf.reserve(256);
  nextAttemptMs = 0;
}

bool connected() { return state == State::Streaming && stream.connected(); }

void poll() {
  if (state == State::Disconnected) {
    if ((int32_t)(millis() - nextAttemptMs) < 0) return;
    if (!connect()) {
      nextAttemptMs = millis() + backoffMs;
      backoffMs = min(backoffMs * 2, (uint32_t)NTFY_RECONNECT_MAX_MS);
    }
    return;
  }

  if (!stream.connected()) {
    disconnect("socket closed by peer");
    return;
  }

  while (stream.available()) {
    char c = (char)stream.read();
    lastDataMs = millis();

    if (c == '\n') {
      String line = lineBuf;
      lineBuf = "";
      line.trim();
      handleLine(line);
    } else if (c != '\r') {
      // Drop absurdly long lines rather than growing the heap without bound.
      if (lineBuf.length() < NTFY_MAX_LINE) {
        lineBuf += c;
      } else if (lineBuf.length() == NTFY_MAX_LINE) {
        Serial.println("[ntfy] line too long, discarding");
        lineBuf += ' ';   // marks it oversized so the rest is skipped
      }
    }
  }

  // ntfy keepalives arrive roughly every 45 s. Silence past the timeout means
  // a socket that looks open but will never deliver again.
  if (millis() - lastDataMs > NTFY_STREAM_TIMEOUT_MS) {
    disconnect("no data past keepalive timeout");
  }
}

bool publish(const String& message, const String& title) {
  if (WiFi.status() != WL_CONNECTED) return false;

  NtfyClient client;
  applyTlsConfig(client);
  client.setTimeout(NTFY_SOCKET_TIMEOUT_MS);

  HTTPClient http;
  String url = String(NTFY_SCHEME) + NTFY_HOST + ":" + String(NTFY_PORT) + "/" +
               String(NTFY_TOPIC);

  if (!http.begin(client, url)) {
    Serial.println("[ntfy] publish: begin failed");
    return false;
  }

  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  http.addHeader("X-Tags", NTFY_SELF_TAG);   // lets poll() ignore our own echo
  if (!title.isEmpty()) http.addHeader("X-Title", title);
  if (!authHeader().isEmpty()) http.addHeader("Authorization", authHeader());

  int code = http.POST((uint8_t*)message.c_str(), message.length());
  http.end();

  if (code != HTTP_CODE_OK) {
    Serial.printf("[ntfy] publish failed (%d, heap %u)\n", code,
                  (unsigned)ESP.getFreeHeap());
    return false;
  }

  Serial.printf("[ntfy] >> %s\n", message.c_str());
  return true;
}

}  // namespace ntfy
