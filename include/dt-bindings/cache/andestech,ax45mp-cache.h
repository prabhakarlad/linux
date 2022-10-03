/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides constants for Andes AX45MP PMA configuration
 *
 * Copyright (C) 2022 Renesas Electronics Corp.
 */

#ifndef __DT_BINDINGS_ANDESTECH_AX45MP_CACHE_H
#define __DT_BINDINGS_ANDESTECH_AX45MP_CACHE_H

/* OFF: PMA entry is disabled */
#define AX45MP_PMACFG_ETYP_DISABLED			0
/* Naturally aligned power of 2 region */
#define AX45MP_PMACFG_ETYP_NAPOT			3

/* Device, Non-bufferable */
#define AX45MP_PMACFG_MTYP_DEV_NON_BUF			(0 << 2)
/* Device, bufferable */
#define AX45MP_PMACFG_MTYP_DEV_BUF			(1 << 2)
/* Memory, Non-cacheable, Non-bufferable */
#define AX45MP_PMACFG_MTYP_MEM_NON_CACHE_NON_BUF	(2 << 2)
/* Memory, Non-cacheable, Bufferable */
#define AX45MP_PMACFG_MTYP_MEM_NON_CACHE_BUF		(3 << 2)
/* Memory, Write-back, No-allocate */
#define AX45MP_PMACFG_MTYP_MEM_WB_NA			(8 << 2)
/* Memory, Write-back, Read-allocate */
#define AX45MP_PMACFG_MTYP_MEM_WB_RA			(9 << 2)
/* Memory, Write-back, Write-allocate */
#define AX45MP_PMACFG_MTYP_MEM_WB_WA			(10 << 2)
/* Memory, Write-back, Read and Write-allocate */
#define AX45MP_PMACFG_MTYP_MEM_WB_R_WA			(11 << 2)

/* AMO instructions are supported */
#define AX45MP_PMACFG_NAMO_AMO_SUPPORT			(0 << 6)
/* AMO instructions are not supported */
#define AX45MP_PMACFG_NAMO_AMO_NO_SUPPORT		(1 << 6)

#endif /* __DT_BINDINGS_ANDESTECH_AX45MP_CACHE_H */
