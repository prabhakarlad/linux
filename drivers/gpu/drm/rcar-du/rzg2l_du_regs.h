/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RZ/G2L LCDC Registers Definitions
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 */

#ifndef __RZG2L_DU_REGS_H__
#define __RZG2L_DU_REGS_H__

/* -----------------------------------------------------------------------------
 * RZ/G2L Display Registers
 */

#define DU_MCR0			0x00
#define DU_MCR0_DPI_OE		BIT(0)
#define DU_MCR0_DI_EN		BIT(8)
#define DU_MCR0_PB_CLR		BIT(16)

#define DU_MSR0			0x04
#define DU_MSR0_ST_DI_BSY	BIT(8)
#define DU_MSR0_ST_PB_WFULL	BIT(16)
#define DU_MSR0_ST_PB_WINIT	BIT(18)
#define DU_MSR0_ST_PB_REMPTY	BIT(20)
#define DU_MSR0_ST_PB_RUF	BIT(21)
#define DU_MSR0_ST_PB_RINIT	BIT(22)

#define DU_MSR1			0x08

#define DU_IMR0			0x0c
#define DU_MSR0_IM_PB_RUF	BIT(0)

#define DU_DITR0		0x10
#define DU_DITR0_DPI_CLKMD	BIT(0)
#define DU_DITR0_DEMD_LOW	0x0
#define DU_DITR0_DEMD_HIGH	(BIT(8) | BIT(9))
#define DU_DITR0_VSPOL		BIT(16)
#define DU_DITR0_HSPOL		BIT(17)

#define DU_DITR1		0x14
#define DU_DITR1_VSA(x)		((x) << 0)
#define DU_DITR1_VACTIVE(x)	((x) << 16)

#define DU_DITR2		0x18
#define DU_DITR2_VBP(x)		((x) << 0)
#define DU_DITR2_VFP(x)		((x) << 16)

#define DU_DITR3		0x1c
#define DU_DITR3_HSA(x)		((x) << 0)
#define DU_DITR3_HACTIVE(x)	((x) << 16)

#define DU_DITR4		0x20
#define DU_DITR4_HBP(x)		((x) << 0)
#define DU_DITR4_HFP(x)		((x) << 16)

#define DU_DITR5		0x24
#define DU_DITR5_VSFT(x)	((x) << 0)
#define DU_DITR5_HSFT(x)	((x) << 16)

#define DU_MCR1			0x40
#define DU_MCR1_PB_AUTOCLR	BIT(16)

#define DU_PBCR0		0x4c
#define DU_PBCR0_PB_DEP(x)	((x) << 0)

#endif /* __RZG2L_DU_REGS_H__ */
