// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Renesas Electronics Corp.
 */

#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/errata_list.h>
#include <asm/patch.h>
#include <asm/vendorid_list.h>
#include <asm/sbi.h>

#include <asm/parse_asm.h>

/* Copy of Heiko's code from patch [0]
 * [0] https://patchwork.kernel.org/project/linux-riscv/patch/20221110164924.529386-6-heiko@sntech.de/
 */
DECLARE_INSN(jalr, MATCH_JALR, MASK_JALR)
DECLARE_INSN(auipc, MATCH_AUIPC, MASK_AUIPC)

static inline bool is_auipc_jalr_pair(long insn1, long insn2)
{
	return is_auipc_insn(insn1) && is_jalr_insn(insn2);
}

#define JALR_SIGN_MASK		BIT(I_IMM_SIGN_OPOFF - I_IMM_11_0_OPOFF)
#define JALR_OFFSET_MASK	I_IMM_11_0_MASK
#define AUIPC_OFFSET_MASK	U_IMM_31_12_MASK
#define AUIPC_PAD		(0x00001000)
#define JALR_SHIFT		I_IMM_11_0_OPOFF

#define to_jalr_imm(offset)						\
	((offset & I_IMM_11_0_MASK) << I_IMM_11_0_OPOFF)

#define to_auipc_imm(offset)						\
	((offset & JALR_SIGN_MASK) ?					\
	((offset & AUIPC_OFFSET_MASK) + AUIPC_PAD) :	\
	(offset & AUIPC_OFFSET_MASK))

static void riscv_alternative_print_old_inst(unsigned int *alt_ptr,
					     unsigned int len)
{
	int num_instr = len / sizeof(u32);
	unsigned int i;

	for (i = 0; i < num_instr; i++)
		pr_err("%s instruction: 0x%x\n", __func__, *(alt_ptr + i));
}

static void riscv_alternative_print_patched_inst(unsigned int *alt_ptr,
						 unsigned int len)
{
	int num_instr = len / sizeof(u32);
	unsigned int i;

	for (i = 0; i < num_instr; i++)
		pr_err("%s instruction: 0x%x\n", __func__, *(alt_ptr + i));
}

static void riscv_alternative_fix_auipc_jalr(unsigned int *alt_ptr,
					     unsigned int len, int patch_offset)
{
	int num_instr = len / sizeof(u32);
	unsigned int call[2];
	int i;
	int imm1;
	u32 rd1;

	for (i = 0; i < num_instr; i++) {
		/* is there a further instruction? */
		if (i + 1 >= num_instr)
			continue;

		if (!is_auipc_jalr_pair(*(alt_ptr + i), *(alt_ptr + i + 1)))
			continue;

		/* call will use ra register */
		rd1 = EXTRACT_RD_REG(*(alt_ptr + i));
		if (rd1 != 1)
			continue;

		/* get and adjust new target address */
		imm1 = EXTRACT_UTYPE_IMM(*(alt_ptr + i));
		imm1 += EXTRACT_ITYPE_IMM(*(alt_ptr + i + 1));
		imm1 -= patch_offset;

		/* pick the original auipc + jalr */
		call[0] = *(alt_ptr + i);
		call[1] = *(alt_ptr + i + 1);

		/* drop the old IMMs */
		call[0] &= ~(U_IMM_31_12_MASK);
		call[1] &= ~(I_IMM_11_0_MASK << I_IMM_11_0_OPOFF);

		/* add the adapted IMMs */
		call[0] |= to_auipc_imm(imm1);
		call[1] |= to_jalr_imm(imm1);

		/* patch the call place again */
		patch_text_nosync(alt_ptr + i * sizeof(u32), call, 8);
	}
}

static bool errata_probe_iocp(unsigned int stage, unsigned long arch_id, unsigned long impid)
{
	if (!IS_ENABLED(CONFIG_ERRATA_RZFIVE_CMO))
		return false;

	if (arch_id != 0x8000000000008a45 || impid != 0x500)
		return false;

	riscv_cbom_block_size = 1;
	riscv_noncoherent_supported();

	return true;
}

static u32 rzfive_errata_probe(unsigned int stage,
			      unsigned long archid, unsigned long impid)
{
	u32 cpu_req_errata = 0;

	if (errata_probe_iocp(stage, archid, impid))
		cpu_req_errata |= BIT(ERRATA_ANDESTECH_NO_IOCP);

	return cpu_req_errata;
}

void __init_or_module andes_errata_patch_func(struct alt_entry *begin, struct alt_entry *end,
					      unsigned long archid, unsigned long impid,
					      unsigned int stage)
{
	u32 cpu_req_errata = rzfive_errata_probe(stage, archid, impid);
	struct alt_entry *alt;
	u32 tmp;

	if (stage == RISCV_ALTERNATIVES_EARLY_BOOT)
		return;

	for (alt = begin; alt < end; alt++) {
		if (alt->vendor_id != ANDESTECH_VENDOR_ID)
			continue;
		if (alt->errata_id >= ERRATA_ANDESTECH_NUMBER)
			continue;

		tmp = (1U << alt->errata_id);
		if (cpu_req_errata & tmp) {
#if 0
			pr_err("stage: %x -> %px--> %x %x %x\n", stage, alt, tmp, cpu_req_errata, alt->errata_id);
			pr_err("old:%ps alt:%ps len:%lx\n", alt->old_ptr, alt->alt_ptr, alt->alt_len);
#endif
			pr_err("Print before patching start\n");
			riscv_alternative_print_old_inst(alt->old_ptr, alt->alt_len);
			pr_err("Print before patching end\n");
			patch_text_nosync(alt->old_ptr, alt->alt_ptr, alt->alt_len);

			riscv_alternative_fix_auipc_jalr(alt->old_ptr, alt->alt_len,
							 alt->old_ptr - alt->alt_ptr);
			pr_err("Print after patching start\n");
			riscv_alternative_print_patched_inst(alt->old_ptr, alt->alt_len);
			pr_err("Print after patching end\n");
		}
	}
}
