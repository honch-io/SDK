#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

check_file() {
  local source_file="$1"
  local vendored_file="$2"

  if ! cmp -s "$source_file" "$vendored_file"; then
    echo "Arduino vendored core drift: $vendored_file differs from $source_file" >&2
    return 1
  fi
}

for source_file in "$ROOT_DIR"/core/src/honch_*.c "$ROOT_DIR"/core/src/honch_internal.h; do
  vendored_file="$ROOT_DIR/ports/arduino/src/$(basename "$source_file")"
  if [[ -f "$vendored_file" ]]; then
    check_file "$source_file" "$vendored_file"
  fi
done

for source_file in "$ROOT_DIR"/core/include/honch/core/*.h; do
  vendored_file="$ROOT_DIR/ports/arduino/src/honch/core/$(basename "$source_file")"
  check_file "$source_file" "$vendored_file"
done
