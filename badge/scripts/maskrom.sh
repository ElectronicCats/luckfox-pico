#!/usr/bin/env bash
#
# File: scripts/maskrom.sh
# Purpose: Reboot the badge from Linux/ADB into Rockchip maskrom.
#
# Primary path writes the Rockchip loader magic to the boot-mode register and
# temporarily unbinds syscon-reboot-mode so the reset does not overwrite it.
# Falls back to the BusyBox `reboot loader` applet when needed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

ADB="$(adb_bin)" || { msg_error "adb no encontrado. Instala adb o define ADB=/ruta/adb."; exit 1; }
ADB_LIB="$(adb_ldpath)"
[ -n "$ADB_LIB" ] && export LD_LIBRARY_PATH="$ADB_LIB:${LD_LIBRARY_PATH:-}"

msg_step "Esperando ADB"
for _ in $(seq 1 120); do
    "$ADB" get-state 2>/dev/null | grep -q device && break
    sleep 0.5
done
if ! "$ADB" get-state 2>/dev/null | grep -q device; then
    msg_error "La placa no aparece por ADB."
    exit 1
fi

BOOT_MODE_REG=0xff020200
LOADER_MAGIC=0x5242c301
REBOOT_MODE_UNBIND=/sys/bus/platform/drivers/syscon-reboot-mode/unbind

msg_step "Entrando a maskrom por registro de boot"
"$ADB" shell sync || true

# The kernel reboot-mode driver normally overwrites this register on reboot.
# Unbind it for this boot so the loader magic survives the reset. If the
# register or devmem is unavailable, fall back to the legacy BusyBox applet.
if "$ADB" shell "test -e $REBOOT_MODE_UNBIND && command -v devmem >/dev/null"; then
    "$ADB" shell "devmem $BOOT_MODE_REG 32 $LOADER_MAGIC; echo ff000000.syscon:reboot-mode > $REBOOT_MODE_UNBIND; reboot -f" || true
else
    "$ADB" shell reboot loader || true
fi

msg_info "Esperando maskrom (2207:110c / 2207:350a)..."
for _ in $(seq 1 30); do
    if lsusb 2>/dev/null | grep -qE '2207:(110c|350a)'; then
        msg_info "Maskrom detectado. Flashea con: sudo ./scripts/flash.sh out/update.img"
        exit 0
    fi
    sleep 0.5
done
msg_warn "No se detectó maskrom automáticamente; revisa lsusb o usa BOOT+RST."
