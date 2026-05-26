#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
SDK_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)"
MICROPYTHON_BIN="${MICROPYTHON_BIN:-micropython}"
HONCH_MICROPYTHON_QUEUE_DIR="${HONCH_MICROPYTHON_QUEUE_DIR:-/tmp/honch-micropython-runtime-smoke}"

"$MICROPYTHON_BIN" -c 'import _honch_core'

MICROPYPATH="$SDK_ROOT/ports/micropython" \
HONCH_MICROPYTHON_QUEUE_DIR="$HONCH_MICROPYTHON_QUEUE_DIR" \
    "$MICROPYTHON_BIN" "$SDK_ROOT/ports/micropython/tests/runtime_smoke.py"
