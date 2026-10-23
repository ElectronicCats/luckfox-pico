#!/usr/bin/env bash
#
# File: scripts/apply-layer.sh
# Purpose: Copy the badge layer into the SDK worktree and apply idempotent patches.
#
# Copy the badge layer into the SDK worktree and apply small idempotent patches.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"

SDK="$BUILD_DIR"
while [ $# -gt 0 ]; do
    case "$1" in
        --sdk) SDK="$2"; shift 2 ;;
        -h|--help)
            echo "Uso: $(basename "$0") [--sdk PATH]"
            exit 0
            ;;
        *) msg_error "Opción desconocida: $1"; exit 1 ;;
    esac
done

[ -d "$SDK" ] || { msg_error "No existe el SDK/worktree: $SDK"; exit 1; }
git -C "$SDK" rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
    msg_error "No es un repositorio git: $SDK"; exit 1;
}

BOARD_CONFIG="BoardConfig-SPI_NAND-Busybox-RV1106_Badge_Camera-IPC_FASTBOOT.mk"
POST_SCRIPT="luckfox-rv1106-badge-tb-spi_nand-post.sh"

copy_tree() {
    local src="$1" dst="$2"
    [ -e "$src" ] || return 0
    mkdir -p "$dst"
    cp -a "$src"/. "$dst"/
}

msg_step "Copiando board config y post-script"
copy_tree "$ROOT_DIR/configs/board" "$SDK/project/cfg/BoardConfig_IPC"
copy_tree "$ROOT_DIR/configs/post" "$SDK/project/cfg/BoardConfig_IPC"

msg_step "Copiando overlay"
rm -rf "$SDK/project/cfg/BoardConfig_IPC/overlay/overlay-badge-fastboot"
copy_tree "$ROOT_DIR/configs/overlay-badge-fastboot" \
          "$SDK/project/cfg/BoardConfig_IPC/overlay/overlay-badge-fastboot"

msg_step "Copiando DTS, kernel config y board del MCU"
copy_tree "$ROOT_DIR/dts" "$SDK/sysdrv/source/kernel/arch/arm/boot/dts"
copy_tree "$ROOT_DIR/kernel" "$SDK/sysdrv/source/kernel/arch/arm/configs"
copy_tree "$ROOT_DIR/mcu/board" \
          "$SDK/sysdrv/source/mcu/rt-thread/bsp/rockchip/rv1106-mcu/board"

msg_step "Copiando init/meta del sensor"
copy_tree "$ROOT_DIR/meta/sensor_init" "$SDK/project/make_meta/sensor_init"
copy_tree "$ROOT_DIR/meta/sensor_ae_awb_table" "$SDK/project/make_meta/sensor_ae_awb_table"

msg_step "Copiando app badge_photo"
rm -rf "$SDK/project/app/badge_photo"
mkdir -p "$SDK/project/app/badge_photo"
copy_tree "$ROOT_DIR/app" "$SDK/project/app/badge_photo"
if [ -d "$ROOT_DIR/third_party/umtprd" ]; then
    mkdir -p "$SDK/project/app/badge_photo/third_party"
    copy_tree "$ROOT_DIR/third_party/umtprd" \
              "$SDK/project/app/badge_photo/third_party/umtprd"
fi

msg_step "Aplicando parches mínimos"
python3 - "$SDK" <<'PY'
import sys
from pathlib import Path

sdk = Path(sys.argv[1])

# 1) make_meta: add sc3336 to the supported sensor list.
build_meta = sdk / "project/make_meta/build_meta.sh"
if build_meta.exists():
    text = build_meta.read_text()
    old = 'support_sensors="os02k10 sc301iot sc501ai sc230ai sc200ai sc3338 sc4336 gc2093 gc3003 sc231hai sc2336p"'
    new = 'support_sensors="os02k10 sc301iot sc501ai sc230ai sc200ai sc3336 sc3338 sc4336 gc2093 gc3003 sc231hai sc2336p"'
    if "sc3336" not in text:
        if old in text:
            build_meta.write_text(text.replace(old, new))
        else:
            # Fallback: insert before sc3338 in the support_sensors line.
            lines = text.splitlines()
            for i, line in enumerate(lines):
                if line.startswith("support_sensors=") and "sc3338" in line:
                    lines[i] = line.replace("sc3338", "sc3336 sc3338", 1)
                    break
            build_meta.write_text("\n".join(lines) + "\n")

# 2) make_meta: avoid -m32 (host without multilib). The structs use int32_t,
#    so the generated layout is unchanged.
for rel in ["project/make_meta/sensor_init/Makefile",
            "project/make_meta/sensor_ae_awb_table/Makefile"]:
    path = sdk / rel
    if path.exists():
        text = path.read_text()
        path.write_text(text.replace("gcc -m32", "gcc"))

# 3) fast_ae: do not touch invalid IR-cut GPIOs. The badge has no IR-cut
#    and the meta uses GPIO_INVALID so it does not overwrite the photo button (GPIO1_D3).
fast_ae = sdk / "sysdrv/source/mcu/rt-thread/applications/battery-ipc/fast_ae.c"
if fast_ae.exists():
    text = fast_ae.read_text()
    marker = "BADGE_GPIO_GUARD"
    if marker not in text:
        old = """int32_t ircut_switch(struct sensor_init_cfg *sensor_init, IR_STATUS_t on_or_off)
{
    if (on_or_off == IR_STATUS_CUT_OFF)
    {
        cam_cfg_gpio_func(sensor_init->ircut_a.gpio_index, sensor_init->ircut_a.gpio_func);
        cam_cfg_gpio_func(sensor_init->ircut_b.gpio_index, sensor_init->ircut_b.gpio_func);
    }
    else if (on_or_off == IR_STATUS_CUT_ON)
    {
        cam_cfg_gpio_func(sensor_init->ircut_a.gpio_index, sensor_init->ircut_b.gpio_func);//exchange a&b
        cam_cfg_gpio_func(sensor_init->ircut_b.gpio_index, sensor_init->ircut_a.gpio_func);
    }

    return 0;
}"""
        new = """int32_t ircut_switch(struct sensor_init_cfg *sensor_init, IR_STATUS_t on_or_off)
{
    /* BADGE_GPIO_GUARD: la badge no tiene IR-cut; el meta usa GPIO_INVALID. */
    if (on_or_off == IR_STATUS_CUT_OFF)
    {
        if (sensor_init->ircut_a.gpio_index >= 0)
            cam_cfg_gpio_func(sensor_init->ircut_a.gpio_index, sensor_init->ircut_a.gpio_func);
        if (sensor_init->ircut_b.gpio_index >= 0)
            cam_cfg_gpio_func(sensor_init->ircut_b.gpio_index, sensor_init->ircut_b.gpio_func);
    }
    else if (on_or_off == IR_STATUS_CUT_ON)
    {
        if (sensor_init->ircut_a.gpio_index >= 0)
            cam_cfg_gpio_func(sensor_init->ircut_a.gpio_index, sensor_init->ircut_b.gpio_func);//exchange a&b
        if (sensor_init->ircut_b.gpio_index >= 0)
            cam_cfg_gpio_func(sensor_init->ircut_b.gpio_index, sensor_init->ircut_a.gpio_func);
    }

    return 0;
}"""
        if old in text:
            fast_ae.write_text(text.replace(old, new))
        else:
            print("WARN: no se pudo aplicar BADGE_GPIO_GUARD a fast_ae.c")
PY

msg_step "Ajustando permisos"
find "$SDK/project/cfg/BoardConfig_IPC/overlay/overlay-badge-fastboot" -type f \
     \( -name "*.sh" -o -path "*/init.d/*" \) -exec chmod +x {} + 2>/dev/null || true
[ -f "$SDK/project/cfg/BoardConfig_IPC/$POST_SCRIPT" ] && \
    chmod +x "$SDK/project/cfg/BoardConfig_IPC/$POST_SCRIPT"

msg_step "Seleccionando board config"
if [ -f "$SDK/project/cfg/BoardConfig_IPC/$BOARD_CONFIG" ]; then
    ln -sfn "project/cfg/BoardConfig_IPC/$BOARD_CONFIG" "$SDK/.BoardConfig.mk"
    msg_info "Board: $BOARD_CONFIG"
else
    msg_warn "Todavía no existe $BOARD_CONFIG; se omite la selección."
fi

msg_step "Capa aplicada en $SDK"
