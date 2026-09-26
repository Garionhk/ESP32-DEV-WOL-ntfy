# ESP32 WOL-NTFY

An ESP32 or ESP32-S3 that listens to an [ntfy](https://ntfy.sh) topic and sends
Wake-on-LAN magic packets to computers it remembers.

Magic packets are link-layer broadcasts and cannot cross the internet, so something has to
already be inside the network to emit them. This is that something: ntfy carries the
command in from anywhere, the ESP32 turns it into a broadcast on the local segment.

Send it `<passphrase> help` from the ntfy app on your phone and it replies with its own
command list.

## Hardware

Runs on both the **ESP32-S3** and the original **ESP32** — there is no chip-specific code,
only `GPIO0` for the BOOT button, which is the boot strapping pin on both.

| Board | FQBN | Flash used | Computers | Status |
|---|---|---|---|---|
| **ESP32-S3 N16R8** (16 MB flash, 8 MB OPI PSRAM) | `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app` | 1.11 MB (35%) | **16** | builds clean; not yet run on hardware |
| ESP32-Dev (WROOM-32, 4 MB) | `esp32:esp32:esp32:PartitionScheme=huge_app` | 1.15 MB (36%) | **16** | verified end to end |

The 8 MB PSRAM on an N16R8 is not used — the firmware needs about 48 KB of RAM — but
enabling it costs nothing and matches the module, so `PSRAM=opi` is included above.
Leaving it out also works.

Whichever board you use, it needs:

- **2.4 GHz WiFi.** Neither chip has a 5 GHz radio.
- **The same network segment as the machines you want to wake.** Magic packets are
  broadcasts and do not cross subnets or VLANs.

### How many computers it can wake

**16 on both boards** — the same number, because the limit is a software constant
(`MAX_HOSTS` in `config.h`), not a hardware constraint. The extra flash and PSRAM on an
S3 N16R8 buy nothing here: the host list lives in the NVS partition, which is `0x5000`
(20 KB) on both targets, and the whole list is well under a kilobyte.

Measured with a full list of 16:

| Names | Stored size | Per entry |
|---|---|---|
| 20 characters (the maximum) | 753 bytes | 47 bytes |
| short, e.g. `pc1` | 503 bytes | 31 bytes |

So 16 entries use roughly a fifth of the 4000-byte ceiling NVS puts on a single string
value. Raising `MAX_HOSTS` is a one-line change and the practical ceiling is about **80**
with long names, or **125** with short ones.

Two things start to bite before that ceiling, though, and neither is about storage:

- **The `list` reply** is one ntfy message, and ntfy caps a message body at 4 KB. Each line
  is up to 40 bytes, so the reply gets truncated somewhere around 100 entries.
- **`wake all` is serial**, and each host takes about 200 ms (3 magic packets with 100 ms
  gaps). 16 hosts take roughly 3 seconds; 80 would take 16, during which the device is not
  servicing the ntfy stream.

16 is a deliberate default rather than a limit worth removing — but if you need 30, change
the constant and rebuild.

### ESP32-S3 serial output

S3 boards vary in how `Serial` reaches you, and it matters because the WiFi diagnostics
print there. With the default `CDCOnBoot=default`, `Serial` goes to **UART0** — correct for
a board with a separate USB-to-UART bridge (CP2102, CH343), such as the DevKitC-1's UART
port.

If your board exposes **only** a native USB port, add `CDCOnBoot=cdc` or you will see
nothing:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app,CDCOnBoot=cdc" ESP32-WOL-NTFY
```

## Commands

Send these as plain messages to your topic. The device replies on the same topic.
**Every command starts with your command passphrase:**

```
sekret wake desktop
sekret add desktop AA:BB:CC:DD:EE:FF
sekret remove desktop
```

| Command (after the passphrase) | Aliases | What it does |
|---|---|---|
| `help` | `?`, `commands` | replies with the command list |
| `list` | `ls` | shows every saved computer |
| `wake <name>` | `wol` | sends the magic packet; `wake all` hits every host |
| `add <name> <mac>` | `save` | remembers a computer |
| `remove <name>` | `rm`, `del`, `delete`, `forget` | forgets one |
| `status` | `stat` | uptime, IP, settings URL, wifi, free memory |
| `factoryreset` | — | wipes settings, reboots into setup; `factoryreset all` also forgets saved computers |

A message without the right passphrase is **ignored silently** — no reply at all, so a
stranger who finds the topic gets no sign the device is listening. The passphrase is
matched case-insensitively, so phone auto-capitalisation (`Sekret wake desktop`) does not
lock you out. It is one word: everything after the first space is the command.

`add`, `remove` and `factoryreset` **always** need a passphrase to be set. The settings page
will not save without one, but if the stored one is ever blank, the other commands answer
without a prefix while those three refuse — an ungated topic stays read-only.

Verbs are case-insensitive, so `WAKE`, `Wake`, and `wake` are the same. The device's own
`help` reply lists only the primary form of each command to stay readable on a phone; the
aliases above all work.

MAC addresses may be written `aa:bb:cc:dd:ee:ff`, `aa-bb-cc-dd-ee-ff`, or `aabbccddeeff`.
Names are case-insensitive, up to 20 characters of `a-z 0-9 _ -`. **Up to 16 computers**
are stored, in NVS, so they survive power loss — the same 16 on either board, and raisable
(see [How many computers it can wake](#how-many-computers-it-can-wake)).

## Setup

**Nothing is compiled in.** WiFi, the ntfy server, the topic, the login and both
passphrases all live in NVS and are set from the device's own web page. The binary
contains no credentials, so a build can be shared.

### 1. Flash

For an **ESP32-S3 N16R8**:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app" ESP32-WOL-NTFY
```

For an original **ESP32** (4 MB):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app ESP32-WOL-NTFY
```

`huge_app` is deliberate on both: this firmware has no OTA, so the default scheme's second
app slot is dead weight. Without the switch the build sits at 85% of the default app area;
with it, about 35%.

Find the port with `arduino-cli board list`, then upload with the **same FQBN** you
compiled with:

```bash
arduino-cli upload -p /dev/cu.SLAB_USBtoUART --fqbn esp32:esp32:esp32:PartitionScheme=huge_app ESP32-WOL-NTFY
```

An S3 on its native USB port usually appears as `/dev/cu.usbmodem*` rather than
`/dev/cu.SLAB_USBtoUART`. `board list` also shows Bluetooth ports and can show stale
`usbserial-*` entries with no device behind them; to see what is genuinely plugged in:

```bash
ioreg -p IOUSB -l -w 0 | grep "USB Product Name"
```

If your board is not one of the two above, find its FQBN with
`arduino-cli board listall esp32`.

See [flash.md](flash.md) for building a shareable `.bin` and flashing with esptool —
including the bootloader offset, which **differs between the S3 and the original ESP32**.

### 2. First-boot setup

On a device with no stored config, the ESP32 starts its own WiFi network:

1. Join **`WOL-Setup-XXXX`** (the suffix is the last four of its MAC) from a phone or
   laptop. The setup page should pop up on its own; if not, open **http://192.168.4.1**.
2. Fill in the form and press save. The device restarts and joins your network.

The form covers everything:

| Section | Fields |
|---|---|
| WiFi | network (picked from a live scan) and password |
| ntfy server | host, port, TLS on/off, topic |
| ntfy login | username + password, or a token — blank for an open server |
| Passphrases | command passphrase, and an admin password for this page |

- **Command passphrase** prefixes every command (`<passphrase> wake pc`) and is required.
- **Admin password** protects the settings page (username **`admin`**) and is required. It
  must differ from the command passphrase (compared case-insensitively), because that one
  travels through ntfy.
- **Topic** must be 1–64 letters, digits, `-` or `_`. Make it long and unguessable.

Password fields are never shown back. Leaving one blank keeps the stored value; tick
*clear stored ntfy login* to remove the ntfy username/password/token.

The network picker is a live scan rather than a text box on purpose — a hand-typed SSID is
how a one-character typo once cost a full reflash cycle.

**The setup screen never times out.** An unprovisioned device has no useful fallback — no
credentials means no WiFi, no ntfy, no purpose — so a page telling you how to fix that is
the most useful thing it can be doing.

### 3. Find it afterwards

Once online it announces itself on your topic:

```
WOL bot online
ip: 192.168.0.89
settings: http://192.168.0.89
1 computer(s) saved. Send "<passphrase> help" for commands.
```

`<passphrase> status` reports the same address at any time. Browse to it to change any setting later —
the page is always running, and asks for the admin login (username **`admin`**).

## Changing settings later

| Situation | What to do |
|---|---|
| Device is online | Browse to its IP and log in as `admin` |
| WiFi is down | It falls back to the `WOL-Setup-XXXX` network (portal at `192.168.4.1`, admin login required) while retrying your router every 10 s |
| Moved house, or forgot the admin password | Press **EN**, release it, then **immediately press and hold BOOT** for ~2 s — setup portal, no login required |
| Remotely | `<passphrase> factoryreset` over ntfy |

**Timing the BOOT button matters.** The ESP32 samples BOOT (GPIO0) *during* reset —
holding it through the EN press puts the chip into its serial bootloader instead, and it
just sits there. Press BOOT only after letting go of EN; the device watches for it for 2 s
(`BOOT_WINDOW_MS`) after `press BOOT now` appears on serial. That window only runs on a
provisioned device, so a normal boot is ~2 s slower.

If no admin password is stored, the settings page does not fall open: on the LAN and on
the fallback AP it answers **Settings locked** until one is set through the BOOT-button
route above.

`factoryreset` wipes the configuration and reboots into setup. Saved computers are kept —
they live in a separate NVS namespace. `<passphrase> factoryreset all` wipes those too.

Physical access bypasses the admin password by design. Anyone holding the board can
reflash it anyway, so a BOOT-button escape hatch costs no real security and saves you from
a lockout.

## Using a different ntfy server

Set the host, port and TLS checkbox on the settings page. Public ntfy.sh, a self-hosted
instance over HTTPS, or plain HTTP on a LAN all work; TLS is a runtime choice, not a build
flag.

For a server with access control (`auth-file` plus `auth-default-access deny`), fill in
the ntfy username and password, or a token — a token wins if both are given. Credentials
are sent as HTTP Basic on every request, both on the subscribe stream and on each reply.
On the server, that user needs read **and** write access to the topic, since the device
both listens and publishes:

```
ntfy access esp32 wol-topic rw
```

A rejected login is reported rather than silently retried:

```
[ntfy] unexpected response: HTTP/1.1 401 Unauthorized
[ntfy] the server rejected the login -- check the ntfy username, password or token
       in the settings page, and this user's access to the topic
```

**On an open server the topic name is the only thing protecting the device** — anyone who
knows it can read the history, including your command passphrase, and can wake, add and
remove at will. Topic reservation is a paid ntfy.sh feature, so on the free tier treat the
topic name as a credential and generate a random one:

```bash
openssl rand -hex 8
```

With a login configured, the login is the gate and the topic name matters much less.

### Certificates for a self-hosted HTTPS server

The bundled roots cover Let's Encrypt and Google Trust Services, which between them handle
public ntfy.sh, most self-hosted servers, and anything behind Cloudflare. A private CA or
self-signed certificate is not supported from the web form — add it to `certs.h` and
rebuild.

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
   Send `<passphrase> wake <name>` and watch for 102-byte datagrams. If they arrive, the ESP32 side is
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
hardware: recovery from a corrupt stored blob, the passphrase gate (silent on a wrong or
missing passphrase, case-insensitive on a right one), and the guarantee that no reply ever
echoes the passphrase back into the topic.

## How it works

| File | Role |
|---|---|
| `ESP32-WOL-NTFY.ino` | boot flow and the setup / fallback / online state machine |
| `settings.cpp` | runtime configuration in NVS — what `secrets.h` used to be |
| `webconfig.cpp` | setup portal and settings page |
| `ntfy.cpp` | long-lived stream in, short-lived POSTs out |
| `commands.cpp` | parse, authorize, dispatch, reply |
| `store.cpp` | the host list, persisted in NVS as JSON |
| `wol.cpp` | magic packet construction and broadcast |
| `config.h` | timeouts, limits, feature flags — no user data |
| `certs.h` | root CAs for verifying the ntfy server |

A few decisions worth knowing about if you modify it:

- **Configuration is runtime, not compile-time.** Consequently `NTFY_USE_TLS` cannot be a
  typedef any more: `ntfy.cpp` holds a `NetworkClient*` and allocates a
  `NetworkClientSecure` or a plain `NetworkClient` at connect time. `HTTPClient::begin()`
  takes the base reference, so both work through one path.
- **Settings changes reboot the device** rather than applying live. WiFi, TLS mode and the
  subscription each have a different re-init path; a 3-second restart avoids a class of
  partial-state bugs.
- **The `ok` sentinel is cleared first and written last** when saving, so a power cut
  mid-save leaves the record marked incomplete rather than half-valid. A record that is flagged complete but
  missing an SSID or topic re-triggers setup instead of booting into a broken state.
- **Config and the host list use different NVS namespaces** (`wolcfg`, `wolntfy`), which
  is what lets `factoryreset` clear credentials while keeping your saved computers.
- **DNS hijacking only happens in AP mode.** The captive-portal redirect is what makes
  phones pop the setup page; doing it in STA would break ordinary name resolution on your
  LAN.
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
