#!/usr/bin/env bash
# Compiles the sketch's pure logic (store.cpp, commands.cpp) natively and runs
# the assertions. No board required.
#
# The real source files are compiled in -- nothing is copied or reimplemented --
# against the small Arduino shim in shim/. The transport (ntfy) and the radio
# (wol) are stubbed inside test_commands.cpp so replies can be inspected.
#
#     ./test/run.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH="$HERE/../ESP32-WOL-NTFY"
JSON="${ARDUINOJSON_SRC:-$HOME/Documents/Arduino/libraries/ArduinoJson/src}"

if [ ! -d "$JSON" ]; then
  echo "ArduinoJson not found at $JSON" >&2
  echo "Set ARDUINOJSON_SRC to its src/ directory." >&2
  exit 1
fi

if [ ! -f "$SKETCH/secrets.h" ]; then
  echo "secrets.h missing -- run: cp $SKETCH/secrets.h.example $SKETCH/secrets.h" >&2
  exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

FLAGS=(
  -std=gnu++17 -w
  -DARDUINOJSON_ENABLE_ARDUINO_STRING=1
  -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0
  -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0
  -DARDUINOJSON_ENABLE_PROGMEM=0
  -DARDUINOJSON_ENABLE_STD_STRING=0
  -I "$HERE/shim" -I "$SKETCH" -I "$JSON"
)

echo "=== store ==="
g++ "${FLAGS[@]}" -o "$OUT/test_store" "$HERE/test_store.cpp" "$SKETCH/store.cpp"
"$OUT/test_store"

echo
echo "=== commands ==="
g++ "${FLAGS[@]}" -o "$OUT/test_commands" \
    "$HERE/test_commands.cpp" "$SKETCH/commands.cpp" "$SKETCH/store.cpp"
"$OUT/test_commands"
