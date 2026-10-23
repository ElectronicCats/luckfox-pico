#!/usr/bin/env bash
#
# File: scripts/check-host.sh
# Purpose: Check the host tools required by the Luckfox SDK build.
#
# Check host tools needed by the Luckfox SDK build.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

REQUIRED=(git make gcc g++ bc cpio rsync fakeroot dtc mkimage python3 scons file tar gzip bzip2 xz)
OPTIONAL=(bison flex texinfo gperf autoconf libtool unzip pkg-config mkfs.erofs mkfs.ubifs ubinize)

missing_required=0
missing_optional=0

echo "Herramientas requeridas:"
for cmd in "${REQUIRED[@]}"; do
    if command -v "$cmd" >/dev/null 2>&1; then
        printf "  [ok]      %-18s %s\n" "$cmd" "$(command -v "$cmd")"
    else
        printf "  [FALTA]   %-18s\n" "$cmd"
        missing_required=$((missing_required + 1))
    fi
done

echo
echo "Herramientas opcionales (el SDK puede compilarlas):"
for cmd in "${OPTIONAL[@]}"; do
    if command -v "$cmd" >/dev/null 2>&1; then
        printf "  [ok]      %-18s %s\n" "$cmd" "$(command -v "$cmd")"
    else
        printf "  [falta]   %-18s\n" "$cmd"
        missing_optional=$((missing_optional + 1))
    fi
done

echo
if [ "$missing_required" -gt 0 ]; then
    msg_error "Faltan $missing_required herramientas requeridas."
    msg_info "En Ubuntu/Debian: sudo apt-get install -y git make gcc g++ bc cpio rsync fakeroot device-tree-compiler u-boot-tools python3 scons"
    exit 1
fi

if [ "$missing_optional" -gt 0 ]; then
    msg_warn "Faltan $missing_optional herramientas opcionales."
    msg_info "Si el build falla, instala: sudo apt-get install -y bison flex texinfo gperf autoconf libtool unzip"
    msg_info "Alternativa sin sudo: ./scripts/install-host-tools.sh"
fi

# gcc -m32 is no longer required: the layer removes -m32 from make_meta.
msg_info "Chequeo de host completado."
