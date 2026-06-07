#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -f "${ROOT_DIR}/HonchReactNativeRelay.podspec" ]] || find "${ROOT_DIR}/ios" -type f 2>/dev/null | grep -q .; then
  echo "iOS native verification failed: react-native-relay should not autolink SDK-owned BLE code." >&2
  exit 1
fi

echo "react-native-relay has no iOS native module; host apps own Bluetooth integration."
