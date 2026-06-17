#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

# Arduino-only vendored sources that legitimately have no core/ counterpart.
ARDUINO_ONLY_C=("honch_tiered_queue.c")

fail() {
  echo "Arduino vendored core sync: $1" >&2
  exit 1
}

check_identical() {
  local source_file="$1"
  local vendored_file="$2"
  [[ -f "$vendored_file" ]] || fail "missing vendored copy $vendored_file (must mirror $source_file)"
  cmp -s "$source_file" "$vendored_file" || fail "$vendored_file differs from $source_file"
}

is_arduino_only_c() {
  local name="$1"
  local entry
  for entry in "${ARDUINO_ONLY_C[@]}"; do
    [[ "$name" == "$entry" ]] && return 0
  done
  return 1
}

# Forward: every core source must have a byte-identical vendored copy.
for source_file in "$ROOT_DIR"/core/src/honch_*.c "$ROOT_DIR"/core/src/honch_internal.h; do
  check_identical "$source_file" "$ROOT_DIR/ports/arduino/src/$(basename "$source_file")"
done
for source_file in "$ROOT_DIR"/core/include/honch/core/*.h; do
  check_identical "$source_file" "$ROOT_DIR/ports/arduino/src/honch/core/$(basename "$source_file")"
done

# Reverse: every vendored copy must correspond to a core source, so a core file
# that was renamed or deleted cannot leave a stale vendored copy shipping.
for vendored_file in "$ROOT_DIR"/ports/arduino/src/honch_*.c; do
  name="$(basename "$vendored_file")"
  is_arduino_only_c "$name" && continue
  [[ -f "$ROOT_DIR/core/src/$name" ]] || \
    fail "orphan vendored source $vendored_file (no core/src/$name; add it to ARDUINO_ONLY_C if intentionally arduino-only)"
done
for vendored_file in "$ROOT_DIR"/ports/arduino/src/honch/core/*.h; do
  name="$(basename "$vendored_file")"
  [[ -f "$ROOT_DIR/core/include/honch/core/$name" ]] || \
    fail "orphan vendored header $vendored_file (no core/include/honch/core/$name)"
done
