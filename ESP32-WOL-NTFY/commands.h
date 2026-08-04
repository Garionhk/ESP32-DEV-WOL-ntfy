#pragma once

#include <Arduino.h>

namespace commands {

// Parses one inbound ntfy message and publishes the reply.
//
// Replies are written so the passphrase is never echoed back into the topic,
// and failed authorization says only "unauthorized" -- no hint about which
// part was wrong.
void handle(const String& message);

}  // namespace commands
