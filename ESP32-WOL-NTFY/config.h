#pragma once

// Tunables. Credentials live in secrets.h, not here.

// ---- Host storage -------------------------------------------------------
#define MAX_HOSTS          16   // entries the NVS blob will hold
#define MAX_NAME_LEN       20   // characters in a host nickname
#define NVS_NAMESPACE      "wolntfy"
#define NVS_KEY_HOSTS      "hosts"

// ---- Wake-on-LAN --------------------------------------------------------
#define WOL_REPEATS         3   // magic packets per wake request
#define WOL_REPEAT_GAP_MS 100   // pause between repeats

// ---- ntfy ---------------------------------------------------------------
// Server host, port, TLS, login and topic live in secrets.h -- they describe
// your deployment. Only behaviour tuning belongs here.

// ntfy sends a keepalive about every 45 s. If nothing at all arrives for this
// long the socket is presumed dead -- the usual failure mode for an always-on
// device is a TLS connection that stays "open" but never delivers again.
#define NTFY_STREAM_TIMEOUT_MS   90000UL

#define NTFY_RECONNECT_MIN_MS     1000UL   // first retry delay
#define NTFY_RECONNECT_MAX_MS    30000UL   // ceiling for exponential backoff

// Read timeout for the subscribe socket, in MILLISECONDS.
// NetworkClientSecure does not override setTimeout(), so this lands on
// Stream::setTimeout(), which is milliseconds -- not the seconds that
// NetworkClient::setTimeout() and setHandshakeTimeout() take. Passing seconds
// here means the header read gives up in ~15 ms and the status line comes back
// empty.
#define NTFY_SOCKET_TIMEOUT_MS    15000UL

// Tag stamped on every message this device publishes. Because commands and
// replies share one topic, the receive path uses this tag to ignore its own
// output and avoid a feedback loop.
#define NTFY_SELF_TAG      "wolbot"

#define NTFY_DEDUP_SLOTS      8   // recently-seen message IDs remembered

// Longest incoming message body accepted. Anything larger is dropped rather
// than parsed, so a huge publish cannot exhaust the heap.
#define NTFY_MAX_LINE      2048

// ---- Authorization ------------------------------------------------------
// Wake is unauthenticated by default: it is harmless and it is the command
// you want to be quick. Set to 1 to require the passphrase on wake too.
#define REQUIRE_PASS_FOR_WAKE   0

#define AUTH_MAX_FAILURES       3        // failures before lockout
#define AUTH_LOCKOUT_MS     60000UL      // how long mutating commands stay shut

// ---- Networking ---------------------------------------------------------
#define WIFI_CONNECT_TIMEOUT_MS  30000UL
#define WIFI_RETRY_INTERVAL_MS   10000UL
#define NTP_SERVER_1       "pool.ntp.org"
#define NTP_SERVER_2       "time.nist.gov"
#define NTP_SYNC_TIMEOUT_MS      30000UL

// TLS certificate validation compares against the clock, so the device must
// know roughly what year it is before the first connection. Anything past
// 2024-01-01 means NTP has replied.
#define NTP_MIN_VALID_EPOCH   1704067200UL

// ---- Debug --------------------------------------------------------------
#define SERIAL_BAUD        115200

// Uncomment to skip TLS certificate verification. Debugging only -- the
// passphrase travels over this connection.
// #define NTFY_TLS_INSECURE
