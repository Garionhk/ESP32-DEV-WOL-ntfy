# ESP32 WOL-NTFY

An ESP32 that listens to an [ntfy](https://ntfy.sh) topic and sends Wake-on-LAN magic
packets to computers it remembers.

Magic packets are link-layer broadcasts and cannot cross the internet, so something has to
already be inside the network to emit them. This is that something: ntfy carries the
command in from anywhere, the ESP32 turns it into a broadcast on the local segment.

Send it `help` from the ntfy app on your phone and it replies with its own command list.

## Commands

Send these as plain messages to your topic. The device replies on the same topic.

| Command | Aliases | Auth | What it does |
|---|---|---|---|
| `help` | `?`, `commands` | — | replies with the command list |
| `list` | `ls` | — | shows every saved computer |
| `wake <name>` | `wol` | — | sends the magic packet; `wake all` hits every host |
| `add <pass> <name> <mac>` | `save` | passphrase | remembers a computer |
| `remove <pass> <name>` | `rm`, `del`, `delete`, `forget` | passphrase | forgets one |
| `status` | `stat` | — | uptime, IP, wifi, free memory |

**The passphrase comes first** on the two commands that need it, before the name:

```
add sekret desktop AA:BB:CC:DD:EE:FF
remove sekret desktop
```

Verbs are case-insensitive, so `WAKE`, `Wake`, and `wake` are the same. The device's own
`help` reply lists only the primary form of each command to stay readable on a phone; the
aliases above all work.

MAC addresses may be written `aa:bb:cc:dd:ee:ff`, `aa-bb-cc-dd-ee-ff`, or `aabbccddeeff`.
Names are case-insensitive, up to 20 characters of `a-z 0-9 _ -`. Up to 16 computers are
stored, in NVS, so they survive power loss.

Three wrong passphrases in a row lock out `add` and `remove` for 60 seconds.

## Using a different ntfy server

Everything about the server lives in `secrets.h`. Public ntfy.sh is the default; any
self-hosted instance works too.

```c
#define NTFY_HOST     "ntfy.example.com"
#define NTFY_PORT     443
#define NTFY_USE_TLS  1        // 0 for plain http on a LAN server
```

`NTFY_USE_TLS` is a compile-time switch, not a runtime one: it selects `WiFiClientSecure`
or plain `WiFiClient`. Turning it off drops the TLS stack and the bundled certificates,
which is worth about 7% of flash.

### Login

For a server with access control (`auth-file` plus `auth-default-access deny`), give the
device credentials. Either form works; a token wins if both are set.

```c
#define NTFY_TOKEN     "tk_abc123..."   // ntfy token add <user>
// or
#define NTFY_USER      "esp32"
#define NTFY_PASSWORD  "..."
```

Username and password are sent as HTTP Basic on every request — both on the subscribe
stream and on each reply. On the server, that user needs read **and** write access to the
topic, since the device both listens and publishes:

```
ntfy access esp32 wol-topic rw
```

If the login is wrong, the device says so explicitly rather than just retrying:

```
[ntfy] unexpected response: HTTP/1.1 401 Unauthorized
[ntfy] the server rejected the login -- check NTFY_USER / NTFY_PASSWORD / NTFY_TOKEN
       and this user's access to the topic
```

With auth enabled the topic name stops being the thing protecting you, so it no longer has
to be long and random — the login is the gate.

### Certificates for a self-hosted HTTPS server

If your server uses Let's Encrypt, the bundled ISRG roots already cover it and there is
nothing to do. For a private CA or a self-signed certificate, paste the CA into
`NTFY_CUSTOM_CA` in `secrets.h` and it is used instead of the bundled roots.

## Setup

### 1. Credentials

```bash
cp ESP32-WOL-NTFY/secrets.h.example ESP32-WOL-NTFY/secrets.h
```

Then edit `secrets.h`: WiFi SSID and password, your ntfy topic, and a passphrase.
`secrets.h` is gitignored.

**Choose the topic name carefully.** On free ntfy.sh anyone who knows the topic can read
its whole history — including every passphrase you have ever sent — and can then add,
remove, and wake at will. Topic reservation is a paid ntfy.sh feature, so on the free tier
the topic name *is* the primary credential. Generate a random one:

```bash
openssl rand -hex 8
```

and use something like `wol-9f3a7c2e5b81d4a6`, not `eddy-wol`.

### 2. Build and flash

The board must be on the same network segment as the computers you want to wake.

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 ESP32-WOL-NTFY
```

This board enumerates as a Silicon Labs CP2102, so the port is
`/dev/cu.SLAB_USBtoUART`:

```bash
arduino-cli upload -p /dev/cu.SLAB_USBtoUART --fqbn esp32:esp32:esp32 ESP32-WOL-NTFY
```

`arduino-cli board list` also shows Bluetooth ports and can show stale
`usbserial-*` entries with no device behind them. To see which USB serial adapters are
actually plugged in:

```bash
ioreg -p IOUSB -l -w 0 | grep "USB Product Name"
```

If your board is not a generic dev module, find its FQBN with
`arduino-cli board listall esp32` and substitute it.

### 3. Watch it boot

```bash
arduino-cli monitor -p /dev/cu.SLAB_USBtoUART -c baudrate=115200
```

You should see the WiFi IP, an NTP sync, `[ntfy] subscribed`, and an "online" message in
your ntfy app. Then send `help` from the app.

## If it will not join WiFi

On a failed join the device prints the decoded status code and then scans, listing every
2.4 GHz network it can actually see and flagging whether your configured SSID is among
them:

```
[wifi] failed to join: DISCONNECTED
[wifi] 9 network(s) visible on 2.4 GHz:
  my-network                       ch11   -58 dBm  secured   <== configured SSID
  neighbour-wifi                   ch6    -71 dBm  secured
  ...
[wifi] SSID is in range, so the password is the likely problem
```

That distinguishes the three cases that otherwise look identical: a typo in the SSID, a
5 GHz-only network (the ESP32 has no 5 GHz radio, so it simply is not there), and a wrong
password. If the scan reports 0 networks, suspect the antenna or an underpowered USB port
rather than the credentials.

## Waking a computer that will not wake

The device can only tell you it *sent* the packet — nothing acknowledges a magic packet.
When a machine does not wake, check in this order:

1. **Is the packet arriving?** On a Mac on the same LAN:
   ```bash
   sudo tcpdump -i en0 -n udp port 9
   ```
   Send `wake <name>` and watch for 102-byte datagrams. If they arrive, the ESP32 side is
   fine and the problem is on the target.
2. **Same subnet?** Broadcasts do not cross subnets or VLANs. The ESP32 must be on the
   same segment as the target.
3. **WOL enabled in BIOS/UEFI?** Often called "Wake on LAN", "Power on by PCI-E", or
   "Resume by LAN", and off by default.
4. **WOL enabled in the OS?** On Windows, in the adapter's Device Manager properties, and
   note that **Fast Startup** silently breaks WOL from a full shutdown — disable it, or
   use sleep/hibernate instead.
5. **Wired vs wireless.** WOL over WiFi (WoWLAN) is rarely supported; ethernet is the
   reliable path.

## Tests

The pure logic — MAC parsing, name validation, the NVS round-trip, and the whole command
surface — is tested natively. No board required:

```bash
./test/run.sh
```

This compiles the real `store.cpp` and `commands.cpp` against a small Arduino shim in
`test/shim/`, with the transport and radio stubbed. It covers things awkward to check on
hardware: recovery from a corrupt stored blob, the passphrase lockout and its expiry, and
the guarantee that no reply ever echoes the passphrase back into the topic.

## How it works

| File | Role |
|---|---|
| `ESP32-WOL-NTFY.ino` | wiring: WiFi, NTP, then the poll loop |
| `ntfy.cpp` | long-lived TLS stream in, short-lived POSTs out |
| `commands.cpp` | parse, authorize, dispatch, reply |
| `store.cpp` | the host list, persisted in NVS as JSON |
| `wol.cpp` | magic packet construction and broadcast |
| `config.h` | timeouts, limits, feature flags |
| `certs.h` | root CAs for verifying ntfy.sh |

A few decisions worth knowing about if you modify it:

- **Cold boot does not replay history.** The subscription sends `?since=` only on
  *reconnect*. Without that, every reboot would re-execute every command ever sent to the
  topic.
- **A keepalive ID is never used as the `since` cursor** — only real messages advance it,
  since the server would not recognise a keepalive ID.
- **The device ignores its own messages.** Commands and replies share one topic, so
  everything published carries the tag `wolbot` and the receive path skips anything
  carrying it. Without this it would answer itself forever.
- **Silence is treated as death.** ntfy sends a keepalive about every 45 s; 90 s of
  nothing drops the socket and reconnects with backoff. A TLS connection that stays "open"
  but never delivers again is the usual way an always-on device goes quiet.
- **Broadcasts use a raw socket.** The Arduino core's `WiFiUDP` never sets `SO_BROADCAST`,
  which lwIP requires for broadcast sends, so `wol.cpp` opens its own socket.
- **`delay()` never appears in the receive path**, only a 10 ms yield in `loop()`.

### Updating the root certificates

`certs.h` bundles four roots covering two issuer families, so one firmware works against
both common hosting setups:

| Root | Covers | Expires |
|---|---|---|
| ISRG Root X1 | Let's Encrypt (ntfy.sh, most self-hosted) | 2035 |
| ISRG Root YR | Let's Encrypt next-gen | — |
| GTS Root R4 | Google Trust Services (Cloudflare-fronted) | 2036 |
| GlobalSign Root CA | cross-signs GTS Root R4 | 2028 |

Both families are bundled deliberately: Cloudflare rotates between issuers, so pinning
only Let's Encrypt gives you a server that works right up until it silently doesn't. If
TLS starts failing after an issuer change, inspect the live chain:

```bash
openssl s_client -connect ntfy.sh:443 -servername ntfy.sh -showcerts
```

and replace the PEM blocks in `certs.h` with the new root. To confirm TLS is the actual
problem first, uncomment `NTFY_TLS_INSECURE` in `config.h` — if it connects with
verification off, the certificates are the cause. Do not leave it that way: the passphrase
travels over this connection.

Certificate validation also depends on the clock, which is why `setup()` waits for NTP
before connecting. A failed NTP sync looks exactly like a certificate problem.
