// SPDX-License-Identifier: GPL-2.0-only
/*
 * Erratas to be applied for Andes CPU cores
 *
 *  Copyright (C) 2023 Renesas Electronics Corporation.
 *
 * Author: Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>
 */

#include <linux/memory.h>
#include <linux/module.h>

#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/errata_list.h>
#include <asm/patch.h>
#include <asm/processor.h>
#include <asm/sbi.h>
#include <asm/vendorid_list.h>

#define ANDESTECH_AX45MP_MARCHID	0x8000000000008a45UL
#define ANDESTECH_AX45MP_MIMPID		0x500UL
#define ANDESTECH_SBI_EXT_ANDES		0x0900031E

#define ANDES_SBI_EXT_IOCP_SW_WORKAROUND	1

static long ax45mp_iocp_sw_workaround(void)
{
	struct sbiret ret;

	/*
	 * ANDES_SBI_EXT_IOCP_SW_WORKAROUND SBI EXT checks if the IOCP is missing and
	 * cache is controllable only then CMO will be applied to the platform.
	 */
	ret = sbi_ecall(ANDESTECH_SBI_EXT_ANDES, ANDES_SBI_EXT_IOCP_SW_WORKAROUND,
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
	void *oldptr, *altptr;
	struct alt_entry *alt;
	u32 tmp;

	if (stage == RISCV_ALTERNATIVES_EARLY_BOOT)
		return;

	for (alt = begin; alt < end; alt++) {
		if (alt->vendor_id != ANDESTECH_VENDOR_ID)
			continue;
		if (alt->patch_id >= ERRATA_ANDESTECH_NUMBER)
			continue;

		tmp = BIT(alt->patch_id);
		if (cpu_req_errata & tmp) {
			oldptr = ALT_OLD_PTR(alt);
			altptr = ALT_ALT_PTR(alt);

			mutex_lock(&text_mutex);
			patch_text_nosync(oldptr, altptr, alt->alt_len);

			riscv_alternative_fix_offsets(oldptr, alt->alt_len,
						      oldptr - altptr);
			mutex_unlock(&text_mutex);
		}
	}
}
