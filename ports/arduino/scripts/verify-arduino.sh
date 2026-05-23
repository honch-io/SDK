#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

cmake -S "$ROOT_DIR/ports/arduino/test/host" -B "$ROOT_DIR/build/arduino-host"
cmake --build "$ROOT_DIR/build/arduino-host"
"$ROOT_DIR/build/arduino-host/honch_arduino_wrapper_test"

if command -v arduino-cli >/dev/null 2>&1; then
  arduino-cli compile --fqbn esp32:esp32:esp32 --library "$ROOT_DIR/ports/arduino" "$ROOT_DIR/ports/arduino/examples/HonchBasic"
  arduino-cli compile --fqbn esp32:esp32:esp32 --library "$ROOT_DIR/ports/arduino" "$ROOT_DIR/ports/arduino/examples/HonchOfflineQueue"
else
  echo "arduino-cli not found; skipped Arduino example compile checks" >&2
fi
