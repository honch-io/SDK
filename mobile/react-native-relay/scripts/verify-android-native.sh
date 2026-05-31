#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="$ROOT_DIR/android"
GRADLEW="$ROOT_DIR/node_modules/@react-native/gradle-plugin/gradlew"

if [[ ! -x "$GRADLEW" ]]; then
  echo "Android native verification failed: React Native Gradle wrapper is missing." >&2
  echo "Run 'bun install' in $ROOT_DIR, then retry." >&2
  exit 1
fi

if [[ -z "${ANDROID_HOME:-}" && -z "${ANDROID_SDK_ROOT:-}" && ! -f "$ANDROID_DIR/local.properties" ]]; then
  echo "Android native verification failed: Android SDK location is not configured." >&2
  echo "Set ANDROID_HOME, set ANDROID_SDK_ROOT, or create android/local.properties with sdk.dir=/path/to/sdk." >&2
  exit 1
fi

"$GRADLEW" -p "$ANDROID_DIR" assembleDebug
