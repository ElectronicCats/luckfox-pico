#!/usr/bin/env bash
#
# File: scripts/install-host-tools.sh
# Purpose: Install missing host tools into .host-tools/ without sudo.
#
# Install missing host tools into .host-tools/ without sudo.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

TOOLS_DIR="$ROOT_DIR/.host-tools"
DEB_DIR="$TOOLS_DIR/debs"
mkdir -p "$DEB_DIR" "$TOOLS_DIR/bin"

PACKAGES=(m4 bison flex libfl2)

need_download=0
for pkg in "${PACKAGES[@]}"; do
    if ! ls "$DEB_DIR/${pkg}"_*.deb >/dev/null 2>&1; then
        need_download=1
    fi
done

if [ "$need_download" -eq 1 ]; then
    require_cmd apt-get || { msg_error "apt-get no está disponible"; exit 1; }
    msg_step "Descargando paquetes en $DEB_DIR"
    (cd "$DEB_DIR" && apt-get download "${PACKAGES[@]}")
fi

msg_step "Extrayendo paquetes"
for deb in "$DEB_DIR"/*.deb; do
    [ -f "$deb" ] || continue
    dpkg-deb -x "$deb" "$TOOLS_DIR"
done

msg_step "Creando enlaces en $TOOLS_DIR/bin"
for d in "$TOOLS_DIR"/usr/bin "$TOOLS_DIR"/bin; do
    [ -d "$d" ] || continue
    find "$d" -maxdepth 1 -type f -executable -printf '%f\n' 2>/dev/null | while read -r name; do
        ln -sfn "$d/$name" "$TOOLS_DIR/bin/$name"
    done
done

# Libraries required by the extracted binaries.
for libdir in "$TOOLS_DIR"/usr/lib/x86_64-linux-gnu "$TOOLS_DIR"/lib/x86_64-linux-gnu; do
    [ -d "$libdir" ] || continue
    find "$libdir" -maxdepth 1 -type f -name '*.so*' -printf '%f\n' 2>/dev/null | while read -r name; do
        ln -sfn "$libdir/$name" "$TOOLS_DIR/bin/$name"
    done
done

msg_info "Herramientas locales listas en $TOOLS_DIR"
msg_info "Uso: export PATH=$TOOLS_DIR/bin:\$PATH"
msg_info "     export LD_LIBRARY_PATH=$TOOLS_DIR/usr/lib/x86_64-linux-gnu:$TOOLS_DIR/lib/x86_64-linux-gnu:\$LD_LIBRARY_PATH"
"$TOOLS_DIR/bin/m4" --version 2>/dev/null | head -1 || true
"$TOOLS_DIR/bin/bison" --version 2>/dev/null | head -1 || true
"$TOOLS_DIR/bin/flex" --version 2>/dev/null | head -1 || true
