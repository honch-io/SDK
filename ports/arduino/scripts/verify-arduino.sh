#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

HONCH_REQUIRE_ARDUINO_CLI=0

for arg in "$@"; do
  case "$arg" in
    --require-arduino-cli)
      HONCH_REQUIRE_ARDUINO_CLI=1
      ;;
    *)
      echo "unknown argument: $arg" >&2
      exit 2
      ;;
  esac
done

if [[ -n "${HONCH_ARDUINO_HOME:-}" ]]; then
  mkdir -p \
    "$HONCH_ARDUINO_HOME/data" \
    "$HONCH_ARDUINO_HOME/downloads" \
    "$HONCH_ARDUINO_HOME/user"
  export ARDUINO_DIRECTORIES_DATA="$HONCH_ARDUINO_HOME/data"
  export ARDUINO_DIRECTORIES_DOWNLOADS="$HONCH_ARDUINO_HOME/downloads"
  export ARDUINO_DIRECTORIES_USER="$HONCH_ARDUINO_HOME/user"
fi

cmake -S "$ROOT_DIR/ports/arduino/test/host" -B "$ROOT_DIR/build/arduino-host"
cmake --build "$ROOT_DIR/build/arduino-host"
"$ROOT_DIR/build/arduino-host/honch_arduino_wrapper_test"

if command -v arduino-cli >/dev/null 2>&1; then
  IFS=' ' read -r -a HONCH_ARDUINO_FQBNS <<< "${HONCH_ARDUINO_FQBNS:-esp32:esp32:esp32 esp32:esp32:esp32s2 esp32:esp32:esp32s3 esp32:esp32:esp32c3 esp32:esp32:esp32c6}"
  HONCH_ARDUINO_EXAMPLES=(
    "$ROOT_DIR/ports/arduino/examples/HonchBasic"
    "$ROOT_DIR/ports/arduino/examples/HonchOfflineQueue"
  )
  HONCH_ARDUINO_BUILD_ROOT="${HONCH_ARDUINO_BUILD_ROOT:-${HONCH_ARDUINO_HOME:-$ROOT_DIR/build/arduino-cli}}"
  mkdir -p "$HONCH_ARDUINO_BUILD_ROOT"

  for fqbn in "${HONCH_ARDUINO_FQBNS[@]}"; do
    for example in "${HONCH_ARDUINO_EXAMPLES[@]}"; do
      fqbn_slug="${fqbn//:/_}"
      example_slug="$(basename "$example")"
      arduino-cli compile \
        --fqbn "$fqbn" \
        --library "$ROOT_DIR/ports/arduino" \
        --build-path "$HONCH_ARDUINO_BUILD_ROOT/builds/$fqbn_slug/$example_slug" \
        "$example"
    done
  done
else
  if [[ "$HONCH_REQUIRE_ARDUINO_CLI" -eq 1 ]]; then
    echo "arduino-cli not found; install arduino-cli or omit --require-arduino-cli" >&2
    exit 1
  fi
  echo "arduino-cli not found; skipped Arduino example compile checks" >&2
fi
