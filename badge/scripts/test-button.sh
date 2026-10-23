#!/usr/bin/env bash
#
# File: scripts/test-button.sh
# Purpose: Physical top-button capture test (requires human interaction).
#
# Physical top capture-button test (GPIO1_D3). This is the only test that
# needs a human action: press the button when the script asks for it.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

ADB="$(adb_bin)" || { msg_error "adb no encontrado. Instala adb o define ADB=/ruta/adb."; exit 1; }
ADB_LIB="$(adb_ldpath)"
[ -n "$ADB_LIB" ] && export LD_LIBRARY_PATH="$ADB_LIB:${LD_LIBRARY_PATH:-}"

PHOTOS_DIR="/userdata/photos"
TIMEOUT="${BUTTON_TIMEOUT:-30}"
WORK_DIR="$ROOT_DIR/out/button-test-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$WORK_DIR"

adb_shell() { "$ADB" shell "$@"; }
wait_adb() {
    local i
    for i in $(seq 1 120); do
        "$ADB" get-state 2>/dev/null | grep -q device && return 0
        sleep 0.5
    done
    return 1
}

if ! command -v "$ADB" >/dev/null 2>&1; then
    msg_error "adb no encontrado: $ADB"
    exit 1
fi
if ! wait_adb; then
    msg_error "La placa no aparece por ADB."
    exit 1
fi

if ! adb_shell 'grep -q "gpio-keys" /proc/bus/input/devices'; then
    msg_error "gpio-keys no está registrado en el kernel."
    exit 1
fi

PID=$(adb_shell "ps | grep '[b]adge_photo' | awk '{print \$1}' | head -1" 2>/dev/null | tr -d '\r' || true)
if [ -z "$PID" ]; then
    msg_error "badge_photo no está corriendo; arranca la app o usa test-hardware.sh."
    exit 1
fi

BEFORE=$(adb_shell "ls $PHOTOS_DIR/*.jpg 2>/dev/null | wc -l" | tr -d '\r')
LOG_BEFORE=$(adb_shell "wc -l < /tmp/badge_photo.log 2>/dev/null || echo 0" | tr -d '\r')
msg_step "Pulsa el botón superior de captura (GPIO1_D3) una vez"
echo "  Esperando hasta ${TIMEOUT}s una foto nueva (antes: $BEFORE)..."
DEADLINE=$((SECONDS + TIMEOUT))
AFTER="$BEFORE"
while [ "$SECONDS" -lt "$DEADLINE" ]; do
    AFTER=$(adb_shell "ls $PHOTOS_DIR/*.jpg 2>/dev/null | wc -l" | tr -d '\r')
    [ "$AFTER" -gt "$BEFORE" ] && break
    sleep 0.5
done

if [ "$AFTER" -le "$BEFORE" ]; then
    msg_error "No apareció ninguna foto. ¿Se pulsó el botón?"
    adb_shell "tail -30 /tmp/badge_photo.log" > "$WORK_DIR/badge_photo.log" 2>&1 || true
    exit 1
fi

NEWEST=$(adb_shell "grep 'captura:' /tmp/badge_photo.log | tail -1 | sed 's/.*captura: \\([^ ]*\\).*/\\1/'" | tr -d '\r')
if [ -z "$NEWEST" ]; then
    msg_error "No se pudo identificar la foto nueva en el log."
    exit 1
fi
NAME=$(basename "$NEWEST")
if ! "$ADB" pull "$NEWEST" "$WORK_DIR/$NAME" >/dev/null 2>&1; then
    msg_error "La foto apareció pero no se pudo descargar: $NEWEST"
    exit 1
fi

if ! python3 - "$WORK_DIR/$NAME" <<'PY'
import os, sys
from PIL import Image
path = sys.argv[1]
im = Image.open(path)
im.load()
if im.size != (2304, 1296):
    print(f"tamaño inesperado: {im.size}")
    sys.exit(1)
if os.path.getsize(path) < 10000:
    print(f"archivo demasiado pequeño: {os.path.getsize(path)}")
    sys.exit(1)
print(f"JPEG válido: {im.size}, {os.path.getsize(path)} bytes")
PY
then
    msg_error "La foto nueva no es un JPEG válido."
    exit 1
fi

adb_shell "tail -n +$((LOG_BEFORE + 1)) /tmp/badge_photo.log" > "$WORK_DIR/badge_photo.log" 2>&1 || true
if grep -q "captura:" "$WORK_DIR/badge_photo.log"; then
    msg_info "Botón físico OK: $NEWEST"
else
    msg_error "La foto apareció, pero el log no muestra la captura."
    exit 1
fi
echo "  Log: $WORK_DIR/badge_photo.log"
