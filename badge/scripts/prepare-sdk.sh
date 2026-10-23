#!/usr/bin/env bash
#
# File: scripts/prepare-sdk.sh
# Purpose: Create or reuse a git worktree of the Luckfox SDK and apply this layer.
#
# Create (or reuse) a git worktree of the Luckfox SDK and apply this layer.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

usage() {
    cat <<EOF
Uso: $(basename "$0") [opciones]

Opciones:
  --sdk PATH        Ruta al SDK luckfox-pico (default: $LUCKFOX_SDK)
  --build PATH      Ruta del worktree (default: $BUILD_DIR)
  --branch NAME     Rama/tag base (default: $SDK_BRANCH)
  --no-apply        Solo crear el worktree, no aplicar la capa
  -h, --help        Esta ayuda
EOF
}

APPLY=1
while [ $# -gt 0 ]; do
    case "$1" in
        --sdk) LUCKFOX_SDK="$2"; shift 2 ;;
        --build) BUILD_DIR="$2"; shift 2 ;;
        --branch) SDK_BRANCH="$2"; shift 2 ;;
        --no-apply) APPLY=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) msg_error "Opción desconocida: $1"; usage; exit 1 ;;
    esac
done

require_cmd git
require_sdk
require_cmd rsync

mkdir -p "$ROOT_DIR/build"

if [ ! -d "$BUILD_DIR/.git" ] && [ ! -f "$BUILD_DIR/.git" ]; then
    msg_step "Creando worktree del SDK en $BUILD_DIR"
    sdk_git worktree add --detach "$BUILD_DIR" "$SDK_BRANCH"
else
    msg_info "Worktree ya existe: $BUILD_DIR"
    worktree_git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
        msg_error "$BUILD_DIR existe pero no es un worktree git válido."
        exit 1
    }
fi

msg_info "SDK base: $LUCKFOX_SDK"
msg_info "Worktree: $BUILD_DIR"
msg_info "HEAD: $(worktree_git rev-parse --short HEAD) $(worktree_git log -1 --pretty=%s)"

if [ "$APPLY" -eq 1 ]; then
    "$SCRIPT_DIR/apply-layer.sh" --sdk "$BUILD_DIR"
fi

msg_step "Listo"
