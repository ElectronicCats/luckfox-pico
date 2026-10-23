#!/usr/bin/env bash
#
# File: scripts/build.sh
# Purpose: Build the badge image from the prepared SDK worktree.
#
# Build the badge image from the prepared SDK worktree.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

usage() {
    cat <<EOF
Uso: $(basename "$0") [etapa] [opciones]

Etapas:
  all       Build completo (default)
  prepare   Solo preparar SDK/worktree y capa
  check     Chequear herramientas de host
  media     Librerías de medios
  uboot     U-Boot + MCU
  kernel    Kernel + DTB
  rootfs    Rootfs
  app       Aplicaciones
  pack      Empaquetar update.img
  firmware  Igual que pack
  clean     Limpiar artefactos del SDK

Opciones:
  --sdk PATH       Ruta al SDK base
  --build PATH     Ruta del worktree
  --no-prepare     No recrear/aplicar la capa
  --jobs N         Paralelismo (default: nproc/2+1)
  -h, --help       Esta ayuda
EOF
}

STEP="all"
while [ $# -gt 0 ]; do
    case "$1" in
        all|prepare|check|media|uboot|kernel|rootfs|app|pack|firmware|clean)
            STEP="$1"; shift ;;
        --sdk) LUCKFOX_SDK="$2"; shift 2 ;;
        --build) BUILD_DIR="$2"; shift 2 ;;
        --no-prepare) NO_PREPARE=1; shift ;;
        --jobs) RK_JOBS="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) msg_error "Opción desconocida: $1"; usage; exit 1 ;;
    esac
done

export RK_JOBS="${RK_JOBS:-$(( $(nproc) / 2 + 1 ))}"
export LC_ALL=C

if [ "$STEP" = "check" ]; then
    exec "$SCRIPT_DIR/check-host.sh"
fi

if [ "${NO_PREPARE:-0}" -eq 0 ]; then
    "$SCRIPT_DIR/prepare-sdk.sh" --sdk "$LUCKFOX_SDK" --build "$BUILD_DIR"
fi

[ -d "$BUILD_DIR" ] || { msg_error "No existe el worktree: $BUILD_DIR"; exit 1; }

TOOLCHAIN="$BUILD_DIR/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin"
if [ -d "$TOOLCHAIN" ]; then
    export PATH="$TOOLCHAIN:$PATH"
    msg_info "Toolchain ARM: $TOOLCHAIN"
else
    msg_warn "No se encontró el toolchain ARM en $TOOLCHAIN"
fi

# RISC-V toolchain for the HPMCU. The SDK downloads it into
# sysdrv/source/mcu/prebuilts on the first build.
RTT_PREBUILT="$BUILD_DIR/sysdrv/source/mcu/prebuilts/gcc/linux-x86/riscv64/xpack-riscv-none-embed-gcc-10.2.0-1.2/bin"
if [ -d "$RTT_PREBUILT" ]; then
    export RTT_EXEC_PATH="$RTT_PREBUILT"
    export PATH="$RTT_EXEC_PATH:$PATH"
    msg_info "Toolchain RISC-V: $RTT_EXEC_PATH"
else
    msg_info "Toolchain RISC-V: el SDK lo descargará en el primer build"
fi

# Local wrappers (cmake 4 + legacy Rockchip projects).
if [ -d "$ROOT_DIR/scripts/host-tools" ]; then
    export PATH="$ROOT_DIR/scripts/host-tools:$PATH"
fi

# Local tools without sudo, when available.
if [ -d "$ROOT_DIR/.host-tools/bin" ]; then
    export PATH="$ROOT_DIR/.host-tools/bin:$PATH"
    export LD_LIBRARY_PATH="$ROOT_DIR/.host-tools/usr/lib/x86_64-linux-gnu:$ROOT_DIR/.host-tools/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
    if [ -d "$ROOT_DIR/.host-tools/usr/share/bison" ]; then
        export BISON_PKGDATADIR="$ROOT_DIR/.host-tools/usr/share/bison"
    fi
    if [ -x "$ROOT_DIR/.host-tools/bin/m4" ]; then
        export M4="$ROOT_DIR/.host-tools/bin/m4"
    fi
    msg_info "Herramientas locales: $ROOT_DIR/.host-tools/bin"
fi

mkdir -p "$OUT_DIR"
cd "$BUILD_DIR"

case "$STEP" in
    prepare)
        msg_info "Preparación completa."
        exit 0
        ;;
    clean)
        msg_step "Limpiando SDK"
        ./build.sh clean
        exit 0
        ;;
    all)
        msg_step "Build completo (puede tardar)"
        ./build.sh
        ;;
    firmware|pack)
        msg_step "Empaquetando firmware"
        ./build.sh firmware
        ;;
    *)
        msg_step "Build: $STEP"
        ./build.sh "$STEP"
        ;;
esac

if [ -f "$BUILD_DIR/output/image/update.img" ]; then
    mkdir -p "$OUT_DIR"
    cp -f "$BUILD_DIR/output/image/update.img" "$OUT_DIR/update.img"
    msg_info "Imagen: $OUT_DIR/update.img"
    ls -lh "$OUT_DIR/update.img"
fi
