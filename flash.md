# Building and distributing firmware

How to produce a `.bin` other people can flash to their own ESP32, and how they
flash it.

Nothing user-specific is compiled in — Wi-Fi credentials, the ntfy server, topic,
login and both passphrases live in NVS on the device, entered through the setup
portal on first boot. That is what makes the binary shareable.

---

## 1. Prerequisites (you, the builder)

[arduino-cli](https://arduino.github.io/arduino-cli/) plus the ESP32 core and one
library:

```bash
arduino-cli core install esp32:esp32
```

```bash
arduino-cli lib install ArduinoJson
```

`WebServer`, `DNSServer`, `WiFiClientSecure`, `HTTPClient` and `Preferences` all
ship with the core.

## 2. Build

For an **ESP32-S3 N16R8** (16 MB flash, 8 MB OPI PSRAM):

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app" --export-binaries ESP32-WOL-NTFY
```

For an original **ESP32** (4 MB):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app --export-binaries ESP32-WOL-NTFY
```

The artifacts land in a per-target folder:

```
ESP32-WOL-NTFY/build/esp32.esp32.esp32s3/     <- S3
ESP32-WOL-NTFY/build/esp32.esp32.esp32/       <- original ESP32
```

| File | S3 N16R8 | ESP32 4 MB | Purpose |
| --- | --- | --- | --- |
| `ESP32-WOL-NTFY.ino.bin` | 1.11 MB | 1.15 MB | the application |
| `ESP32-WOL-NTFY.ino.bootloader.bin` | 20 KB | 25 KB | second-stage bootloader |
| `ESP32-WOL-NTFY.ino.partitions.bin` | 3 KB | 3 KB | partition table |
| `boot_app0.bin` | 8 KB | 8 KB | OTA selector |
| `ESP32-WOL-NTFY.ino.merged.bin` | **16 MB** | **4 MB** | all of the above, pre-combined |

The application is about 35% of the 3 MB app partition either way, so there is plenty of
headroom.

`--export-binaries` is what writes these into the sketch folder; without it arduino-cli
builds into a temporary directory and throws the result away.

Note `merged.bin` spans the **whole** flash, so it is 16 MB for an N16R8 and 4 MB for a
4 MB ESP32. The individual files are the same either way and total about 1.15 MB.

### Why `huge_app`

The default partition scheme reserves a second application slot for OTA updates.
This firmware has no OTA, so that slot is dead weight — and without the switch
the build sits at **85%** of the default 1.25 MB app area, which leaves no room
to grow. `huge_app` gives 3 MB and drops the same image to 36%.

The scheme is baked into `partitions.bin`, so it travels with the firmware as
long as whoever flashes it writes the partition table too (both methods below
do).

### Chip and flash size matter

**Images are not interchangeable between the S3 and the original ESP32.** They are
different architectures (Xtensa LX7 vs LX6), so an S3 image will not run on an ESP32 and
esptool will refuse the write once it detects the chip.

Flash size is baked in too, and `merged.bin` is a whole-flash image, so it assumes the size
it was built for. The original-ESP32 command above targets 4 MB, which covers the ESP32
DevKitC, NodeMCU-32S, Wemos D1 R32 and most ESP-WROOM-32 modules. For an 8 MB module,
rebuild rather than reusing the image:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,FlashSize=8M --export-binaries ESP32-WOL-NTFY
```

Name the outputs clearly when you publish them, e.g. `wol-ntfy-v1.0.0-esp32s3-n16r8.bin`
and `wol-ntfy-v1.0.0-esp32-4mb.bin`. Run `arduino-cli board listall esp32` if your board is
neither.

### ESP32-S3 serial output

If the S3 board exposes only a native USB port (no separate CP2102/CH343 bridge), add
`CDCOnBoot=cdc` so `Serial` reaches it; otherwise `Serial` goes to UART0 and the boot log
and WiFi diagnostics appear nowhere.

## 3. Confirm the binary carries no secrets

Do this before publishing. It is the whole point of keeping configuration off the
compile-time path:

```bash
strings ESP32-WOL-NTFY/build/esp32.esp32.esp32/ESP32-WOL-NTFY.ino.bin | grep -iE "your-ssid|your-wifi-password|your-topic|your-ntfy-host"
```

Substitute your own network name, password, topic and server. Expect **no
output**.

Grep for the actual values, not for the word "password" — the settings form
contains `<input type=password …>` markup, so searching for the word produces
harmless hits and hides a real one.

Sanity-check that `strings` is actually reading the file, so an empty result
means "clean" rather than "nothing was scanned". This should print a non-zero
count:

```bash
strings ESP32-WOL-NTFY/build/esp32.esp32.esp32/ESP32-WOL-NTFY.ino.bin | grep -c "ESP32 WOL-NTFY"
```

Publish a checksum alongside the binary:

```bash
shasum -a 256 ESP32-WOL-NTFY/build/esp32.esp32.esp32/ESP32-WOL-NTFY.ino.bin
```

---

## 4. Instructions for the person flashing

They need [esptool](https://github.com/espressif/esptool) — no Arduino
toolchain, no source:

```bash
pip install esptool
```

### Find the port

| OS | Via a USB-UART bridge | Via an S3's native USB |
| --- | --- | --- |
| macOS | `/dev/cu.SLAB_USBtoUART`, `/dev/cu.usbserial-0001` | `/dev/cu.usbmodem*` |
| Linux | `/dev/ttyUSB0` | `/dev/ttyACM0` |
| Windows | `COM3`, `COM4`, … | `COM3`, `COM4`, … |

An ESP32-S3 flashed over its native USB port needs no driver at all — it presents as a USB
CDC device. A board with a bridge chip does.

If nothing appears, the USB-serial driver is missing — [CP210x](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
for Silicon Labs bridges, [CH340](https://www.wch-ic.com/downloads/CH341SER_ZIP.html)
for WCH. A charge-only USB cable produces the same symptom, so try another cable
before chasing drivers.

On macOS, `arduino-cli board list` also lists Bluetooth ports and can show stale
`usbserial-*` entries with nothing behind them. To see what is genuinely plugged
in:

```bash
ioreg -p IOUSB -l -w 0 | grep "USB Product Name"
```

### Erase, then write

Erasing first avoids inheriting stale settings from whatever was on the board
before:

```bash
esptool --port /dev/cu.SLAB_USBtoUART erase-flash
```

**An ESP32 is not flashed like an ESP8266** — it needs a bootloader, a partition
table and an OTA selector alongside the application, at specific offsets. Writing
the application alone to `0x0` produces a board that does not boot.

Simplest — one pre-combined file at `0x0`. Same command for either chip, because
`merged.bin` already contains everything at the right places. It is a
whole-flash image, so it also blanks NVS: a first flash wants exactly that, an
upgrade that should keep the settings wants the four-file method below.

```bash
esptool --port /dev/cu.SLAB_USBtoUART --baud 460800 write-flash 0x0 ESP32-WOL-NTFY.ino.merged.bin
```

Faster — the four pieces at their offsets, writing ~1.15 MB instead of the whole flash.
**The bootloader offset differs by chip**, and getting it wrong produces a board that does
not boot:

ESP32-S3 — bootloader at `0x0`:

```bash
esptool --port /dev/cu.usbmodem101 --baud 460800 write-flash 0x0 ESP32-WOL-NTFY.ino.bootloader.bin 0x8000 ESP32-WOL-NTFY.ino.partitions.bin 0xe000 boot_app0.bin 0x10000 ESP32-WOL-NTFY.ino.bin
```

Original ESP32 — bootloader at `0x1000`:

```bash
esptool --port /dev/cu.SLAB_USBtoUART --baud 460800 write-flash 0x1000 ESP32-WOL-NTFY.ino.bootloader.bin 0x8000 ESP32-WOL-NTFY.ino.partitions.bin 0xe000 boot_app0.bin 0x10000 ESP32-WOL-NTFY.ino.bin
```

Those offsets are not guesswork — arduino-cli writes them to `flash_args` in each build
folder, which is the authoritative source if a future core version changes them:

```bash
cat ESP32-WOL-NTFY/build/esp32.esp32.esp32s3/flash_args
```

Older esptool (v4 and earlier) uses underscores: `erase_flash`, `write_flash`.

Some boards need the BOOT/GPIO0 button held during the first connect; most dev
boards auto-reset into the bootloader and need nothing.

### First boot

1. Power the board. **It must be on the same network segment as the computers you
   want to wake** — magic packets are broadcasts and do not cross subnets.
2. Join the open Wi-Fi network **`WOL-Setup-XXXX`** (suffix is the last two bytes
   of that board's MAC, so it differs per device).
3. The setup page should open automatically. If not, browse to
   **http://192.168.4.1**.
4. Fill in the form → **Save & restart**:

   | Section | Fields |
   | --- | --- |
   | Wi-Fi | network (picked from a live scan) and password — **2.4 GHz only** |
   | ntfy server | host, port, TLS on/off, topic |
   | ntfy login | username + password, or a token — blank for an open server |
   | Passphrases | command passphrase, and an admin password for the settings page |

The device joins the network and publishes a boot message to the topic containing
the address of its settings page:

```
WOL bot online
ip: 192.168.0.89
settings: http://192.168.0.89
0 computer(s) saved. Send "<passphrase> help" for commands.
```

That page stays available on the LAN afterwards, protected by the admin password
(username **`admin`**). Send `<passphrase> help` to the topic for the command
list — every command starts with the passphrase, and anything without it is
ignored silently.

The setup screen deliberately never times out — an unprovisioned device has no
credentials, so no Wi-Fi, no ntfy and no purpose. A page telling you how to fix
that is the most useful state available.

Watch progress on serial at **115200 baud** if anything goes wrong. A failed
Wi-Fi join prints a scan of every 2.4 GHz network in range and flags whether the
configured SSID is among them, which separates a typo from a 5 GHz-only network
from a wrong password.

### GUI alternatives

For people who would rather not use a terminal:

- [ESP Web Tools](https://esphome.github.io/esp-web-tools/) — flashes from a
  Chrome or Edge browser over Web Serial, no install at all. Point its manifest
  at `merged.bin` with an offset of `0`.
- [Espressif Flash Download Tool](https://www.espressif.com/en/support/download/other-tools)
  — official Windows GUI; enter the four files and offsets from the table above.

Both need the same erase-then-write behaviour.

---

## 5. Notes

- **Whether reflashing erases settings depends on which method you used.** NVS
  lives outside the application partition, at `0x9000`, so writing the
  application does not touch it: the four-file method above keeps the
  configuration and the saved computers, and so does writing
  `ESP32-WOL-NTFY.ino.bin` alone at `0x10000`. `merged.bin` is a whole-flash
  image — blank (`0xff`) everywhere the four pieces do not reach, NVS included —
  so writing it at `0x0` clears both, as does `erase-flash`. Use the one that
  matches what you want: the four-file method to upgrade in place, the merged
  image or `erase-flash` for a clean device.
- **This firmware has an in-device factory reset**, so returning a provisioned
  device to setup mode does not need USB:

  | Method | Effect |
  | --- | --- |
  | Press **EN**, release, then press and hold **BOOT** within 2 s | Opens the setup portal with no admin login. Nothing is erased; the form is pre-filled. |
  | `<passphrase> factoryreset` over ntfy | Clears configuration, keeps saved computers |
  | `<passphrase> factoryreset all` | Clears both |

  Press BOOT *after* letting go of EN — holding it through the reset puts the
  chip into its serial bootloader instead.

  The BOOT-button route is deliberately unauthenticated — physical access already
  implies control, since anyone holding the board can reflash it. It is the way
  back in from a forgotten admin password.
- **To wipe settings over USB without reflashing**, erase just the NVS partition:

  ```bash
  esptool --port /dev/cu.SLAB_USBtoUART erase-region 0x9000 0x5000
  ```

  That wipes the configuration *and* the saved computers, leaving the firmware in
  place. The offset is the `nvs` line in `partitions.csv` in the build folder;
  check it there rather than assuming, as it differs for other layouts.
- **Changing partition scheme requires writing the partition table**, which both
  flashing methods above do. `nvs` sits at `0x9000` in both the default and
  `huge_app` schemes, so the switch itself does not move your settings — but
  they only survive it if you take the four-file route, per the first note.
- **There is nothing to distribute alongside the binary.** Configuration exists
  only in NVS on each device, and no build step embeds it.
