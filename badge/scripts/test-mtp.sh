#!/usr/bin/env bash
#
# File: scripts/test-mtp.sh
# Purpose: Host-side MTP smoke test for the Badge Hack GDL 2026.
#
# Host-side MTP smoke test for the Badge Hack GDL 2026.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

need_any=0
for cmd in mtp-detect jmtpfs gio; do
    if command -v "$cmd" >/dev/null 2>&1; then
        need_any=1
        break
    fi
done

if [ "$need_any" -eq 0 ]; then
    msg_warn "No hay herramientas MTP instaladas."
    msg_info "Linux: sudo apt-get install libmtp-runtime libmtp-common jmtpfs gvfs-backends"
    exit 0
fi

echo "== USB =="
lsusb | grep -i "2207:0019" || msg_warn "No se ve 2207:0019 en lsusb"

if command -v mtp-detect >/dev/null 2>&1; then
    echo
    echo "== mtp-detect =="
    if mtp-detect 2>&1 | tee /tmp/badge-mtp-detect.log | grep -q "Badge Hack GDL 2026"; then
        msg_info "mtp-detect encontró la badge"
    else
        msg_warn "mtp-detect no encontró el producto esperado"
    fi
    grep -E "Manufacturer|Model|Serial|Storage|Description" /tmp/badge-mtp-detect.log | head -30 || true
fi

if command -v jmtpfs >/dev/null 2>&1; then
    echo
    echo "== jmtpfs =="
    mnt=$(mktemp -d /tmp/badge-mtp.XXXXXX)
    if jmtpfs -o allow_other "$mnt" 2>/tmp/badge-jmtpfs.log; then
        echo "Montado en $mnt"
        find "$mnt" -maxdepth 2 -type f -printf '%p %s bytes\n' | head -20 || true
        first=$(find "$mnt" -maxdepth 2 -type f -iname '*.jpg' | head -1 || true)
        if [ -n "$first" ]; then
            sha256sum "$first"
            cp "$first" /tmp/badge-mtp-sample.jpg
            msg_info "Copia de prueba: /tmp/badge-mtp-sample.jpg"
        else
            msg_warn "No se encontraron JPEG en el storage MTP"
        fi
        fusermount -u "$mnt" 2>/dev/null || umount "$mnt" 2>/dev/null || true
    else
        msg_warn "jmtpfs no pudo montar; ver /tmp/badge-jmtpfs.log"
    fi
    rmdir "$mnt" 2>/dev/null || true
fi

if command -v gio >/dev/null 2>&1; then
    echo
    echo "== gio =="
    gio mount -l 2>/dev/null | grep -i -A4 "mtp\|Badge" || true
fi

echo
msg_info "Test MTP terminado."
