#!/usr/bin/env bash
#
# File: scripts/test.sh
# Purpose: Host unit tests for badge_photo modules and MTP configuration.
#
# Host unit tests for the pure badge_photo modules and MTP config checks.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

SDK="${LUCKFOX_SDK:-$ROOT_DIR/build/luckfox-pico}"
if [ ! -d "$SDK/project/make_meta/include" ]; then
    SDK="$(cd "$ROOT_DIR/.." && pwd)"
fi
if [ ! -d "$SDK/project/make_meta/include" ]; then
    msg_error "No se encontró el SDK. Define LUCKFOX_SDK o ejecuta prepare-sdk.sh."
    exit 1
fi

mkdir -p "$ROOT_DIR/build/tests"

msg_step "Compilando tests host"
cc -std=gnu11 -O2 -Wall -Wextra \
    -I"$ROOT_DIR/app/src" \
    -I"$SDK/project/make_meta/include" \
    "$ROOT_DIR/tests/test_all.c" \
    "$ROOT_DIR/app/src/config.c" \
    "$ROOT_DIR/app/src/photo_path.c" \
    "$ROOT_DIR/app/src/ws2812.c" \
    "$ROOT_DIR/app/src/led_engine.c" \
    "$ROOT_DIR/app/src/user_led.c" \
    "$ROOT_DIR/app/src/buttons.c" \
    "$ROOT_DIR/app/src/meta.c" \
    "$ROOT_DIR/app/src/log.c" \
    -o "$ROOT_DIR/build/tests/test_all"

msg_step "Ejecutando tests C"
"$ROOT_DIR/build/tests/test_all"

msg_step "Validando configuración MTP"
chmod +x "$ROOT_DIR/tests/test_mtp_conf.sh"
"$ROOT_DIR/tests/test_mtp_conf.sh"

msg_info "Tests host completados."
