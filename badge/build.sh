#!/usr/bin/env bash
#
# File: build.sh
# Purpose: Convenience entry point for the badge build layer.
#
# Builds inside badge/build/luckfox-pico using this checkout as the SDK source.
# Any argument is forwarded to scripts/build.sh (all, app, kernel, pack, ...).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SDK_DIR="${LUCKFOX_SDK:-$(cd "$ROOT_DIR/.." && pwd)}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/luckfox-pico}"

exec "$ROOT_DIR/scripts/build.sh" \
    --sdk "$SDK_DIR" \
    --build "$BUILD_DIR" \
    "$@"
