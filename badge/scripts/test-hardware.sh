#!/usr/bin/env bash
#
# File: scripts/test-hardware.sh
# Purpose: Hardware test suite over ADB for the Badge Hack GDL 2026.
#
# Usage:
#   ./scripts/test-hardware.sh [--app PATH] [--count N] [--reboot] [--no-mtp]
#
# Defaults:
#   app   = /oem/usr/bin/badge_photo
#   count = 3
#
# It never unbinds the USB gadget. If the app dies, ADB stays available.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

ADB="$(adb_bin)" || { msg_error "adb no encontrado. Instala adb o define ADB=/ruta/adb."; exit 1; }
ADB_LIB="$(adb_ldpath)"
[ -n "$ADB_LIB" ] && export LD_LIBRARY_PATH="$ADB_LIB:${LD_LIBRARY_PATH:-}"

APP_BIN="/oem/usr/bin/badge_photo"
COUNT=3
DO_REBOOT=0
DO_MTP=1
DO_RESTART=0
PHOTOS_DIR="/userdata/photos"
PIDFILE="/var/run/badge_photo_test.pid"
WORK_DIR="$ROOT_DIR/out/hardware-test-$(date +%Y%m%d-%H%M%S)"
PASS=0
FAIL=0

usage() {
    cat <<EOF
Uso: $(basename "$0") [opciones]

  --app PATH     binario badge_photo en la placa (default $APP_BIN)
  --count N      capturas a probar (default $COUNT)
  --reboot       probar persistencia tras reboot
  --restart      reiniciar la app antes de capturar (por defecto usa la del boot)
  --no-mtp       omitir validación MTP
  -h, --help     esta ayuda
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --app) APP_BIN="$2"; shift 2 ;;
        --count) COUNT="$2"; shift 2 ;;
        --reboot) DO_REBOOT=1; shift ;;
        --restart) DO_RESTART=1; shift ;;
        --no-mtp) DO_MTP=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) msg_error "Opción desconocida: $1"; usage; exit 1 ;;
    esac
done

mkdir -p "$WORK_DIR"
exec > >(tee "$WORK_DIR/test.log") 2>&1

pass() { PASS=$((PASS + 1)); echo "  [PASS] $*"; }
fail() { FAIL=$((FAIL + 1)); echo "  [FAIL] $*"; }
check() {
    local desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        pass "$desc"
    else
        fail "$desc"
    fi
}

adb_shell() { "$ADB" shell "$@"; }
adb_cmd() { "$ADB" "$@"; }

wait_adb() {
    local i
    for i in $(seq 1 120); do
        if "$ADB" get-state 2>/dev/null | grep -q device; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

if ! command -v "$ADB" >/dev/null 2>&1; then
    msg_error "adb no encontrado: $ADB"
    msg_info "Define ADB=/ruta/a/adb"
    exit 1
fi

msg_step "Esperando ADB"
if ! wait_adb; then
    msg_error "La placa no aparece por ADB."
    exit 1
fi
pass "ADB responde"

msg_step "Estado del gadget USB"
STAGE=$(adb_shell 'cat /tmp/usb_stage 2>/dev/null' | tr -d '\r')
[ "$STAGE" = "0,32,0" ] && pass "stage USB verde ($STAGE)" || fail "stage USB no verde ($STAGE)"
UDC=$(adb_shell 'cat /sys/kernel/config/usb_gadget/badge/UDC 2>/dev/null' | tr -d '\r')
[ -n "$UDC" ] && pass "UDC bindeado: $UDC" || fail "UDC vacío"
check "MTP FunctionFS listo" adb_shell 'test -c /dev/ffs-mtp/ep1'
check "ADB FunctionFS listo" adb_shell 'test -c /dev/usb-ffs/adb/ep1'
check "umtprd corriendo" adb_shell 'ps | grep -v grep | grep -q umtprd'
check "adbd corriendo" adb_shell 'ps | grep -v grep | grep -q adbd'
check "/dev/ptmx presente" adb_shell 'test -c /dev/ptmx'
check "/system/bin/sh presente" adb_shell 'test -x /system/bin/sh'
check "event0 existe" adb_shell 'test -c /dev/input/event0'
check "gpio-keys presente" adb_shell 'grep -q "gpio-keys" /proc/bus/input/devices'

msg_step "Almacenamiento"
check "/userdata montado" adb_shell 'mountpoint -q /userdata'
check "/userdata/photos existe" adb_shell "test -d $PHOTOS_DIR"

msg_step "Cámara y capturas ($COUNT)"
BEFORE=$(adb_shell "ls $PHOTOS_DIR/*.jpg 2>/dev/null | wc -l" | tr -d '\r')
# User config for the next start (aiq off, photos dir). The boot app already
# read its config; this is mainly for --restart or a cold start.
adb_shell "printf 'output_dir=$PHOTOS_DIR\nwidth=2304\nheight=1296\nquality=90\nled=1\nled_count=1\ninput_name=gpio-keys\ncapture_timeout_ms=3000\nretries=3\naiq=1\nlog_level=3\n' > /userdata/badge_photo.conf"

PID=""
if [ "$DO_RESTART" -eq 0 ]; then
    adb_shell 'rm -f /tmp/badge_photo_pause' 2>/dev/null || true
    for i in $(seq 1 20); do
        PID=$(adb_shell "ps | grep '[b]adge_photo' | awk '{print \$1}' | head -1" 2>/dev/null | tr -d '\r' || true)
        [ -n "$PID" ] && break
        sleep 0.5
    done
fi

if [ "$DO_RESTART" -eq 1 ] || [ -z "$PID" ]; then
    adb_shell "touch /tmp/badge_photo_pause; killall badge_photo badge_photo_test 2>/dev/null || true; sleep 2; rm -f $PIDFILE /tmp/badge_photo.log"
    # Wrapper so the daemon's stderr goes to a log we can inspect.
    adb_shell "echo '#!/bin/sh' > /userdata/run_badge_photo.sh; echo 'exec $APP_BIN >>/tmp/badge_photo.log 2>&1' >> /userdata/run_badge_photo.sh; chmod 755 /userdata/run_badge_photo.sh"
    if ! adb_shell "start-stop-daemon --start --background --make-pidfile --pidfile $PIDFILE --exec /userdata/run_badge_photo.sh; sleep 1"; then
        fail "no se pudo arrancar $APP_BIN"
    else
        sleep 2
        PID=$(adb_shell "cat $PIDFILE 2>/dev/null" | tr -d '\r')
        if [ -n "$PID" ] && adb_shell "kill -0 $PID"; then
            pass "app reiniciada (pid $PID)"
        else
            fail "app no quedó corriendo"
            PID=""
        fi
    fi
else
    pass "app del boot (pid $PID)"
fi

if [ -n "$PID" ] && adb_shell "kill -0 $PID"; then
    sleep 1
    check "input gpio-keys en log" adb_shell 'grep -q "input: /dev/input/event0 (gpio-keys)" /tmp/badge_photo.log'
fi

# Wait for the app's own readiness marker (AIQ settle + warmup) before
# sending capture signals. The marker contains the PID, so a stale line from
# a previous boot cannot make the test signal an app that is still starting.
READY=0
for i in $(seq 1 120); do
    PID=$(adb_shell "ps | grep '[b]adge_photo' | awk '{print \$1}' | head -1" 2>/dev/null | tr -d '\r' || true)
    RPID=$(adb_shell 'cat /tmp/badge_photo.ready 2>/dev/null' 2>/dev/null | tr -d '\r' || true)
    if [ -n "$PID" ] && [ "$RPID" = "$PID" ]; then
        READY=1
        break
    fi
    sleep 0.5
done
if [ "$READY" -eq 1 ]; then
    pass "app lista (pid $PID, AIQ/cámara asentados)"
else
    fail "app no llegó a listo"
    PID=""
fi

if [ -n "${PID:-}" ]; then
    for i in $(seq 1 "$COUNT"); do
        CURRENT_PID=""
        for _ in $(seq 1 20); do
            CURRENT_PID=$(adb_shell "ps | grep '[b]adge_photo' | awk '{print \$1}' | head -1" 2>/dev/null | tr -d '\r' || true)
            [ -n "$CURRENT_PID" ] && break
            sleep 0.5
        done
        if [ -n "$CURRENT_PID" ] && adb_shell "kill -USR1 $CURRENT_PID"; then
            pass "SIGUSR1 captura $i enviada (pid $CURRENT_PID)"
        else
            fail "SIGUSR1 captura $i falló (sin app)"
        fi
        sleep 1.2
    done
    sleep 2
else
    fail "sin PID; no se pueden enviar capturas"
fi

AFTER=$(adb_shell "ls $PHOTOS_DIR/*.jpg 2>/dev/null | wc -l" | tr -d '\r')
if [ "$AFTER" -ge $((BEFORE + COUNT)) ]; then
    pass "fotos: $BEFORE -> $AFTER"
else
    fail "fotos: $BEFORE -> $AFTER (se esperaban +$COUNT)"
fi

# The MTP storage must only see complete JPEGs. Temporary files are staged
# outside /userdata/photos; a .tmp here can leave a nameless MTP object.
TMP_LEFT=$(adb_shell "ls $PHOTOS_DIR/*.tmp 2>/dev/null | wc -l" | tr -d '\r')
if [ "${TMP_LEFT:-0}" = "0" ]; then
    pass "sin temporales .tmp en el storage MTP"
else
    fail "quedaron $TMP_LEFT temporales .tmp en el storage MTP"
fi

AWB_BAD=$(adb_shell 'LINE=$(grep -n "camera: pipeline listo" /tmp/badge_photo.log 2>/dev/null | tail -1 | cut -d: -f1); if [ -n "$LINE" ]; then tail -n +"$LINE" /tmp/badge_photo.log | grep -c "AWB stats invalid" || true; else grep -c "AWB stats invalid" /tmp/badge_photo.log 2>/dev/null || true; fi' | tr -d '\r')
if [ "${AWB_BAD:-0}" = "0" ]; then
    pass "AIQ AWB stats válidas (después del último pipeline listo)"
else
    fail "AIQ AWB stats inválidas: $AWB_BAD"
fi

# The hardware RGN overlay must attach the two logos and the text.
OVERLAY_OK=$(adb_shell 'LINE=$(grep -n "aiq: listo" /tmp/badge_photo.log 2>/dev/null | tail -1 | cut -d: -f1); if [ -n "$LINE" ]; then tail -n +"$LINE" /tmp/badge_photo.log | grep -c "overlay: .* en (" || true; else grep -c "overlay: .* en (" /tmp/badge_photo.log 2>/dev/null || true; fi' | tr -d '\r')
if [ "${OVERLAY_OK:-0}" -ge 3 ]; then
    pass "overlay RGN (2 logos + texto)"
else
    fail "overlay RGN incompleto: $OVERLAY_OK/3"
fi

msg_step "Validación de JPEG"
rm -rf "$WORK_DIR/photos"
mkdir -p "$WORK_DIR/photos"
if adb_cmd pull "$PHOTOS_DIR/." "$WORK_DIR/photos/" >/dev/null 2>&1; then
    pass "fotos descargadas"
else
    fail "adb pull de fotos"
fi
if python3 - "$WORK_DIR/photos" "$COUNT" <<'PY'
import os, sys
from PIL import Image
root, count = sys.argv[1], int(sys.argv[2])
files = sorted(
    (os.path.join(root, f) for f in os.listdir(root) if f.lower().endswith('.jpg')),
    key=os.path.getmtime,
)
if len(files) < count:
    print(f"  [FAIL] solo {len(files)} JPEG, se esperaban {count}")
    sys.exit(1)
ok = 0
for path in files[-count:]:
    try:
        im = Image.open(path)
        im.load()
        if im.size == (2304, 1296) and os.path.getsize(path) > 10000:
            ok += 1
            print(f"  [PASS] {os.path.basename(path)} {im.size} {os.path.getsize(path)} bytes")
        else:
            print(f"  [FAIL] {os.path.basename(path)} {im.size} {os.path.getsize(path)} bytes")
    except Exception as exc:
        print(f"  [FAIL] {path}: {exc}")
if ok != count:
    sys.exit(1)
PY
then
    pass "JPEG válidos ($COUNT)"
else
    fail "JPEG inválidos"
fi

msg_step "MTP"
if [ "$DO_MTP" -eq 1 ] && command -v gio >/dev/null 2>&1; then
    URI=$(gio mount -l 2>/dev/null | sed -n 's/.*Mount(0):.*-> \(mtp:\/\/[^ ]*\).*/\1/p' | head -1)
    if [ -z "$URI" ]; then
        fail "MTP no montado por gio"
    else
        pass "MTP montado: $URI"
        LIST=$(gio list "${URI}Badge%20Hack%20GDL%202026%20Photos" 2>/dev/null || true)
        echo "$LIST" | sed 's/^/    /'
        if echo "$LIST" | grep -q '<none>'; then
            fail "MTP muestra un objeto sin nombre (<none>)"
        else
            pass "MTP sin objetos fantasma"
        fi
        if echo "$LIST" | grep -q '\.jpg'; then
            pass "MTP lista JPEG"
            SAMPLE=$(echo "$LIST" | grep '\.jpg' | head -1)
            if gio copy "${URI}Badge%20Hack%20GDL%202026%20Photos/$SAMPLE" "$WORK_DIR/mtp-$SAMPLE" >/dev/null 2>&1; then
                pass "MTP copia $SAMPLE"
                if adb_cmd pull "$PHOTOS_DIR/$SAMPLE" "$WORK_DIR/adb-$SAMPLE" >/dev/null 2>&1; then
                    if cmp -s "$WORK_DIR/mtp-$SAMPLE" "$WORK_DIR/adb-$SAMPLE"; then
                        pass "MTP y ADB coinciden (checksum)"
                    else
                        fail "MTP y ADB difieren"
                    fi
                else
                    fail "adb pull de la muestra MTP"
                fi
            else
                fail "MTP copy falló"
            fi
        else
            fail "MTP no lista JPEG"
        fi
    fi
else
    msg_warn "MTP omitido (gio no disponible o --no-mtp)"
fi

msg_step "Logs"
adb_cmd pull /tmp/usb_stage "$WORK_DIR/usb_stage" >/dev/null 2>&1 || true
adb_cmd pull /tmp/umtprd.log "$WORK_DIR/umtprd.log" >/dev/null 2>&1 || true
adb_cmd pull /tmp/adbd.log "$WORK_DIR/adbd.log" >/dev/null 2>&1 || true
adb_cmd pull /tmp/badge_photo.log "$WORK_DIR/badge_photo.log" >/dev/null 2>&1 || true
adb_shell 'dmesg' > "$WORK_DIR/dmesg.txt" 2>/dev/null || true

if [ "$DO_REBOOT" -eq 1 ]; then
    msg_step "Persistencia tras reboot"
    SERIAL_BEFORE=$(adb_cmd get-serialno 2>/dev/null || true)
    PHOTOS_BEFORE=$(adb_shell "ls $PHOTOS_DIR/*.jpg 2>/dev/null | wc -l" | tr -d '\r')
    adb_cmd reboot || true
    sleep 3
    if wait_adb; then
        pass "ADB volvió tras reboot"
        PHOTOS_AFTER=$(adb_shell "ls $PHOTOS_DIR/*.jpg 2>/dev/null | wc -l" | tr -d '\r')
        [ "$PHOTOS_AFTER" -ge "$PHOTOS_BEFORE" ] && pass "fotos persisten ($PHOTOS_BEFORE -> $PHOTOS_AFTER)" || fail "fotos no persisten"
        SERIAL_AFTER=$(adb_cmd get-serialno 2>/dev/null || true)
        [ -n "$SERIAL_AFTER" ] && pass "serial ADB: $SERIAL_AFTER" || fail "serial ADB vacío"
        [ "$SERIAL_BEFORE" = "$SERIAL_AFTER" ] && pass "serial estable" || fail "serial cambió"
    else
        fail "ADB no volvió tras reboot"
    fi
fi

msg_step "Resultado"
echo "  PASS: $PASS"
echo "  FAIL: $FAIL"
echo "  Logs: $WORK_DIR"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
msg_info "Hardware test OK."
