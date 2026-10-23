#!/usr/bin/env bash
#
# File: scripts/inspect-image.sh
# Purpose: Static validation of the generated badge image.
#
# Static validation of the generated badge image.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

SDK="$BUILD_DIR"
IMAGE="$SDK/output/image"
FASTBOOT="$SDK/output/out/fastboot"
KCONFIG="$SDK/sysdrv/source/objs_kernel/.config"
FAIL=0

ok()   { echo "  [ok]   $*"; }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }
note() { echo "  [info] $*"; }

need_file() {
    if [ -f "$1" ]; then ok "$1"; else bad "falta $1"; fi
}

msg_step "Imagen"
need_file "$IMAGE/update.img"
need_file "$IMAGE/boot.img"
need_file "$IMAGE/meta.img"
need_file "$IMAGE/idblock.img"
need_file "$IMAGE/uboot.img"
if [ -f "$IMAGE/update.img" ]; then
    ls -lh "$IMAGE/update.img"
fi

msg_step "Device Tree"
DTB="$FASTBOOT/rv1106g-badge-fastboot.dtb"
if [ -f "$DTB" ]; then
    dtc -I dtb -O dts "$DTB" -o /tmp/badge-inspect.dts 2>/dev/null
    for needle in 'compatible = "smartsens,sc3336"' 'gpio-keys' 'rockchip,spidev' 'thunder-boot-service'; do
        if grep -q "$needle" /tmp/badge-inspect.dts; then
            ok "DTB contiene: $needle"
        else
            bad "DTB no contiene: $needle"
        fi
    done
    if grep -q 'fbtft' /tmp/badge-inspect.dts; then
        bad "DTB conserva el nodo fbtft (conflicto con botones)"
    else
        ok "DTB sin fbtft"
    fi
    if grep -q 'mis5001@31' /tmp/badge-inspect.dts; then
        bad "DTB conserva mis5001"
    else
        ok "DTB sin mis5001"
    fi
else
    bad "falta $DTB"
fi

msg_step "Kernel"
if [ -f "$KCONFIG" ]; then
    grep -q '^CONFIG_VIDEO_SC3336=y' "$KCONFIG" && ok "SC3336=y" || bad "SC3336 no está en el kernel"
    grep -q '^# CONFIG_VIDEO_SC3338 is not set' "$KCONFIG" && ok "SC3338 deshabilitado" || bad "SC3338 sigue habilitado"
    grep -q '^CONFIG_USB_CONFIGFS_F_FS=y' "$KCONFIG" && ok "FunctionFS configfs habilitado" || bad "FunctionFS configfs ausente"
    grep -q '^CONFIG_USB_CONFIGFS_ACM=y' "$KCONFIG" && ok "ACM de rescate habilitado" || bad "ACM de rescate ausente"
    grep -q '^CONFIG_POSIX_MQUEUE=y' "$KCONFIG" && ok "POSIX mqueue para umtprd" || bad "POSIX mqueue ausente (umtprd fallará)"
    grep -q '^CONFIG_USB_F_FS=m' "$KCONFIG" && ok "usb_f_fs.ko presente" || bad "usb_f_fs.ko ausente"
    grep -q '^CONFIG_INPUT_EVDEV=y' "$KCONFIG" && ok "evdev para gpio-keys" || bad "evdev ausente (sin /dev/input/eventX)"
    grep -q '^CONFIG_SPI_ROCKCHIP=y' "$KCONFIG" && ok "SPI rockchip" || bad "SPI rockchip ausente"
    grep -q '^CONFIG_SPI_SPIDEV=y' "$KCONFIG" && ok "spidev para LED" || bad "spidev ausente (sin /dev/spidev0.0)"
else
    bad "falta $KCONFIG"
fi

msg_step "MCU"
if grep -a -q 'SC3336_stream_on_late' "$IMAGE/idblock.img"; then
    ok "idblock contiene MCU SC3336"
else
    bad "idblock no contiene MCU SC3336"
fi

msg_step "Meta"
if [ -f "$IMAGE/meta.img" ]; then
    python3 - "$IMAGE/meta.img" <<'PY'
import struct, sys
b=open(sys.argv[1],'rb').read()
assert len(b)==393216, f"meta size {len(b)}"
o=2048
head,l,crc=struct.unpack_from('<IiI',b,o); o+=12
for _ in range(8):
    idx,func=struct.unpack_from('<ii',b,o); o+=28
    assert idx == -1, f"GPIO inesperado {idx}"
cam_w,cam_h,als_type,als_value=struct.unpack_from('<4i',b,o)
assert (cam_w,cam_h)==(2304,1296), (cam_w,cam_h)
assert als_type==0, als_type
print("  [ok]   meta init 2304x1296, GPIOs -1, ALS none")
PY
    if [ $? -eq 0 ]; then :; else bad "meta init inválido"; fi
    if grep -a -q 'rk_iqbin_size' "$IMAGE/meta.img"; then
        ok "meta contiene cmdline de IQ"
    else
        bad "meta no contiene cmdline de IQ"
    fi
else
    bad "falta meta.img"
fi

msg_step "Rootfs"
ROOTFS="$FASTBOOT/rootfs_erofs.img"
if [ -f "$ROOTFS" ]; then
    rm -rf /tmp/badge-rootfs-inspect
    mkdir -p /tmp/badge-rootfs-inspect
    if fsck.erofs --extract=/tmp/badge-rootfs-inspect "$ROOTFS" >/dev/null 2>&1; then
        ok "rootfs erofs extraído"
        for f in oem/usr/bin/badge_photo oem/usr/sbin/umtprd \
                 usr/bin/adbd oem/usr/bin/overlay_web oem/usr/bin/rndis_dhcp etc/init.d/S49mtp etc/umtprd/umtprd.conf \
                 etc/init.d/S99badge_photo etc/badge_photo.conf; do
            [ -e "/tmp/badge-rootfs-inspect/$f" ] && ok "rootfs: $f" || bad "rootfs falta: $f"
        done
        grep -q 'mount userdata' /tmp/badge-rootfs-inspect/etc/init.d/rcS && \
            ok "rcS monta userdata" || bad "rcS no monta userdata"
        grep -q '/dev/ptmx' /tmp/badge-rootfs-inspect/etc/init.d/rcS && \
            ok "rcS crea /dev/ptmx" || bad "rcS no crea /dev/ptmx"
        grep -q 'S49mtp start' /tmp/badge-rootfs-inspect/etc/init.d/rcS && \
            ok "rcS arranca MTP" || bad "rcS no arranca MTP"
        grep -q 'badge_photo' /tmp/badge-rootfs-inspect/etc/init.d/rcS && \
            ok "rcS arranca badge_photo" || bad "rcS no arranca la app"
        file /tmp/badge-rootfs-inspect/oem/usr/bin/badge_photo | grep -q 'ARM' && \
            ok "badge_photo es ELF ARM" || bad "badge_photo no es ELF ARM"
        file /tmp/badge-rootfs-inspect/oem/usr/sbin/umtprd | grep -q 'ARM' && \
            ok "umtprd es ELF ARM" || bad "umtprd no es ELF ARM"
        for f in electroniccats.bmp hack_gdl.bmp text_hack_gdl.bmp overlay.ini font.ttf editor.html electroniccats.png hack_gdl.png; do
            [ -f "/tmp/badge-rootfs-inspect/oem/usr/share/badge/$f" ] && \
                ok "rootfs: web/overlay $f" || bad "rootfs falta web/overlay $f"
        done
        [ -f /tmp/badge-rootfs-inspect/oem/usr/ko/usb_f_fs.ko ] && \
            ok "rootfs: usb_f_fs.ko" || bad "rootfs falta usb_f_fs.ko"
        [ -f /tmp/badge-rootfs-inspect/oem/usr/ko/usb_f_acm.ko ] && \
            ok "rootfs: usb_f_acm.ko" || bad "rootfs falta usb_f_acm.ko"
        [ -f /tmp/badge-rootfs-inspect/oem/usr/ko/u_serial.ko ] && \
            ok "rootfs: u_serial.ko" || bad "rootfs falta u_serial.ko"
        [ -f /tmp/badge-rootfs-inspect/oem/usr/ko/u_ether.ko ] && \
            ok "rootfs: u_ether.ko (RNDIS)" || bad "rootfs falta u_ether.ko (RNDIS)"
        [ -f /tmp/badge-rootfs-inspect/oem/usr/ko/usb_f_rndis.ko ] && \
            ok "rootfs: usb_f_rndis.ko (RNDIS)" || bad "rootfs falta usb_f_rndis.ko (RNDIS)"
        grep -q 'ffs.adb' /tmp/badge-rootfs-inspect/etc/init.d/S49mtp && \
            ok "S49mtp configura ADB" || bad "S49mtp no configura ADB"
        grep -q 'adbd' /tmp/badge-rootfs-inspect/etc/init.d/S49mtp && \
            ok "S49mtp arranca adbd" || bad "S49mtp no arranca adbd"
    else
        bad "no se pudo extraer rootfs erofs"
    fi
else
    bad "falta $ROOTFS"
fi

msg_step "Resultado"
if [ "$FAIL" -ne 0 ]; then
    msg_error "$FAIL verificación(es) fallaron."
    exit 1
fi
msg_info "Validación estática OK."
