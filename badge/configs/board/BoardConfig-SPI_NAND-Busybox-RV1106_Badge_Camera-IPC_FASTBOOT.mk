#!/bin/bash
#
# File: configs/board/BoardConfig-SPI_NAND-Busybox-RV1106_Badge_Camera-IPC_FASTBOOT.mk
# Purpose: Board configuration for the RV1106 badge fastboot image.
#
#
# Badge Hack GDL 2026 — RV1106 + SPI NAND + SC3336
# Fastboot / thunder boot image with a minimal photo application.
#

#################################################
# Board Config
#################################################
export LF_ORIGIN_BOARD_CONFIG=BoardConfig-SPI_NAND-Busybox-RV1106_Badge_Camera-IPC_FASTBOOT.mk

# Target CHIP
export RK_CHIP=rv1106

# Application installed by this layer
export RK_APP_TYPE="BADGE_PHOTO"

# CMA for VI/VENC/ISP
export RK_BOOTARGS_CMA_SIZE="32M"

# Kernel DTS
export RK_KERNEL_DTS=rv1106g-badge-fastboot.dts

#################################################
# BOOT MEDIUM
#################################################
export RK_BOOT_MEDIUM=spi_nand

# U-Boot defconfig fragment
export RK_UBOOT_DEFCONFIG_FRAGMENT=rk-sfc.config

export LF_ENABLE_SPI_NAND_FAST_BOOT=y

# 256K env, 1M idblock, 512K uboot, 2M meta, 32M boot, 96M userdata.
# The remaining NAND is left unused for now.
export RK_PARTITION_CMD_IN_ENV="256K(env),1M@256K(idblock),512K(uboot),2M(meta),32M(boot),96M(userdata)"

export RK_META_SIZE=393216

# Boot contains kernel + erofs rootfs; userdata is writable UBIFS.
export RK_PARTITION_FS_TYPE_CFG=boot@IGNORE@erofs,userdata@/userdata@ubifs

#################################################
# ROOTFS
#################################################
export LF_TARGET_ROOTFS=busybox

#################################################
# DEFCONFIG
#################################################
export RK_ARCH=arm
export RK_TOOLCHAIN_CROSS=arm-rockchip830-linux-uclibcgnueabihf

export RK_MISC=wipe_all-misc.img

export RK_UBOOT_DEFCONFIG=luckfox_rv1106_spi_nand_tb_defconfig

# Compile and embed the RT-Thread MCU firmware for the SC3336 board.
export RK_UBOOT_RKBIN_MCU_CFG=rv1106_evb-SC3336-ADC

export RK_KERNEL_DEFCONFIG=luckfox_rv1106_linux_tb_defconfig
export RK_KERNEL_DEFCONFIG_FRAGMENT="luckfox_rv1106-tb.config rv1106-smart-door.config rv1106-nand.config rv1106-badge.config"

# Binary IQ file consumed by the MCU fast AE/AWB.
export RK_CAMERA_SENSOR_IQFILES="sc3336_CMK-OT2119-PC1_30IRC-F16.bin"

# Keep apps inside the rootfs/oem tree (fastboot image).
export RK_BUILD_APP_TO_OEM_PARTITION=n

export RK_AIISP_MODEL=NONE
export RK_AUDIO_MODEL=NONE
export RK_NPU_MODEL=NONE

# Debug tools not needed in the fastboot image.
export RK_ENABLE_GDB=n
export RK_ENABLE_EUDEV=n
# ADB is kept for hardware automation/debugging (MTP + ADB gadget).
export RK_ENABLE_ADBD=y

#################################################
# PRE / POST / OVERLAY
#################################################
export RK_POST_BUILD_SCRIPT=luckfox-rv1106-badge-tb-spi_nand-post.sh
export RK_PRE_BUILD_OEM_SCRIPT=luckfox-rv1106-tb-pre-oem.sh

export RK_ENABLE_FASTBOOT=y
export RK_POST_OVERLAY=overlay-badge-fastboot
export RK_TINY_META=n

# Camera behaviour read by the MCU and the Linux app.
export RK_META_PARAM=" \
 --rk_cam_mirror_flip=3 \
 --rk_cam_fps=25 \
 --rk_cam_hdr=0 \
 --rk_led_value=0 \
 --rk_night_mode=0 \
 --rk_color_mode=0 \
"

# Kernel-native RNDIS is disabled: the badge uses the validated RNDIS gadget
# modules shipped in the overlay (oem/usr/ko) with the stable kernel.
export RK_ENABLE_RNDIS=n
