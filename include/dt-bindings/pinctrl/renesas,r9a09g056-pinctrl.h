/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides constants for Renesas RZ/V2N SoC pinctrl bindings.
 *
 * Copyright (C) 2025 Renesas Electronics Corp.
 */

#ifndef __DT_BINDINGS_PINCTRL_RENESAS_R9A09G056_PINCTRL_H__
#define __DT_BINDINGS_PINCTRL_RENESAS_R9A09G056_PINCTRL_H__

#include <dt-bindings/pinctrl/rzg2l-pinctrl.h>

/* RZV2N_Px = Offset address of PFC_P_mn  - 0x20 */
#define RZV2N_P0	0
#define RZV2N_P1	1
#define RZV2N_P2	2
#define RZV2N_P3	3
#define RZV2N_P4	4
#define RZV2N_P5	5
#define RZV2N_P6	6
#define RZV2N_P7	7
#define RZV2N_P8	8
#define RZV2N_P9	9
#define RZV2N_PA	10
#define RZV2N_PB	11

#define RZV2N_PORT_PINMUX(b, p, f)	RZG2L_PORT_PINMUX(RZV2N_P##b, p, f)
#define RZV2N_GPIO(port, pin)		RZG2L_GPIO(RZV2N_P##port, pin)

#endif /* __DT_BINDINGS_PINCTRL_RENESAS_R9A09G056_PINCTRL_H__ */
