#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <base64.h>

#include "certs.h"
#include "config.h"
#include "ntfy.h"
#include "settings.h"

namespace ntfy {
namespace {

// TLS is now a runtime setting, so the client type cannot be a typedef.
// NetworkClientSecure derives from NetworkClient and HTTPClient::begin() takes
// the base reference, so one pointer covers both cases.
NetworkClient* stream = nullptr;
MessageHandler onMessage;

String   cachedAuth;
bool     authBuilt = false;

String scheme() { return g_settings.ntfyTls ? "https://" : "http://"; }

// Allocates the right client for the configured mode, with the CA bundle and
// read timeout applied. Caller owns the result.
NetworkClient* makeClient() {
  if (g_settings.ntfyTls) {
    NetworkClientSecure* s = new NetworkClientSecure();
#if defined(NTFY_TLS_INSECURE)
    s->setInsecure();
#else
    s->setCACert(NTFY_ROOT_CAS);
#endif
    s->setTimeout(NTFY_SOCKET_TIMEOUT_MS);
    return s;
  }
  NetworkClient* c = new NetworkClient();
  c->setTimeout(NTFY_SOCKET_TIMEOUT_MS);
  return c;
}

// "Bearer tk_..." or "Basic base64(user:password)", or empty for an open
// server. Settings are fixed until the next reboot, so building once is safe.
const String& authHeader() {
  if (!authBuilt) {
    if (!g_settings.ntfyToken.isEmpty()) {
      cachedAuth = "Bearer " + g_settings.ntfyToken;
    } else if (!g_settings.ntfyUser.isEmpty()) {
      // This core's libb64 does not wrap lines, so the result is header-safe.
      cachedAuth = "Basic " +
                   base64::encode(g_settings.ntfyUser + ":" + g_settings.ntfyPass);
    }
    authBuilt = true;
  }
  return cachedAuth;
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

void disconnect(const char* why) {
  if (state != State::Disconnected || (stream && stream->connected())) {
    Serial.printf("[ntfy] disconnecting: %s\n", why);
  }
  if (stream) stream->stop();
  state = State::Disconnected;
  lineBuf = "";

  nextAttemptMs = millis() + backoffMs;
  backoffMs = min(backoffMs * 2, (uint32_t)NTFY_RECONNECT_MAX_MS);
}

bool connect() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!stream) return false;

  Serial.printf("[ntfy] connecting to %s:%u (%s, heap %u)\n",
                g_settings.ntfyHost.c_str(), (unsigned)g_settings.ntfyPort,
                g_settings.ntfyTls ? "tls" : "plain",
                (unsigned)ESP.getFreeHeap());

  if (!stream->connect(g_settings.ntfyHost.c_str(), g_settings.ntfyPort)) {
    Serial.println("[ntfy] TCP/TLS connect failed");
    return false;
  }

  // On a cold boot lastId is empty and no ?since= is sent, so only genuinely
  // new messages arrive. Replaying the whole topic history at boot would
  // re-execute every command ever sent.
  String path = "/" + g_settings.ntfyTopic + "/json";
  if (!lastId.isEmpty()) path += "?since=" + lastId;

  String req = "GET " + path + " HTTP/1.1\r\n"
               "Host: " + g_settings.ntfyHost + "\r\n"
               "User-Agent: esp32-wol-ntfy\r\n"
               "Accept: application/x-ndjson\r\n";
  if (!authHeader().isEmpty()) {
    req += "Authorization: " + authHeader() + "\r\n";
  }
  req += "Connection: keep-alive\r\n\r\n";

  stream->print(req);

  // Status line, then headers up to the blank line.
  String status = stream->readStringUntil('\n');
  status.trim();
  if (status.indexOf("200") < 0) {
    Serial.printf("[ntfy] unexpected response: %s\n", status.c_str());
    if (status.indexOf("401") >= 0 || status.indexOf("403") >= 0) {
      Serial.println("[ntfy] the server rejected the login -- check the ntfy "
                     "username, password or token in the settings page, and "
                     "this user's access to the topic");
    }
    stream->stop();
    return false;
  }

  while (stream->connected()) {
    String header = stream->readStringUntil('\n');
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

  // Allocated once. Settings cannot change without a reboot, so the client
  // type is fixed for the lifetime of the process.
  if (!stream) stream = makeClient();
}

bool connected() {
  return state == State::Streaming && stream && stream->connected();
}

void poll() {
  if (!stream) return;

  if (state == State::Disconnected) {
    if ((int32_t)(millis() - nextAttemptMs) < 0) return;
    if (!connect()) {
      nextAttemptMs = millis() + backoffMs;
      backoffMs = min(backoffMs * 2, (uint32_t)NTFY_RECONNECT_MAX_MS);
    }
    return;
  }

  if (!stream->connected()) {
    disconnect("socket closed by peer");
    return;
  }

  while (stream->available()) {
    char c = (char)stream->read();
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

  // A short-lived client per publish, so the TLS buffers are freed straight
  // after rather than sitting alongside the subscribe connection.
  NetworkClient* client = makeClient();
  if (!client) return false;

  int code;
  {
    // HTTPClient must be gone before the client is deleted. It keeps the
    // connection for reuse by default, so end() leaves its pointer to the
    // client set, and its destructor calls client->stop(). Destroyed after
    // `delete client`, that is a use-after-free -- harmless until the freed
    // block is reused, which the TLS buffers of the next subscribe
    // reconnect make near-certain.
    HTTPClient http;
    String url = scheme() + g_settings.ntfyHost + ":" + String(g_settings.ntfyPort) +
                 "/" + g_settings.ntfyTopic;

    if (!http.begin(*client, url)) {
      Serial.println("[ntfy] publish: begin failed");
      code = -1;
    } else {
      http.addHeader("Content-Type", "text/plain; charset=utf-8");
      http.addHeader("X-Tags", NTFY_SELF_TAG);   // lets poll() ignore our own echo
      if (!title.isEmpty()) http.addHeader("X-Title", title);
      if (!authHeader().isEmpty()) http.addHeader("Authorization", authHeader());

      code = http.POST((uint8_t*)message.c_str(), message.length());
      http.end();
    }
  }
  delete client;

  if (code != HTTP_CODE_OK) {
    Serial.printf("[ntfy] publish failed (%d, heap %u)\n", code,
                  (unsigned)ESP.getFreeHeap());
    return false;
  }

  Serial.printf("[ntfy] >> %s\n", message.c_str());
  return true;
}

}  // namespace ntfy
