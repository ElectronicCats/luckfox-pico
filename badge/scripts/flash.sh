#!/usr/bin/env bash
#
# File: scripts/flash.sh
# Purpose: Flash update.img with the Rockchip upgrade tool.
#
# Flash update.img with the Rockchip upgrade tool.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

SDK="$BUILD_DIR"
IMG="${1:-$OUT_DIR/update.img}"
TOOL="$SDK/tools/linux/Linux_Upgrade_Tool/upgrade_tool"

if [ ! -f "$IMG" ]; then
    IMG="$SDK/output/image/update.img"
fi
[ -f "$IMG" ] || { msg_error "No se encontró update.img"; exit 1; }
[ -x "$TOOL" ] || { msg_error "No se encontró upgrade_tool en $TOOL"; exit 1; }

msg_step "Dispositivos Rockchip"
"$TOOL" LD || true

cat <<EOF

Para entrar en maskrom:
  1. Mantén presionado BOOT.
  2. Pulsa y suelta RST.
  3. Sigue sosteniendo BOOT ~5 s hasta que el host detecte maskrom.
  4. Suelta BOOT.

Se flasheará: $IMG
Usa SIEMPRE UF (no DI -b).

EOF

if [ "$(id -u)" -eq 0 ]; then
    "$TOOL" UF "$IMG"
else
    sudo "$TOOL" UF "$IMG"
fi
