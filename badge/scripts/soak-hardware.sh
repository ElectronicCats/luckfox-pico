#!/usr/bin/env bash
#
# File: scripts/soak-hardware.sh
# Purpose: Two-hour soak for the battery badge: periodic captures, live overlay
#          changes, MTP/ADB and resource telemetry, followed by a final suite.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

DURATION="${SOAK_SECONDS:-7200}"
CAPTURE_EVERY="${SOAK_CAPTURE_EVERY:-600}"
CONFIG_EVERY="${SOAK_CONFIG_EVERY:-1200}"
METRIC_EVERY="${SOAK_METRIC_EVERY:-60}"

ADB="$(adb_bin)" || { msg_error "adb no encontrado."; exit 1; }
ADB_LIB="$(adb_ldpath)"
[ -n "$ADB_LIB" ] && export LD_LIBRARY_PATH="$ADB_LIB:${LD_LIBRARY_PATH:-}"

STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$OUT_DIR/soak-$STAMP.log"
mkdir -p "$OUT_DIR"
: > "$LOG"

log() {
    printf '%s\n' "$*" | tee -a "$LOG"
}

dev_shell() {
    "$ADB" shell "$*" 2>&1 | tr -d '\r'
}

wait_adb() {
    local i
    for i in $(seq 1 80); do
        "$ADB" get-state 2>/dev/null | grep -q device && return 0
        sleep 1
    done
    return 1
}

photo_count() {
    dev_shell 'ls /userdata/photos/*.jpg 2>/dev/null | wc -l' | tr -d ' '
}

capture() {
    local pid before after
    before="$(photo_count)"
    pid="$(dev_shell 'cat /tmp/badge_photo.ready 2>/dev/null' | tr -d '\r')"
    if [ -z "$pid" ]; then
        log "CAPTURE FAIL: sin /tmp/badge_photo.ready"
        return 1
    fi
    dev_shell "kill -USR1 $pid" >/dev/null || {
        log "CAPTURE FAIL: kill -USR1 $pid"
        return 1
    }
    sleep 4
    after="$(photo_count)"
    if [ "$after" -le "$before" ]; then
        log "CAPTURE FAIL: fotos $before -> $after"
        return 1
    fi
    log "CAPTURE OK: fotos $before -> $after"
    return 0
}

config_a="$OUT_DIR/soak-overlay-a.ini"
config_b="$OUT_DIR/soak-overlay-b.ini"
backup="$OUT_DIR/soak-overlay-original.ini"
cat > "$config_a" <<'EOF'
[text]
enabled = 1
x = 100
y = 100
text = SOAK 2H
size = 32
color = fff799
font = /oem/usr/share/badge/font.ttf

[logo1]
enabled = 1
x = 100
y = 640
image = /oem/usr/share/badge/electroniccats.bmp

[logo2]
enabled = 1
x = 1520
y = 640
image = /oem/usr/share/badge/hack_gdl.bmp
EOF
sed 's/^x = 100$/x = 180/' "$config_a" > "$config_b"

restore_config() {
    if [ -s "$backup" ]; then
        "$ADB" push "$backup" /userdata/badge/overlay.ini >/dev/null 2>&1 || true
        log "CONFIG: original restaurada"
    fi
}
trap restore_config EXIT INT TERM

if ! wait_adb; then
    log "SOAK ABORT: no ADB"
    exit 1
fi
"$ADB" pull /userdata/badge/overlay.ini "$backup" >/dev/null 2>&1 || {
    log "SOAK ABORT: no se pudo respaldar overlay.ini"
    exit 1
}

start_epoch="$(date +%s)"
end_epoch=$((start_epoch + DURATION))
next_capture=$((start_epoch + CAPTURE_EVERY))
next_config=$((start_epoch + CONFIG_EVERY))
next_metric=$((start_epoch + METRIC_EVERY))
use_a=1

log "SOAK START $(date -Is)"
log "duration=${DURATION}s capture=${CAPTURE_EVERY}s config=${CONFIG_EVERY}s metric=${METRIC_EVERY}s"
log "image=$(sha256sum "$OUT_DIR/update.img" 2>/dev/null | awk '{print $1}')"
log "photos_before=$(photo_count)"

"$ADB" push "$config_a" /userdata/badge/overlay.ini >/dev/null 2>&1 || true
if ! capture; then
    log "SOAK ABORT: primera captura"
    exit 1
fi

while :; do
    now="$(date +%s)"
    [ "$now" -ge "$end_epoch" ] && break

    if [ "$now" -ge "$next_metric" ]; then
        pid="$(dev_shell 'cat /tmp/badge_photo.ready 2>/dev/null' | tr -d '\r')"
        fds="?"
        rss="?"
        if [ -n "$pid" ]; then
            fds="$(dev_shell "ls /proc/$pid/fd 2>/dev/null | wc -l" | tr -d ' ')"
            rss="$(dev_shell "awk '/VmRSS/ {print \$2}' /proc/$pid/status 2>/dev/null")"
        fi
        invalid="$(dev_shell 'grep -c "AWB stats invalid" /tmp/badge_photo.log 2>/dev/null' | tr -d ' ')"
        temp="$(dev_shell 'for z in /sys/class/thermal/thermal_zone*/temp; do [ -e "$z" ] && cat "$z"; done' | tr '\n' ',' | sed 's/,$//')"
        up="$(dev_shell 'uptime' | tr -s ' ')"
        free="$(dev_shell 'df -k /userdata | tail -1' | awk '{print $4}')"
        log "METRIC $(date -Is) pid=${pid:-none} rss_kb=${rss:-?} fds=${fds:-?} photos=$(photo_count) free_kb=${free:-?} awb_invalid=${invalid:-?} temp=${temp:-?} uptime=${up:-?}"
        next_metric=$((next_metric + METRIC_EVERY))
    fi

    if [ "$now" -ge "$next_config" ]; then
        if [ "$use_a" -eq 1 ]; then
            "$ADB" push "$config_b" /userdata/badge/overlay.ini >/dev/null 2>&1 || true
            use_a=0
            log "CONFIG: variante B (x=180)"
        else
            "$ADB" push "$config_a" /userdata/badge/overlay.ini >/dev/null 2>&1 || true
            use_a=1
            log "CONFIG: variante A (x=100)"
        fi
        next_config=$((next_config + CONFIG_EVERY))
    fi

    if [ "$now" -ge "$next_capture" ]; then
        capture || log "SOAK WARN: captura falló"
        next_capture=$((next_capture + CAPTURE_EVERY))
    fi

    if ! "$ADB" get-state 2>/dev/null | grep -q device; then
        log "SOAK ABORT: ADB perdido $(date -Is)"
        exit 1
    fi
    sleep 10
done

log "SOAK END $(date -Is) photos_after=$(photo_count)"
restore_config
trap - EXIT INT TERM
log "FINAL SUITE"
"$SCRIPT_DIR/test-hardware.sh" --count 2 >>"$LOG" 2>&1
rc=$?
log "FINAL SUITE rc=$rc"
exit "$rc"
