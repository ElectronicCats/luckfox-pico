#!/usr/bin/env bash
#
# File: scripts/stress-hardware.sh
# Purpose: Stress, live-config and long-run persistence tests for badge_photo.
#
# Usage:
#   ./scripts/stress-hardware.sh [--captures N] [--reboots N]
#                                [--config-cycles N] [--soak-minutes M]
#
# Defaults: 100 captures, 10 reboots, 50 config cycles, 0 soak minutes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

ADB="$(adb_bin)" || { msg_error "adb no encontrado. Instala adb o define ADB=/ruta/adb."; exit 1; }
ADB_LIB="$(adb_ldpath)"
[ -n "$ADB_LIB" ] && export LD_LIBRARY_PATH="$ADB_LIB:${LD_LIBRARY_PATH:-}"

CAPTURES=100
REBOOTS=10
CONFIG_CYCLES=50
SOAK_MINUTES=0
KEEP_LAST=0
PHOTOS_DIR="/userdata/photos"
CONFIG_FILE="/userdata/badge/overlay.ini"
SERIAL="94a38310ac09f19f"
WORK_DIR="$ROOT_DIR/out/stress-$(date +%Y%m%d-%H%M%S)"
PASS=0
FAIL=0

usage() {
    cat <<EOF
Uso: $(basename "$0") [opciones]

  --captures N       capturas de estrés (default $CAPTURES)
  --reboots N        ciclos de reboot (default $REBOOTS)
  --config-cycles N  cambios de overlay.ini (default $CONFIG_CYCLES)
  --soak-minutes M   minutos de soak test (default $SOAK_MINUTES)
  --keep-last N      conservar solo las N fotos mas nuevas (0 = todas)
  -h, --help         esta ayuda
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --captures) CAPTURES="$2"; shift 2 ;;
        --reboots) REBOOTS="$2"; shift 2 ;;
        --config-cycles) CONFIG_CYCLES="$2"; shift 2 ;;
        --soak-minutes) SOAK_MINUTES="$2"; shift 2 ;;
        --keep-last) KEEP_LAST="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) msg_error "Opción desconocida: $1"; usage; exit 1 ;;
    esac
done

mkdir -p "$WORK_DIR/configs"
exec > >(tee "$WORK_DIR/stress.log") 2>&1

pass() { PASS=$((PASS + 1)); echo "  [PASS] $*"; }
fail() { FAIL=$((FAIL + 1)); echo "  [FAIL] $*"; }
step() { msg_step "$*"; }

adb_shell() { "$ADB" shell "$@"; }

wait_adb() {
    local i
    for i in $(seq 1 120); do
        if "$ADB" get-state 2>/dev/null | grep -q device; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_ready() {
    local i pid rpid
    for i in $(seq 1 180); do
        pid=$(adb_shell "ps | grep '[b]adge_photo' | awk '{print \$1}' | head -1" 2>/dev/null | tr -d '\r' || true)
        rpid=$(adb_shell 'cat /tmp/badge_photo.ready 2>/dev/null' 2>/dev/null | tr -d '\r' || true)
        if [ -n "$pid" ] && [ "$rpid" = "$pid" ]; then
            echo "$pid"
            return 0
        fi
        sleep 1
    done
    return 1
}

photos_count() {
    adb_shell "ls $PHOTOS_DIR/*.jpg 2>/dev/null | wc -l" | tr -d '\r'
}

capture_once() {
    local pid="$1"
    adb_shell "kill -USR1 $pid" >/dev/null 2>&1 || return 1
    sleep 1.2
    return 0
}

rotate_photos() {
    local count oldest
    [ "$KEEP_LAST" -gt 0 ] || return 0
    count=$(photos_count)
    while [ "$count" -gt "$KEEP_LAST" ]; do
        oldest=$(adb_shell "ls $PHOTOS_DIR/*.jpg 2>/dev/null | sort | head -n 1" | tr -d '\r' || true)
        [ -n "$oldest" ] || break
        adb_shell "rm -f '$oldest'" >/dev/null 2>&1 || true
        count=$((count - 1))
    done
}

fd_count() {
    adb_shell "ls /proc/$1/fd 2>/dev/null | wc -l" | tr -d '\r'
}

rss_kb() {
    adb_shell "grep VmRSS /proc/$1/status 2>/dev/null | awk '{print \$2}'" | tr -d '\r'
}

write_config() {
    local file="$1"
    "$ADB" push "$file" "$CONFIG_FILE" >/dev/null 2>&1
    adb_shell sync >/dev/null 2>&1 || true
    sleep 1.5
}

make_config() {
    local file="$1" text_x="$2" text_y="$3" logo1_x="$4" logo1_y="$5" logo2_x="$6" logo2_y="$7" logo2_en="$8"
    cat > "$file" <<EOF
[text]
enabled = 1
x = $text_x
y = $text_y
[logo1]
enabled = 1
x = $logo1_x
y = $logo1_y
image = /oem/usr/share/badge/electroniccats.bmp
[logo2]
enabled = $logo2_en
x = $logo2_x
y = $logo2_y
image = /oem/usr/share/badge/hack_gdl.bmp
EOF
}

msg_step "Esperando ADB"
if ! wait_adb; then
    fail "ADB no responde"
    exit 1
fi
pass "ADB responde"

PID=$(wait_ready) || { fail "app no llegó a listo"; exit 1; }
pass "app lista (pid $PID)"

BASE_PHOTOS=$(photos_count)
BASE_FDS=$(fd_count "$PID")
BASE_RSS=$(rss_kb "$PID")
echo "  base: fotos=$BASE_PHOTOS fds=$BASE_FDS rss=${BASE_RSS}KB"

# ---------------------------------------------------------------------------
step "Estrés de captura ($CAPTURES)"
CAPTURE_ERRORS=0
for i in $(seq 1 "$CAPTURES"); do
    PID=$(wait_ready) || { fail "app perdida en captura $i"; CAPTURE_ERRORS=$((CAPTURE_ERRORS + 1)); break; }
    capture_once "$PID" || CAPTURE_ERRORS=$((CAPTURE_ERRORS + 1))
    rotate_photos
    if [ $((i % 50)) -eq 0 ]; then
        echo "  capturas: $i/$CAPTURES"
    fi
done
AFTER_PHOTOS=$(photos_count)
if [ "$KEEP_LAST" -gt 0 ]; then
    if [ "$CAPTURE_ERRORS" -eq 0 ]; then
        pass "capturas con rotacion: $CAPTURES procesadas (keep=$KEEP_LAST, fotos=$AFTER_PHOTOS)"
    else
        fail "capturas con rotacion: $CAPTURE_ERRORS errores de $CAPTURES"
    fi
elif [ "$AFTER_PHOTOS" -ge $((BASE_PHOTOS + CAPTURES)) ]; then
    pass "capturas: $BASE_PHOTOS -> $AFTER_PHOTOS"
else
    fail "capturas: $BASE_PHOTOS -> $AFTER_PHOTOS (esperadas +$CAPTURES)"
fi

PID=$(wait_ready) || { fail "app no responde tras capturas"; exit 1; }
AFTER_FDS=$(fd_count "$PID")
AFTER_RSS=$(rss_kb "$PID")
echo "  post-captura: fds=$AFTER_FDS rss=${AFTER_RSS}KB"
if [ $((AFTER_FDS - BASE_FDS)) -lt 32 ]; then
    pass "sin fuga evidente de FDs (+$((AFTER_FDS - BASE_FDS)))"
else
    fail "posible fuga de FDs: $BASE_FDS -> $AFTER_FDS"
fi
if [ $((AFTER_RSS - BASE_RSS)) -lt 20480 ]; then
    pass "sin crecimiento excesivo de RSS (+$((AFTER_RSS - BASE_RSS))KB)"
else
    fail "RSS creció demasiado: $BASE_RSS -> $AFTER_RSS KB"
fi

# ---------------------------------------------------------------------------
step "Cambios de configuración en vivo ($CONFIG_CYCLES)"
CFG_OK=0
for i in $(seq 1 "$CONFIG_CYCLES"); do
    x=$((100 + (i % 10) * 40))
    y=$((100 + (i % 7) * 40))
    make_config "$WORK_DIR/configs/valid.ini" "$x" "$y" "$((x + 40))" "$((y + 80))" 1520 640 "$((i % 2))"
    write_config "$WORK_DIR/configs/valid.ini" || { fail "push de configuración $i"; break; }
    PID=$(wait_ready) || { fail "app perdida en configuración $i"; break; }
    capture_once "$PID" || true
    if adb_shell 'grep -q "overlay: configuración aplicada en vivo" /tmp/badge_photo.log' 2>/dev/null; then
        CFG_OK=$((CFG_OK + 1))
    fi
    if [ $((i % 10)) -eq 0 ]; then
        echo "  config: $i/$CONFIG_CYCLES"
    fi
done
if [ "$CFG_OK" -ge $((CONFIG_CYCLES / 2)) ]; then
    pass "configuración aplicada en vivo ($CFG_OK/$CONFIG_CYCLES)"
else
    fail "pocas aplicaciones en vivo ($CFG_OK/$CONFIG_CYCLES)"
fi

# Invalid and partial configurations must not kill the app.
printf 'not an ini\n' > "$WORK_DIR/configs/invalid.ini"
write_config "$WORK_DIR/configs/invalid.ini" || true
PID=$(wait_ready) && capture_once "$PID" || true
if [ -n "${PID:-}" ] && adb_shell "kill -0 $PID" 2>/dev/null; then
    pass "INI inválido no tumba la app"
else
    fail "app perdida con INI inválido"
fi

printf '[text]\nx = 10\n' > "$WORK_DIR/configs/partial.ini"
write_config "$WORK_DIR/configs/partial.ini" || true
PID=$(wait_ready) && capture_once "$PID" || true
if [ -n "${PID:-}" ] && adb_shell "kill -0 $PID" 2>/dev/null; then
    pass "INI parcial no tumba la app"
else
    fail "app perdida con INI parcial"
fi

# Restore defaults before reboot stress.
"$ADB" push "$ROOT_DIR/app/assets/overlays/overlay.ini" "$CONFIG_FILE" >/dev/null 2>&1 || true
adb_shell sync >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
step "Estrés de reboot ($REBOOTS ciclos)"
REBOOT_OK=0
for i in $(seq 1 "$REBOOTS"); do
    BEFORE=$(photos_count)
    adb_shell reboot >/dev/null 2>&1 || true
    sleep 3
    if ! wait_adb; then
        fail "ADB no volvió en reboot $i"
        break
    fi
    PID=$(wait_ready) || { fail "app no lista en reboot $i"; break; }
    AFTER=$(photos_count)
    SERIAL_NOW=$(adb_shell getprop ro.serialno 2>/dev/null | tr -d '\r' || true)
    CFG_PRESENT=$(adb_shell "test -f $CONFIG_FILE && echo yes" 2>/dev/null | tr -d '\r' || true)
    if [ "$AFTER" -ge "$BEFORE" ] && [ "$CFG_PRESENT" = "yes" ]; then
        REBOOT_OK=$((REBOOT_OK + 1))
    fi
    capture_once "$PID" || true
    echo "  reboot $i/$REBOOTS: fotos=$AFTER cfg=$CFG_PRESENT"
done
if [ "$REBOOT_OK" -eq "$REBOOTS" ]; then
    pass "persistencia tras $REBOOTS reboots"
else
    fail "persistencia en reboots: $REBOOT_OK/$REBOOTS"
fi

# ---------------------------------------------------------------------------
if [ "$SOAK_MINUTES" -gt 0 ]; then
    step "Soak test ($SOAK_MINUTES minutos)"
    SOAK_OK=0
    for m in $(seq 1 "$SOAK_MINUTES"); do
        PID=$(wait_ready) || { fail "app perdida en soak minuto $m"; break; }
        capture_once "$PID" || true
        if adb_shell "kill -0 $PID" 2>/dev/null; then
            SOAK_OK=$((SOAK_OK + 1))
        fi
        echo "  soak: $m/$SOAK_MINUTES min (fotos=$(photos_count))"
    done
    if [ "$SOAK_OK" -eq "$SOAK_MINUTES" ]; then
        pass "soak test $SOAK_MINUTES min"
    else
        fail "soak test: $SOAK_OK/$SOAK_MINUTES min"
    fi
fi

# ---------------------------------------------------------------------------
step "Resultado"
echo "  PASS: $PASS"
echo "  FAIL: $FAIL"
echo "  Logs: $WORK_DIR"
[ "$FAIL" -eq 0 ]
