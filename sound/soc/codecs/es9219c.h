/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ES9219C register map.
 */

#ifndef __SND_SOC_ES9219C_H
#define __SND_SOC_ES9219C_H

#include <linux/bits.h>

/* Register map: control registers */
#define ES9219C_SYSTEM			0x00
#define ES9219C_INPUT_SEL		0x01
#define ES9219C_MIX_AMUTE		0x02
#define ES9219C_ANALOGVOL		0x03
#define ES9219C_AMUTE_TIME		0x04
#define ES9219C_AMUTE_LEVEL		0x05
#define ES9219C_DOP_VOL_RAMP		0x06
#define ES9219C_FBW_MUTE		0x07
#define ES9219C_GPIO_CONFIG		0x08
#define ES9219C_MODE_SYNC		0x0a
#define ES9219C_OC_PROTECT		0x0b
#define ES9219C_DPLL_BANDWITH		0x0c
#define ES9219C_THD_MONO_MODE		0x0d
#define ES9219C_SS_CONFIG		0x0e
#define ES9219C_VOL_CH1			0x10 /* VOLUME_1 */
#define ES9219C_VOL_CH2			0x0f /* VOLUME_2 */
#define ES9219C_MASTER_TRIM		0x11 /* 32-bit */
#define ES9219C_GPIO_INPUT_SEL		0x15
#define ES9219C_THD_C2			0x16 /* 16-bit */
#define ES9219C_THD_C3			0x18 /* 16-bit */
#define ES9219C_CP_SS_DELAY		0x1a
#define ES9219C_GEN_CONFIG		0x1b
#define ES9219C_GPIO_CONFIG2		0x1d /* GPIO + auto clock gear */
#define ES9219C_CP_CLOCK		0x1e /* 16-bit */
#define ES9219C_AMP_CONFIG		0x20
#define ES9219C_IMASK			0x21
#define ES9219C_NCO			0x22 /* 32-bit */
#define ES9219C_FIR_ADDR		0x28
#define ES9219C_FIR_DATA		0x29 /* 24-bit */
#define ES9219C_FIR_CONFIG		0x2c
#define ES9219C_ANALOG_OV_1		0x2d
#define ES9219C_ANALOG_OV_2		0x2e
#define ES9219C_ANALOG_OV_3		0x2f
#define ES9219C_ANALOG_CTRL		0x30
#define ES9219C_ACG_THRESH		0x31 /* 24-bit */
#define ES9219C_THD_C2_CH2		0x35 /* 16-bit */
#define ES9219C_THD_C3_CH2		0x37 /* 16-bit */

/* Register map: status/readback registers */
#define ES9219C_CHIP_STATUS		0x40
#define ES9219C_GPIO_READBACK		0x41
#define ES9219C_DPLL_NUMBER		0x42 /* 32-bit */
#define ES9219C_INPUT_STATUS		0x48
#define ES9219C_RAM_READBACK		0x49 /* 24-bit */

/* Register map: extended control registers */
#define ES9219C_CROSSTALK_CONFIG	0x80
#define ES9219C_CT_SCALE_CH1		0x82 /* 16-bit */
#define ES9219C_CT_SCALE_CH2		0x84 /* 16-bit */
#define ES9219C_DRE_CONFIG		0x88

#define ES9219C_MAX_REGISTER		ES9219C_DRE_CONFIG

/* SYSTEM (0x00) */
#define ES9219C_SYSTEM_OSC_DRV		GENMASK(7, 4)
#define ES9219C_SYSTEM_CLK_GEAR		GENMASK(3, 2)

/* INPUT_SEL (0x01) */
#define ES9219C_INPUT_SEL_SER_LEN	GENMASK(7, 6)
#define ES9219C_INPUT_SEL_SER_MODE	GENMASK(5, 4)
#define ES9219C_INPUT_SEL_AUTOSELECT	GENMASK(3, 2)
#define ES9219C_INPUT_SEL_DECODER	GENMASK(1, 0)

/* FBW_MUTE (0x07) */
#define ES9219C_FBW_MUTE_MUTE		BIT(0)

/* Filter shape (0x07) */
#define ES9219C_FILTER_SHAPE		GENMASK(7, 5)

/* DoP/volume ramp (0x06) */
#define ES9219C_DOP_ENABLE		BIT(3)

/* MODE_SYNC (0x0a) */
#define ES9219C_MODE_SYNC_MASTER	BIT(7)

/* SS_CONFIG (0x0e) */
#define ES9219C_SS_CONFIG_TYPE		BIT(5)
#define ES9219C_SS_CONFIG_TIME		GENMASK(4, 0)

/* GEN_CONFIG (0x1b) */
#define ES9219C_GEN_CONFIG_ASRC_EN	BIT(7)

/* AMP_CONFIG (0x20) */
#define ES9219C_AMP_CONFIG_MODE		GENMASK(4, 3)
#define ES9219C_AMP_MODE_CORE_ON	0
#define ES9219C_AMP_MODE_LOWFI		1
#define ES9219C_AMP_MODE_HIFI_1V	2
#define ES9219C_AMP_MODE_HIFI_2V	3

/* CHIP_STATUS (0x40) */
#define ES9219C_CHIP_ID_MASK		GENMASK(7, 2)
#define ES9219C_CHIP_ID			0x23

/* CROSSTALK_CONFIG (0x80) */
#define ES9219C_CT_BYPASS		BIT(7)

/* DRE_CONFIG (0x88) */
#define ES9219C_ENABLE_DRE		BIT(0)

#endif /* __SND_SOC_ES9219C_H */
