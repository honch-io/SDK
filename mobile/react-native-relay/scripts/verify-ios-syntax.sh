#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_PATH="$(xcrun --sdk iphoneos --show-sdk-path)"
HEADER_ROOT="${TMPDIR:-/tmp}/honch-rn-relay-headers"

rm -rf "${HEADER_ROOT}"
mkdir -p "${HEADER_ROOT}/React" "${HEADER_ROOT}/RCTDeprecation"

find "${ROOT_DIR}/node_modules/react-native/React" -name '*.h' -exec ln -sf {} "${HEADER_ROOT}/React/" \;
ln -sf \
  "${ROOT_DIR}/node_modules/react-native/ReactApple/Libraries/RCTFoundation/RCTDeprecation/Exported/RCTDeprecation.h" \
  "${HEADER_ROOT}/RCTDeprecation/RCTDeprecation.h"

xcrun --sdk iphoneos clang \
  -fsyntax-only \
  -ObjC \
  -fobjc-arc \
  -isysroot "${SDK_PATH}" \
  -miphoneos-version-min=13.0 \
  -I "${HEADER_ROOT}" \
  -I "${ROOT_DIR}/node_modules/react-native/React" \
  -I "${ROOT_DIR}/node_modules/react-native/React/Base" \
  -I "${ROOT_DIR}/node_modules/react-native/React/Modules" \
  -I "${ROOT_DIR}/node_modules/react-native/ReactCommon" \
  -I "${ROOT_DIR}/node_modules/react-native/ReactCommon/react/nativemodule/core" \
  "${ROOT_DIR}/ios/HonchReactNativeRelay.m"
