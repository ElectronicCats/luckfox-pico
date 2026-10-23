/*
 * File: mcu/board/rv1106_evb-SC3336-ADC/board.c
 * Purpose: RT-Thread board camera/ISP hardware configuration for the badge.
 */

/*
 * Copyright (c) 2022 Rockchip Electronics Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Badge Hack GDL 2026 — RV1106 + SC3336.
 *
 * The SC3336 driver uses the GPIO definitions from board_base.h
 * (reset on GPIO3_C5), so this file only configures the MIPI/ISP path.
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_ISP3
#include "drv_isp3.h"

void cif_hw_config(void)
{
    struct dphy_board_desc *csi2_dphy = (struct dphy_board_desc *)&csi2_dphy_board;
    struct csi2_board_desc *csi_host_0 = (struct csi2_board_desc *)&csi2_host0_board;
    struct vicap_board_desc *vicap = (struct vicap_board_desc *)&vicap_board;

    csi2_dphy->enable = true;
    csi2_dphy->csi2_dphy1.enable = true;
    csi2_dphy->csi2_dphy1.csi_host_idx = 0;
    csi2_dphy->csi2_dphy1.data_lanes = 2;
    csi2_dphy->csi2_dphy1.isp_subdev_name = RK_ISP_MAIN_SUBDEV_NAME;

    csi_host_0->enable = true;

    vicap->enable = true;
}
#endif
