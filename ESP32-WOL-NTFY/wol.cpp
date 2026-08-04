#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>

#include "config.h"
#include "store.h"
#include "wol.h"

namespace wol {
namespace {

const uint16_t PORTS[] = {9, 7};

// A magic packet is 6 bytes of 0xFF followed by the target MAC repeated 16
// times -- 102 bytes total.
void buildPacket(const uint8_t mac[6], uint8_t packet[102]) {
  memset(packet, 0xFF, 6);
  for (int i = 0; i < 16; i++) memcpy(packet + 6 + i * 6, mac, 6);
}

// Directed broadcast for the ESP32's own subnet, e.g. 192.168.1.255. Some
// routers drop 255.255.255.255 but forward this one, so both are sent.
IPAddress subnetBroadcast() {
  uint32_t ip   = (uint32_t)WiFi.localIP();
  uint32_t mask = (uint32_t)WiFi.subnetMask();
  return IPAddress((ip & mask) | ~mask);
}

// A raw socket rather than WiFiUDP, because WiFiUDP never sets SO_BROADCAST
// (verified in the 3.3.10 core's NetworkUdp.cpp) and lwIP refuses broadcast
// sends without it. Everything here is a broadcast, so that option is the
// whole point.
int openBroadcastSocket() {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    Serial.printf("[wol] socket() failed: %d\n", errno);
    return -1;
  }

  int yes = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) {
    Serial.printf("[wol] SO_BROADCAST failed: %d\n", errno);
    close(sock);
    return -1;
  }
  return sock;
}

bool sendTo(int sock, IPAddress target, uint16_t port, const uint8_t* packet,
            size_t len) {
  struct sockaddr_in dest;
  memset(&dest, 0, sizeof(dest));
  dest.sin_family      = AF_INET;
  dest.sin_port        = htons(port);
  dest.sin_addr.s_addr = (uint32_t)target;

  int sent = sendto(sock, packet, len, 0, (struct sockaddr*)&dest, sizeof(dest));
  return sent == (int)len;
}

}  // namespace

bool wake(const uint8_t mac[6]) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wol] not on WiFi, cannot send");
    return false;
  }

  int sock = openBroadcastSocket();
  if (sock < 0) return false;

  uint8_t packet[102];
  buildPacket(mac, packet);

  const IPAddress broadcast = subnetBroadcast();
  const IPAddress targets[] = {broadcast, IPAddress(255, 255, 255, 255)};
  bool any = false;

  for (int r = 0; r < WOL_REPEATS; r++) {
    for (IPAddress target : targets) {
      for (uint16_t port : PORTS) {
        if (sendTo(sock, target, port, packet, sizeof(packet))) any = true;
      }
    }
    if (r < WOL_REPEATS - 1) delay(WOL_REPEAT_GAP_MS);
  }

  close(sock);

  Serial.printf("[wol] %s magic packet for %s via %s\n",
                any ? "sent" : "FAILED to send",
                store::formatMac(mac).c_str(),
                broadcast.toString().c_str());
  return any;
}

}  // namespace wol
