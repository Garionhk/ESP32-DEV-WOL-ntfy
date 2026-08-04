#pragma once

#include <Arduino.h>
#include <functional>

// ntfy transport: one long-lived TLS connection streaming inbound messages,
// plus short-lived POSTs for replies.

namespace ntfy {

// Called for each inbound message that survives dedup and the self-message
// filter. The argument is the message body.
using MessageHandler = std::function<void(const String& message)>;

void begin(MessageHandler handler);

// Pumps the stream. Non-blocking; call every loop().
void poll();

// Publishes a reply to the topic. Stamped with NTFY_SELF_TAG so poll() will
// ignore it when it comes back down the stream.
bool publish(const String& message, const String& title = "");

bool connected();

}  // namespace ntfy
