#pragma once

#include <Arduino.h>

namespace wol {

// Broadcasts a Wake-on-LAN magic packet for the given MAC.
//
// The packet goes to both the directed subnet broadcast and 255.255.255.255,
// on ports 9 and 7, repeated WOL_REPEATS times. That redundancy is deliberate:
// which combination actually reaches a sleeping NIC varies by router and by
// network card, and the extra packets cost nothing.
//
// Returns true if at least one datagram was accepted by the network stack.
// It cannot tell you whether the target woke -- nothing acknowledges a magic
// packet.
bool wake(const uint8_t mac[6]);

}  // namespace wol
