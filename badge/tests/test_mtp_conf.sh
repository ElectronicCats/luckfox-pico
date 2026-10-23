#!/bin/sh
#
# File: tests/test_mtp_conf.sh
# Purpose: Host validation of the MTP configuration template and boot order.
#
# Validate the MTP configuration template and boot ordering.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CONF="$ROOT/configs/overlay-badge-fastboot/etc/umtprd/umtprd.conf"
MTP="$ROOT/configs/overlay-badge-fastboot/etc/init.d/S49mtp"
POST="$ROOT/configs/post/luckfox-rv1106-badge-tb-spi_nand-post.sh"
fail=0

check() {
    if ! grep -q "$1" "$2"; then
        echo "FAIL: '$1' no está en $2"
        fail=1
    fi
}

check 'storage "/userdata/photos"' "$CONF"
check 'sync_when_close 0x1' "$CONF"
check 'umask 022' "$CONF"
check 'loop_on_disconnect 1' "$CONF"
check 'serial "@MTP_SERIAL@"' "$CONF"
check 'usb_functionfs_mode 0x1' "$CONF"
check 'usb_vendor_id  *0x2207' "$CONF"
check 'usb_product_id  *0x0019' "$CONF"

if grep -q 'storage "/userdata"' "$CONF"; then
    echo "FAIL: el storage no debe ser /userdata completo"
    fail=1
fi

# In start(), umtprd must be launched before binding the UDC.
start_line=$(grep -n '^start()' "$MTP" | cut -d: -f1)
umtprd_line=$(grep -n 'start_umtprd' "$MTP" | tail -1 | cut -d: -f1)
udc_line=$(grep -n 'bind_udc ||' "$MTP" | tail -1 | cut -d: -f1)
if [ -n "$start_line" ] && { [ -z "$umtprd_line" ] || [ -z "$udc_line" ] || [ "$umtprd_line" -ge "$udc_line" ]; }; then
    echo "FAIL: S49mtp debe arrancar umtprd antes de bind_udc"
    fail=1
fi

# rcS order: userdata -> camera app -> camera-ready wait -> MTP/web/ADB.
link_line=$(grep -n '_S20linkmount start' "$POST" | head -1 | cut -d: -f1)
app_line=$(grep -n '^[[:space:]]*badge_photo[[:space:]]' "$POST" | head -1 | cut -d: -f1)
wait_line=$(grep -n 'badge_system.ready' "$POST" | head -1 | cut -d: -f1)
mtp_line=$(grep -n 'S49mtp start' "$POST" | head -1 | cut -d: -f1)
if [ -z "$link_line" ] || [ -z "$app_line" ] || [ -z "$wait_line" ] || \
   [ -z "$mtp_line" ] || [ "$link_line" -ge "$app_line" ] || \
   [ "$app_line" -ge "$wait_line" ] || [ "$wait_line" -ge "$mtp_line" ]; then
    echo "FAIL: orden incorrecto (userdata -> app -> cámara -> MTP)"
    fail=1
fi
grep -q 'system timeout' "$POST" || {
    echo "FAIL: falta el fallback de timeout de cámara"
    fail=1
}

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "MTP configuration checks passed"
