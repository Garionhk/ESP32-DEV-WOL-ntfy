#pragma once

#include <Arduino.h>

namespace commands {

// Parses one inbound ntfy message and publishes the reply.
//
//   * the command passphrase must prefix every command, matched
//     case-insensitively so phone auto-capitalisation doesn't lock you out;
//     a message without it is ignored silently -- no reply at all;
//   * add / remove / factoryreset ALWAYS require that a passphrase is set;
//   * replies never echo the passphrase back into the topic.
void handle(const String& message);

}  // namespace commands
