#!/usr/bin/env bash
#
# File: scripts/lib.sh
# Purpose: Shared helpers for the badge-fastboot build layer.
#
# Shared helpers for the badge-fastboot build layer.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/luckfox-pico}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out}"

# Default SDK location: the luckfox-pico checkout that contains this layer.
DEFAULT_SDK="$(cd "$ROOT_DIR/.." && pwd)"
LUCKFOX_SDK="${LUCKFOX_SDK:-$DEFAULT_SDK}"
SDK_BRANCH="${SDK_BRANCH:-main}"

C_RED=$'\e[31;1m'
C_GREEN=$'\e[32;1m'
C_YELLOW=$'\e[33;1m'
C_BLUE=$'\e[34;1m'
C_NORMAL=$'\e[0m'

msg_info()  { echo "${C_GREEN}[info]${C_NORMAL} $*"; }
msg_warn()  { echo "${C_YELLOW}[warn]${C_NORMAL} $*"; }
msg_error() { echo "${C_RED}[error]${C_NORMAL} $*" >&2; }
msg_step()  { echo; echo "${C_BLUE}==>${C_NORMAL} $*"; }

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        msg_error "Falta el comando '$1'."
        return 1
    }
}

require_sdk() {
    [ -d "$LUCKFOX_SDK" ] || {
        msg_error "No existe el SDK: $LUCKFOX_SDK"
        msg_info "Define LUCKFOX_SDK=/ruta/a/luckfox-pico"
        return 1
    }
    git -C "$LUCKFOX_SDK" rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
        msg_error "El SDK no es un repositorio git: $LUCKFOX_SDK"
        return 1
    }
}

sdk_git() {
    git -C "$LUCKFOX_SDK" "$@"
}

worktree_git() {
    git -C "$BUILD_DIR" "$@"
}

# Prefer an explicit ADB, then a local helper installation, then the host adb.
adb_bin() {
    if [ -n "${ADB:-}" ] && command -v "$ADB" >/dev/null 2>&1; then
        printf '%s\n' "$ADB"
        return 0
    fi
    if [ -x "$ROOT_DIR/.host-tools/adb-root/usr/bin/adb" ]; then
        printf '%s\n' "$ROOT_DIR/.host-tools/adb-root/usr/bin/adb"
        return 0
    fi
    command -v adb 2>/dev/null || return 1
}

# Add the bundled adb libraries only when they are present.
adb_ldpath() {
    local base="$ROOT_DIR/.host-tools/adb-root/usr/lib"
    [ -d "$base" ] || return 0
    printf '%s\n' \
        "$base/x86_64-linux-gnu/android:$base/x86_64-linux-gnu:$base"
}
