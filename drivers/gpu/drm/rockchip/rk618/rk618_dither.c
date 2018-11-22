// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2017 Rockchip Electronics Co. Ltd.
 *
 * Author: Wyon Bi <bivvy.bi@rock-chips.com>
 */

#include "rk618_dither.h"

#define RK618_FRC_REG			0x0054
#define FRC_DEN_INV			HIWORD_UPDATE(1, 6, 6)
#define FRC_SYNC_INV			HIWORD_UPDATE(1, 5, 5)
#define FRC_DCLK_INV			HIWORD_UPDATE(1, 4, 4)
#define FRC_OUT_ZERO			HIWORD_UPDATE(1, 3, 3)
#define FRC_OUT_MODE_RGB666		HIWORD_UPDATE(1, 2, 2)
#define FRC_OUT_MODE_RGB888		HIWORD_UPDATE(0, 2, 2)
#define FRC_DITHER_MODE_HI_FRC		HIWORD_UPDATE(1, 1, 1)
#define FRC_DITHER_MODE_FRC		HIWORD_UPDATE(0, 1, 1)
#define FRC_DITHER_ENABLE		HIWORD_UPDATE(1, 0, 0)
#define FRC_DITHER_DISABLE		HIWORD_UPDATE(0, 0, 0)

void rk618_frc_dither_disable(struct rk618 *rk618)
{
	regmap_write(rk618->regmap, RK618_FRC_REG, FRC_DITHER_DISABLE);
}
EXPORT_SYMBOL_GPL(rk618_frc_dither_disable);

void rk618_frc_dither_enable(struct rk618 *rk618)
{
	regmap_write(rk618->regmap, RK618_FRC_REG, FRC_DITHER_ENABLE);
}
EXPORT_SYMBOL_GPL(rk618_frc_dither_enable);

void rk618_frc_dclk_invert(struct rk618 *rk618)
{
	regmap_write(rk618->regmap, RK618_FRC_REG, FRC_DCLK_INV);
}
EXPORT_SYMBOL_GPL(rk618_frc_dclk_invert);
