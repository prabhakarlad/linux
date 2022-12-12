// SPDX-License-Identifier: GPL-2.0-only
/*
 * Erratas to be applied for Andes CPU cores
 *
 *  Copyright (C) 2022 Renesas Electronics Corporation.
 *
 * Author: Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>

#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/errata_list.h>
#include <asm/patch.h>
#include <asm/sbi.h>
#include <asm/vendorid_list.h>

#define ANDESTECH_AX45MP_MARCHID	0x8000000000008a45UL
#define ANDESTECH_AX45MP_MIMPID		0x500UL
#define ANDESTECH_SBI_EXT_ANDES		0x0900031E

#define RZFIVE_SBI_EXT_IOCP_SW_WORKAROUND	0

static long ax45mp_iocp_sw_workaround(void)
{
	struct sbiret ret;

	ret = sbi_ecall(ANDESTECH_SBI_EXT_ANDES, RZFIVE_SBI_EXT_IOCP_SW_WORKAROUND,
			0, 0, 0, 0, 0, 0);

	return ret.error ? 0 : ret.value;
}

static bool errata_probe_iocp(unsigned int stage, unsigned long arch_id, unsigned long impid)
{
	if (!IS_ENABLED(CONFIG_ERRATA_ANDES_CMO))
		return false;

	if (arch_id != ANDESTECH_AX45MP_MARCHID || impid != ANDESTECH_AX45MP_MIMPID)
		return false;

	if (!ax45mp_iocp_sw_workaround())
		return false;

	/* Set this just to make core cbo code happy */
	riscv_cbom_block_size = 1;
	riscv_noncoherent_supported();

	return true;
}

static u32 andes_errata_probe(unsigned int stage, unsigned long archid, unsigned long impid)
{
	u32 cpu_req_errata = 0;

	/*
	 * In the absence of the I/O Coherency Port, access to certain peripherals
	 * requires vendor specific DMA handling.
	 */
	if (errata_probe_iocp(stage, archid, impid))
		cpu_req_errata |= BIT(ERRATA_ANDESTECH_NO_IOCP);

	return cpu_req_errata;
}

void __init_or_module andes_errata_patch_func(struct alt_entry *begin, struct alt_entry *end,
					      unsigned long archid, unsigned long impid,
					      unsigned int stage)
{
	u32 cpu_req_errata = andes_errata_probe(stage, archid, impid);
	struct alt_entry *alt;
	u32 tmp;

	if (stage == RISCV_ALTERNATIVES_EARLY_BOOT)
		return;

	for (alt = begin; alt < end; alt++) {
		if (alt->vendor_id != ANDESTECH_VENDOR_ID)
			continue;
		if (alt->errata_id >= ERRATA_ANDESTECH_NUMBER)
			continue;

		tmp = BIT(alt->errata_id);
		if (cpu_req_errata & tmp) {
			patch_text_nosync(alt->old_ptr, alt->alt_ptr, alt->alt_len);

			riscv_alternative_fix_offsets(alt->old_ptr, alt->alt_len,
						      alt->old_ptr - alt->alt_ptr);
		}
	}
}
