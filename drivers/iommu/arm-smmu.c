/*
 * IOMMU API for ARM architected SMMU implementations.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 *
 * This driver currently supports:
 *	- SMMUv1 and v2 implementations
 *	- Stream-matching and stream-indexing
 *	- v7/v8 long-descriptor format
 *	- Non-secure access to the SMMU
 *	- 4k and 64k pages, with contiguous pte hints.
 *	- Up to 42-bit addressing (dependent on VA_BITS)
 *	- Context fault reporting
 */

#define pr_fmt(fmt) "arm-smmu: " fmt

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <linux/amba/bus.h>

#include <asm/pgalloc.h>

/* Maximum number of stream IDs assigned to a single device */
#define MAX_MASTER_STREAMIDS		MAX_PHANDLE_ARGS

/* Maximum number of context banks per SMMU */
#define ARM_SMMU_MAX_CBS		128

/* Maximum number of mapping groups per SMMU */
#define ARM_SMMU_MAX_SMRS		128

/* SMMU global address space */
#define ARM_SMMU_GR0(smmu)		((smmu)->base)
#define ARM_SMMU_GR1(smmu)		((smmu)->base + (smmu)->pagesize)

/*
 * SMMU global address space with conditional offset to access secure
 * aliases of non-secure registers (e.g. nsCR0: 0x400, nsGFSR: 0x448,
 * nsGFSYNR0: 0x450)
 */
#define ARM_SMMU_GR0_NS(smmu)						\
	((smmu)->base +							\
		((smmu->options & ARM_SMMU_OPT_SECURE_CFG_ACCESS)	\
			? 0x400 : 0))

/* Page table bits */
#define ARM_SMMU_PTE_XN			(((pteval_t)3) << 53)
#define ARM_SMMU_PTE_CONT		(((pteval_t)1) << 52)
#define ARM_SMMU_PTE_AF			(((pteval_t)1) << 10)
#define ARM_SMMU_PTE_SH_NS		(((pteval_t)0) << 8)
#define ARM_SMMU_PTE_SH_OS		(((pteval_t)2) << 8)
#define ARM_SMMU_PTE_SH_IS		(((pteval_t)3) << 8)
#define ARM_SMMU_PTE_PAGE		(((pteval_t)3) << 0)

#if PAGE_SIZE == SZ_4K
#define ARM_SMMU_PTE_CONT_ENTRIES	16
#elif PAGE_SIZE == SZ_64K
#define ARM_SMMU_PTE_CONT_ENTRIES	32
#else
#define ARM_SMMU_PTE_CONT_ENTRIES	1
#endif

#define ARM_SMMU_PTE_CONT_SIZE		(PAGE_SIZE * ARM_SMMU_PTE_CONT_ENTRIES)
#define ARM_SMMU_PTE_CONT_MASK		(~(ARM_SMMU_PTE_CONT_SIZE - 1))

/* Stage-1 PTE */
#define ARM_SMMU_PTE_AP_UNPRIV		(((pteval_t)1) << 6)
#define ARM_SMMU_PTE_AP_RDONLY		(((pteval_t)2) << 6)
#define ARM_SMMU_PTE_ATTRINDX_SHIFT	2
#define ARM_SMMU_PTE_nG			(((pteval_t)1) << 11)

/* Stage-2 PTE */
#define ARM_SMMU_PTE_HAP_FAULT		(((pteval_t)0) << 6)
#define ARM_SMMU_PTE_HAP_READ		(((pteval_t)1) << 6)
#define ARM_SMMU_PTE_HAP_WRITE		(((pteval_t)2) << 6)
#define ARM_SMMU_PTE_MEMATTR_OIWB	(((pteval_t)0xf) << 2)
#define ARM_SMMU_PTE_MEMATTR_NC		(((pteval_t)0x5) << 2)
#define ARM_SMMU_PTE_MEMATTR_DEV	(((pteval_t)0x1) << 2)

/* Configuration registers */
#define ARM_SMMU_GR0_sCR0		0x0
#define sCR0_CLIENTPD			(1 << 0)
#define sCR0_GFRE			(1 << 1)
#define sCR0_GFIE			(1 << 2)
#define sCR0_GCFGFRE			(1 << 4)
#define sCR0_GCFGFIE			(1 << 5)
#define sCR0_USFCFG			(1 << 10)
#define sCR0_VMIDPNE			(1 << 11)
#define sCR0_PTM			(1 << 12)
#define sCR0_FB				(1 << 13)
#define sCR0_BSU_SHIFT			14
#define sCR0_BSU_MASK			0x3

/* Identification registers */
#define ARM_SMMU_GR0_ID0		0x20
#define ARM_SMMU_GR0_ID1		0x24
#define ARM_SMMU_GR0_ID2		0x28
#define ARM_SMMU_GR0_ID3		0x2c
#define ARM_SMMU_GR0_ID4		0x30
#define ARM_SMMU_GR0_ID5		0x34
#define ARM_SMMU_GR0_ID6		0x38
#define ARM_SMMU_GR0_ID7		0x3c
#define ARM_SMMU_GR0_sGFSR		0x48
#define ARM_SMMU_GR0_sGFSYNR0		0x50
#define ARM_SMMU_GR0_sGFSYNR1		0x54
#define ARM_SMMU_GR0_sGFSYNR2		0x58
#define ARM_SMMU_GR0_PIDR0		0xfe0
#define ARM_SMMU_GR0_PIDR1		0xfe4
#define ARM_SMMU_GR0_PIDR2		0xfe8

#define ID0_S1TS			(1 << 30)
#define ID0_S2TS			(1 << 29)
#define ID0_NTS				(1 << 28)
#define ID0_SMS				(1 << 27)
#define ID0_PTFS_SHIFT			24
#define ID0_PTFS_MASK			0x2
#define ID0_PTFS_V8_ONLY		0x2
#define ID0_CTTW			(1 << 14)
#define ID0_NUMIRPT_SHIFT		16
#define ID0_NUMIRPT_MASK		0xff
#define ID0_NUMSMRG_SHIFT		0
#define ID0_NUMSMRG_MASK		0xff

#define ID1_PAGESIZE			(1 << 31)
#define ID1_NUMPAGENDXB_SHIFT		28
#define ID1_NUMPAGENDXB_MASK		7
#define ID1_NUMS2CB_SHIFT		16
#define ID1_NUMS2CB_MASK		0xff
#define ID1_NUMCB_SHIFT			0
#define ID1_NUMCB_MASK			0xff

#define ID2_OAS_SHIFT			4
#define ID2_OAS_MASK			0xf
#define ID2_IAS_SHIFT			0
#define ID2_IAS_MASK			0xf
#define ID2_UBS_SHIFT			8
#define ID2_UBS_MASK			0xf
#define ID2_PTFS_4K			(1 << 12)
#define ID2_PTFS_16K			(1 << 13)
#define ID2_PTFS_64K			(1 << 14)

#define PIDR2_ARCH_SHIFT		4
#define PIDR2_ARCH_MASK			0xf

/* Global TLB invalidation */
#define ARM_SMMU_GR0_STLBIALL		0x60
#define ARM_SMMU_GR0_TLBIVMID		0x64
#define ARM_SMMU_GR0_TLBIALLNSNH	0x68
#define ARM_SMMU_GR0_TLBIALLH		0x6c
#define ARM_SMMU_GR0_sTLBGSYNC		0x70
#define ARM_SMMU_GR0_sTLBGSTATUS	0x74
#define sTLBGSTATUS_GSACTIVE		(1 << 0)
#define TLB_LOOP_TIMEOUT		1000000	/* 1s! */

/* Stream mapping registers */
#define ARM_SMMU_GR0_SMR(n)		(0x800 + ((n) << 2))
#define SMR_VALID			(1 << 31)
#define SMR_MASK_SHIFT			16
#define SMR_MASK_MASK			0x7fff
#define SMR_ID_SHIFT			0
#define SMR_ID_MASK			0x7fff

#define ARM_SMMU_GR0_S2CR(n)		(0xc00 + ((n) << 2))
#define S2CR_CBNDX_SHIFT		0
#define S2CR_CBNDX_MASK			0xff
#define S2CR_TYPE_SHIFT			16
#define S2CR_TYPE_MASK			0x3
#define S2CR_TYPE_TRANS			(0 << S2CR_TYPE_SHIFT)
#define S2CR_TYPE_BYPASS		(1 << S2CR_TYPE_SHIFT)
#define S2CR_TYPE_FAULT			(2 << S2CR_TYPE_SHIFT)

/* Context bank attribute registers */
#define ARM_SMMU_GR1_CBAR(n)		(0x0 + ((n) << 2))
#define CBAR_VMID_SHIFT			0
#define CBAR_VMID_MASK			0xff
#define CBAR_S1_BPSHCFG_SHIFT		8
#define CBAR_S1_BPSHCFG_MASK		3
#define CBAR_S1_BPSHCFG_NSH		3
#define CBAR_S1_MEMATTR_SHIFT		12
#define CBAR_S1_MEMATTR_MASK		0xf
#define CBAR_S1_MEMATTR_WB		0xf
#define CBAR_TYPE_SHIFT			16
#define CBAR_TYPE_MASK			0x3
#define CBAR_TYPE_S2_TRANS		(0 << CBAR_TYPE_SHIFT)
#define CBAR_TYPE_S1_TRANS_S2_BYPASS	(1 << CBAR_TYPE_SHIFT)
#define CBAR_TYPE_S1_TRANS_S2_FAULT	(2 << CBAR_TYPE_SHIFT)
#define CBAR_TYPE_S1_TRANS_S2_TRANS	(3 << CBAR_TYPE_SHIFT)
#define CBAR_IRPTNDX_SHIFT		24
#define CBAR_IRPTNDX_MASK		0xff

#define ARM_SMMU_GR1_CBA2R(n)		(0x800 + ((n) << 2))
#define CBA2R_RW64_32BIT		(0 << 0)
#define CBA2R_RW64_64BIT		(1 << 0)

/* Translation context bank */
#define ARM_SMMU_CB_BASE(smmu)		((smmu)->base + ((smmu)->size >> 1))
#define ARM_SMMU_CB(smmu, n)		((n) * (smmu)->pagesize)

#define ARM_SMMU_CB_SCTLR		0x0
#define ARM_SMMU_CB_RESUME		0x8
#define ARM_SMMU_CB_TTBCR2		0x10
#define ARM_SMMU_CB_TTBR0_LO		0x20
#define ARM_SMMU_CB_TTBR0_HI		0x24
#define ARM_SMMU_CB_TTBCR		0x30
#define ARM_SMMU_CB_S1_MAIR0		0x38
#define ARM_SMMU_CB_FSR			0x58
#define ARM_SMMU_CB_FAR_LO		0x60
#define ARM_SMMU_CB_FAR_HI		0x64
#define ARM_SMMU_CB_FSYNR0		0x68
#define ARM_SMMU_CB_S1_TLBIASID		0x610

#define SCTLR_S1_ASIDPNE		(1 << 12)
#define SCTLR_CFCFG			(1 << 7)
#define SCTLR_CFIE			(1 << 6)
#define SCTLR_CFRE			(1 << 5)
#define SCTLR_E				(1 << 4)
#define SCTLR_AFE			(1 << 2)
#define SCTLR_TRE			(1 << 1)
#define SCTLR_M				(1 << 0)
#define SCTLR_EAE_SBOP			(SCTLR_AFE | SCTLR_TRE)

#define RESUME_RETRY			(0 << 0)
#define RESUME_TERMINATE		(1 << 0)

#define TTBCR_EAE			(1 << 31)

#define TTBCR_PASIZE_SHIFT		16
#define TTBCR_PASIZE_MASK		0x7

#define TTBCR_TG0_4K			(0 << 14)
#define TTBCR_TG0_64K			(1 << 14)

#define TTBCR_SH0_SHIFT			12
#define TTBCR_SH0_MASK			0x3
#define TTBCR_SH_NS			0
#define TTBCR_SH_OS			2
#define TTBCR_SH_IS			3

#define TTBCR_ORGN0_SHIFT		10
#define TTBCR_IRGN0_SHIFT		8
#define TTBCR_RGN_MASK			0x3
#define TTBCR_RGN_NC			0
#define TTBCR_RGN_WBWA			1
#define TTBCR_RGN_WT			2
#define TTBCR_RGN_WB			3

#define TTBCR_SL0_SHIFT			6
#define TTBCR_SL0_MASK			0x3
#define TTBCR_SL0_LVL_2			0
#define TTBCR_SL0_LVL_1			1

#define TTBCR_T1SZ_SHIFT		16
#define TTBCR_T0SZ_SHIFT		0
#define TTBCR_SZ_MASK			0xf

#define TTBCR2_SEP_SHIFT		15
#define TTBCR2_SEP_MASK			0x7

#define TTBCR2_PASIZE_SHIFT		0
#define TTBCR2_PASIZE_MASK		0x7

/* Common definitions for PASize and SEP fields */
#define TTBCR2_ADDR_32			0
#define TTBCR2_ADDR_36			1
#define TTBCR2_ADDR_40			2
#define TTBCR2_ADDR_42			3
#define TTBCR2_ADDR_44			4
#define TTBCR2_ADDR_48			5

#define TTBRn_HI_ASID_SHIFT		16

#define MAIR_ATTR_SHIFT(n)		((n) << 3)
#define MAIR_ATTR_MASK			0xff
#define MAIR_ATTR_DEVICE		0x04
#define MAIR_ATTR_NC			0x44
#define MAIR_ATTR_WBRWA			0xff
#define MAIR_ATTR_IDX_NC		0
#define MAIR_ATTR_IDX_CACHE		1
#define MAIR_ATTR_IDX_DEV		2

#define FSR_MULTI			(1 << 31)
#define FSR_SS				(1 << 30)
#define FSR_UUT				(1 << 8)
#define FSR_ASF				(1 << 7)
#define FSR_TLBLKF			(1 << 6)
#define FSR_TLBMCF			(1 << 5)
#define FSR_EF				(1 << 4)
#define FSR_PF				(1 << 3)
#define FSR_AFF				(1 << 2)
#define FSR_TF				(1 << 1)

#define FSR_IGN				(FSR_AFF | FSR_ASF | \
					 FSR_TLBMCF | FSR_TLBLKF)
#define FSR_FAULT			(FSR_MULTI | FSR_SS | FSR_UUT | \
					 FSR_EF | FSR_PF | FSR_TF | FSR_IGN)

#define FSYNR0_WNR			(1 << 4)

struct arm_smmu_smr {
	u8				idx;
	u16				mask;
	u16				id;
};

struct arm_smmu_master_cfg {
	int				num_streamids;
	u16				streamids[MAX_MASTER_STREAMIDS];
	struct arm_smmu_smr		*smrs;
};

struct arm_smmu_master {
	struct device_node		*of_node;
	struct rb_node			node;
	struct arm_smmu_master_cfg	cfg;
};

struct arm_smmu_device {
	struct device			*dev;

	void __iomem			*base;
	unsigned long			size;
	unsigned long			pagesize;

#define ARM_SMMU_FEAT_COHERENT_WALK	(1 << 0)
#define ARM_SMMU_FEAT_STREAM_MATCH	(1 << 1)
#define ARM_SMMU_FEAT_TRANS_S1		(1 << 2)
#define ARM_SMMU_FEAT_TRANS_S2		(1 << 3)
#define ARM_SMMU_FEAT_TRANS_NESTED	(1 << 4)
	u32				features;

#define ARM_SMMU_OPT_SECURE_CFG_ACCESS (1 << 0)
	u32				options;
	int				version;

	u32				num_context_banks;
	u32				num_s2_context_banks;
	DECLARE_BITMAP(context_map, ARM_SMMU_MAX_CBS);
	atomic_t			irptndx;

	u32				num_mapping_groups;
	DECLARE_BITMAP(smr_map, ARM_SMMU_MAX_SMRS);

	unsigned long			input_size;
	unsigned long			s1_output_size;
	unsigned long			s2_output_size;

	u32				num_global_irqs;
	u32				num_context_irqs;
	unsigned int			*irqs;

	struct list_head		list;
	struct rb_root			masters;
};

struct arm_smmu_cfg {
	u8				cbndx;
	u8				irptndx;
	u32				cbar;
	pgd_t				*pgd;
};
#define INVALID_IRPTNDX			0xff

#define ARM_SMMU_CB_ASID(cfg)		((cfg)->cbndx + 1)
#define ARM_SMMU_CB_VMID(cfg)		((cfg)->cbndx + 2)

struct arm_smmu_domain {
	struct arm_smmu_device		*smmu;
	struct arm_smmu_cfg		cfg;
	spinlock_t			lock;
};

static DEFINE_SPINLOCK(arm_smmu_devices_lock);
static LIST_HEAD(arm_smmu_devices);

struct arm_smmu_option_prop {
	u32 opt;
	const char *prop;
};

static struct arm_smmu_option_prop arm_smmu_options[] = {
	{ ARM_SMMU_OPT_SECURE_CFG_ACCESS, "calxeda,smmu-secure-config-access" },
	{ 0, NULL},
};

static void parse_driver_options(struct arm_smmu_device *smmu)
{
	int i = 0;

	do {
		if (of_property_read_bool(smmu->dev->of_node,
						arm_smmu_options[i].prop)) {
			smmu->options |= arm_smmu_options[i].opt;
			dev_notice(smmu->dev, "option %s\n",
				arm_smmu_options[i].prop);
		}
	} while (arm_smmu_options[++i].opt);
}

static struct device *dev_get_master_dev(struct device *dev)
{
	if (dev_is_pci(dev)) {
		struct pci_bus *bus = to_pci_dev(dev)->bus;

		while (!pci_is_root_bus(bus))
			bus = bus->parent;
		return bus->bridge->parent;
	}

	return dev;
}

static struct arm_smmu_master *find_smmu_master(struct arm_smmu_device *smmu,
						struct device_node *dev_node)
{
	struct rb_node *node = smmu->masters.rb_node;

	while (node) {
		struct arm_smmu_master *master;

		master = container_of(node, struct arm_smmu_master, node);

		if (dev_node < master->of_node)
			node = node->rb_left;
		else if (dev_node > master->of_node)
			node = node->rb_right;
		else
			return master;
	}

	return NULL;
}

static struct arm_smmu_master_cfg *
find_smmu_master_cfg(struct arm_smmu_device *smmu, struct device *dev)
{
	struct arm_smmu_master *master;

	if (dev_is_pci(dev))
		return dev->archdata.iommu;

	master = find_smmu_master(smmu, dev->of_node);
	return master ? &master->cfg : NULL;
}

static int insert_smmu_master(struct arm_smmu_device *smmu,
			      struct arm_smmu_master *master)
{
	struct rb_node **new, *parent;

	new = &smmu->masters.rb_node;
	parent = NULL;
	while (*new) {
		struct arm_smmu_master *this
			= container_of(*new, struct arm_smmu_master, node);

		parent = *new;
		if (master->of_node < this->of_node)
			new = &((*new)->rb_left);
		else if (master->of_node > this->of_node)
			new = &((*new)->rb_right);
		else
			return -EEXIST;
	}

	rb_link_node(&master->node, parent, new);
	rb_insert_color(&master->node, &smmu->masters);
	return 0;
}

static int register_smmu_master(struct arm_smmu_device *smmu,
				struct device *dev,
				struct of_phandle_args *masterspec)
{
	int i;
	struct arm_smmu_master *master;

	master = find_smmu_master(smmu, masterspec->np);
	if (master) {
		dev_err(dev,
			"rejecting multiple registrations for master device %s\n",
			masterspec->np->name);
		return -EBUSY;
	}

	if (masterspec->args_count > MAX_MASTER_STREAMIDS) {
		dev_err(dev,
			"reached maximum number (%d) of stream IDs for master device %s\n",
			MAX_MASTER_STREAMIDS, masterspec->np->name);
		return -ENOSPC;
	}

	master = devm_kzalloc(dev, sizeof(*master), GFP_KERNEL);
	if (!master)
		return -ENOMEM;

	master->of_node			= masterspec->np;
	master->cfg.num_streamids	= masterspec->args_count;

	for (i = 0; i < master->cfg.num_streamids; ++i)
		master->cfg.streamids[i] = masterspec->args[i];

	return insert_smmu_master(smmu, master);
}

static struct arm_smmu_device *find_smmu_for_device(struct device *dev)
{
	struct arm_smmu_device *smmu;
	struct arm_smmu_master *master = NULL;
	struct device_node *dev_node = dev_get_master_dev(dev)->of_node;

	spin_lock(&arm_smmu_devices_lock);
	list_for_each_entry(smmu, &arm_smmu_devices, list) {
		master = find_smmu_master(smmu, dev_node);
		if (master)
			break;
	}
	spin_unlock(&arm_smmu_devices_lock);

	return master ? smmu : NULL;
}

static int __arm_smmu_alloc_bitmap(unsigned long *map, int start, int end)
{
	int idx;

	do {
		idx = find_next_zero_bit(map, end, start);
		if (idx == end)
			return -ENOSPC;
	} while (test_and_set_bit(idx, map));

	return idx;
}

static void __arm_smmu_free_bitmap(unsigned long *map, int idx)
{
	clear_bit(idx, map);
}

/* Wait for any pending TLB invalidations to complete */
static void arm_smmu_tlb_sync(struct arm_smmu_device *smmu)
{
	int count = 0;
	void __iomem *gr0_base = ARM_SMMU_GR0(smmu);

	writel_relaxed(0, gr0_base + ARM_SMMU_GR0_sTLBGSYNC);
	while (readl_relaxed(gr0_base + ARM_SMMU_GR0_sTLBGSTATUS)
	       & sTLBGSTATUS_GSACTIVE) {
		cpu_relax();
		if (++count == TLB_LOOP_TIMEOUT) {
			dev_err_ratelimited(smmu->dev,
			"TLB sync timed out -- SMMU may be deadlocked\n");
			return;
		}
		udelay(1);
	}
}

static void arm_smmu_tlb_inv_context(struct arm_smmu_domain *smmu_domain)
{
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	void __iomem *base = ARM_SMMU_GR0(smmu);
	bool stage1 = cfg->cbar != CBAR_TYPE_S2_TRANS;

	if (stage1) {
		base = ARM_SMMU_CB_BASE(smmu) + ARM_SMMU_CB(smmu, cfg->cbndx);
		writel_relaxed(ARM_SMMU_CB_ASID(cfg),
			       base + ARM_SMMU_CB_S1_TLBIASID);
	} else {
		base = ARM_SMMU_GR0(smmu);
		writel_relaxed(ARM_SMMU_CB_VMID(cfg),
			       base + ARM_SMMU_GR0_TLBIVMID);
	}

	arm_smmu_tlb_sync(smmu);
}

static irqreturn_t arm_smmu_context_fault(int irq, void *dev)
{
	int flags, ret;
	u32 fsr, far, fsynr, resume;
	unsigned long iova;
	struct iommu_domain *domain = dev;
	struct arm_smmu_domain *smmu_domain = domain->priv;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	void __iomem *cb_base;

	cb_base = ARM_SMMU_CB_BASE(smmu) + ARM_SMMU_CB(smmu, cfg->cbndx);
	fsr = readl_relaxed(cb_base + ARM_SMMU_CB_FSR);

	if (!(fsr & FSR_FAULT))
		return IRQ_NONE;

	if (fsr & FSR_IGN)
		dev_err_ratelimited(smmu->dev,
				    "Unexpected context fault (fsr 0x%x)\n",
				    fsr);

	fsynr = readl_relaxed(cb_base + ARM_SMMU_CB_FSYNR0);
	flags = fsynr & FSYNR0_WNR ? IOMMU_FAULT_WRITE : IOMMU_FAULT_READ;

	far = readl_relaxed(cb_base + ARM_SMMU_CB_FAR_LO);
	iova = far;
#ifdef CONFIG_64BIT
	far = readl_relaxed(cb_base + ARM_SMMU_CB_FAR_HI);
	iova |= ((unsigned long)far << 32);
#endif

	if (!report_iommu_fault(domain, smmu->dev, iova, flags)) {
		ret = IRQ_HANDLED;
		resume = RESUME_RETRY;
	} else {
		dev_err_ratelimited(smmu->dev,
		    "Unhandled context fault: iova=0x%08lx, fsynr=0x%x, cb=%d\n",
		    iova, fsynr, cfg->cbndx);
		ret = IRQ_NONE;
		resume = RESUME_TERMINATE;
	}

	/* Clear the faulting FSR */
	writel(fsr, cb_base + ARM_SMMU_CB_FSR);

	/* Retry or terminate any stalled transactions */
	if (fsr & FSR_SS)
		writel_relaxed(resume, cb_base + ARM_SMMU_CB_RESUME);

	return ret;
}

static irqreturn_t arm_smmu_global_fault(int irq, void *dev)
{
	u32 gfsr, gfsynr0, gfsynr1, gfsynr2;
	struct arm_smmu_device *smmu = dev;
	void __iomem *gr0_base = ARM_SMMU_GR0_NS(smmu);

	gfsr = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSR);
	gfsynr0 = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSYNR0);
	gfsynr1 = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSYNR1);
	gfsynr2 = readl_relaxed(gr0_base + ARM_SMMU_GR0_sGFSYNR2);

	if (!gfsr)
		return IRQ_NONE;

	dev_err_ratelimited(smmu->dev,
		"Unexpected global fault, this could be serious\n");
	dev_err_ratelimited(smmu->dev,
		"\tGFSR 0x%08x, GFSYNR0 0x%08x, GFSYNR1 0x%08x, GFSYNR2 0x%08x\n",
		gfsr, gfsynr0, gfsynr1, gfsynr2);

	writel(gfsr, gr0_base + ARM_SMMU_GR0_sGFSR);
	return IRQ_HANDLED;
}

static void arm_smmu_flush_pgtable(struct arm_smmu_device *smmu, void *addr,
				   size_t size)
{
	unsigned long offset = (unsigned long)addr & ~PAGE_MASK;


	/* Ensure new page tables are visible to the hardware walker */
	if (smmu->features & ARM_SMMU_FEAT_COHERENT_WALK) {
		dsb(ishst);
	} else {
		/*
		 * If the SMMU can't walk tables in the CPU caches, treat them
		 * like non-coherent DMA since we need to flush the new entries
		 * all the way out to memory. There's no possibility of
		 * recursion here as the SMMU table walker will not be wired
		 * through another SMMU.
		 */
		dma_map_page(smmu->dev, virt_to_page(addr), offset, size,
				DMA_TO_DEVICE);
	}
}

static void arm_smmu_init_context_bank(struct arm_smmu_domain *smmu_domain)
{
	u32 reg;
	bool stage1;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	void __iomem *cb_base, *gr0_base, *gr1_base;

	gr0_base = ARM_SMMU_GR0(smmu);
	gr1_base = ARM_SMMU_GR1(smmu);
	stage1 = cfg->cbar != CBAR_TYPE_S2_TRANS;
	cb_base = ARM_SMMU_CB_BASE(smmu) + ARM_SMMU_CB(smmu, cfg->cbndx);

	/* CBAR */
	reg = cfg->cbar;
	if (smmu->version == 1)
		reg |= cfg->irptndx << CBAR_IRPTNDX_SHIFT;

	/*
	 * Use the weakest shareability/memory types, so they are
	 * overridden by the ttbcr/pte.
	 */
	if (stage1) {
		reg |= (CBAR_S1_BPSHCFG_NSH << CBAR_S1_BPSHCFG_SHIFT) |
			(CBAR_S1_MEMATTR_WB << CBAR_S1_MEMATTR_SHIFT);
	} else {
		reg |= ARM_SMMU_CB_VMID(cfg) << CBAR_VMID_SHIFT;
	}
	writel_relaxed(reg, gr1_base + ARM_SMMU_GR1_CBAR(cfg->cbndx));

	if (smmu->version > 1) {
		/* CBA2R */
#ifdef CONFIG_64BIT
		reg = CBA2R_RW64_64BIT;
#else
		reg = CBA2R_RW64_32BIT;
#endif
		writel_relaxed(reg,
			       gr1_base + ARM_SMMU_GR1_CBA2R(cfg->cbndx));

		/* TTBCR2 */
		switch (smmu->input_size) {
		case 32:
			reg = (TTBCR2_ADDR_32 << TTBCR2_SEP_SHIFT);
			break;
		case 36:
			reg = (TTBCR2_ADDR_36 << TTBCR2_SEP_SHIFT);
			break;
		case 39:
		case 40:
			reg = (TTBCR2_ADDR_40 << TTBCR2_SEP_SHIFT);
			break;
		case 42:
			reg = (TTBCR2_ADDR_42 << TTBCR2_SEP_SHIFT);
			break;
		case 44:
			reg = (TTBCR2_ADDR_44 << TTBCR2_SEP_SHIFT);
			break;
		case 48:
			reg = (TTBCR2_ADDR_48 << TTBCR2_SEP_SHIFT);
			break;
		}

		switch (smmu->s1_output_size) {
		case 32:
			reg |= (TTBCR2_ADDR_32 << TTBCR2_PASIZE_SHIFT);
			break;
		case 36:
			reg |= (TTBCR2_ADDR_36 << TTBCR2_PASIZE_SHIFT);
			break;
		case 39:
		case 40:
			reg |= (TTBCR2_ADDR_40 << TTBCR2_PASIZE_SHIFT);
			break;
		case 42:
			reg |= (TTBCR2_ADDR_42 << TTBCR2_PASIZE_SHIFT);
			break;
		case 44:
			reg |= (TTBCR2_ADDR_44 << TTBCR2_PASIZE_SHIFT);
			break;
		case 48:
			reg |= (TTBCR2_ADDR_48 << TTBCR2_PASIZE_SHIFT);
			break;
		}

		if (stage1)
			writel_relaxed(reg, cb_base + ARM_SMMU_CB_TTBCR2);
	}

	/* TTBR0 */
	arm_smmu_flush_pgtable(smmu, cfg->pgd,
			       PTRS_PER_PGD * sizeof(pgd_t));
	reg = __pa(cfg->pgd);
	writel_relaxed(reg, cb_base + ARM_SMMU_CB_TTBR0_LO);
	reg = (phys_addr_t)__pa(cfg->pgd) >> 32;
	if (stage1)
		reg |= ARM_SMMU_CB_ASID(cfg) << TTBRn_HI_ASID_SHIFT;
	writel_relaxed(reg, cb_base + ARM_SMMU_CB_TTBR0_HI);

	/*
	 * TTBCR
	 * We use long descriptor, with inner-shareable WBWA tables in TTBR0.
	 */
	if (smmu->version > 1) {
		if (PAGE_SIZE == SZ_4K)
			reg = TTBCR_TG0_4K;
		else
			reg = TTBCR_TG0_64K;

		if (!stage1) {
			reg |= (64 - smmu->s1_output_size) << TTBCR_T0SZ_SHIFT;

			switch (smmu->s2_output_size) {
			case 32:
				reg |= (TTBCR2_ADDR_32 << TTBCR_PASIZE_SHIFT);
				break;
			case 36:
				reg |= (TTBCR2_ADDR_36 << TTBCR_PASIZE_SHIFT);
				break;
			case 40:
				reg |= (TTBCR2_ADDR_40 << TTBCR_PASIZE_SHIFT);
				break;
			case 42:
				reg |= (TTBCR2_ADDR_42 << TTBCR_PASIZE_SHIFT);
				break;
			case 44:
				reg |= (TTBCR2_ADDR_44 << TTBCR_PASIZE_SHIFT);
				break;
			case 48:
				reg |= (TTBCR2_ADDR_48 << TTBCR_PASIZE_SHIFT);
				break;
			}
		} else {
			reg |= (64 - smmu->input_size) << TTBCR_T0SZ_SHIFT;
		}
	} else {
		reg = 0;
	}

	reg |= TTBCR_EAE |
	      (TTBCR_SH_IS << TTBCR_SH0_SHIFT) |
	      (TTBCR_RGN_WBWA << TTBCR_ORGN0_SHIFT) |
	      (TTBCR_RGN_WBWA << TTBCR_IRGN0_SHIFT);

	if (!stage1)
		reg |= (TTBCR_SL0_LVL_1 << TTBCR_SL0_SHIFT);

	writel_relaxed(reg, cb_base + ARM_SMMU_CB_TTBCR);

	/* MAIR0 (stage-1 only) */
	if (stage1) {
		reg = (MAIR_ATTR_NC << MAIR_ATTR_SHIFT(MAIR_ATTR_IDX_NC)) |
		      (MAIR_ATTR_WBRWA << MAIR_ATTR_SHIFT(MAIR_ATTR_IDX_CACHE)) |
		      (MAIR_ATTR_DEVICE << MAIR_ATTR_SHIFT(MAIR_ATTR_IDX_DEV));
		writel_relaxed(reg, cb_base + ARM_SMMU_CB_S1_MAIR0);
	}

	/* SCTLR */
	reg = SCTLR_CFCFG | SCTLR_CFIE | SCTLR_CFRE | SCTLR_M | SCTLR_EAE_SBOP;
	if (stage1)
		reg |= SCTLR_S1_ASIDPNE;
#ifdef __BIG_ENDIAN
	reg |= SCTLR_E;
#endif
	writel_relaxed(reg, cb_base + ARM_SMMU_CB_SCTLR);
}

static int arm_smmu_init_domain_context(struct iommu_domain *domain,
					struct arm_smmu_device *smmu)
{
	int irq, start, ret = 0;
	unsigned long flags;
	struct arm_smmu_domain *smmu_domain = domain->priv;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;

	spin_lock_irqsave(&smmu_domain->lock, flags);
	if (smmu_domain->smmu)
		goto out_unlock;

	if (smmu->features & ARM_SMMU_FEAT_TRANS_NESTED) {
		/*
		 * We will likely want to change this if/when KVM gets
		 * involved.
		 */
		cfg->cbar = CBAR_TYPE_S1_TRANS_S2_BYPASS;
		start = smmu->num_s2_context_banks;
	} else if (smmu->features & ARM_SMMU_FEAT_TRANS_S1) {
		cfg->cbar = CBAR_TYPE_S1_TRANS_S2_BYPASS;
		start = smmu->num_s2_context_banks;
	} else {
		cfg->cbar = CBAR_TYPE_S2_TRANS;
		start = 0;
	}

	ret = __arm_smmu_alloc_bitmap(smmu->context_map, start,
				      smmu->num_context_banks);
	if (IS_ERR_VALUE(ret))
		goto out_unlock;

	cfg->cbndx = ret;
	if (smmu->version == 1) {
		cfg->irptndx = atomic_inc_return(&smmu->irptndx);
		cfg->irptndx %= smmu->num_context_irqs;
	} else {
		cfg->irptndx = cfg->cbndx;
	}

	ACCESS_ONCE(smmu_domain->smmu) = smmu;
	arm_smmu_init_context_bank(smmu_domain);
	spin_unlock_irqrestore(&smmu_domain->lock, flags);

	irq = smmu->irqs[smmu->num_global_irqs + cfg->irptndx];
	ret = request_irq(irq, arm_smmu_context_fault, IRQF_SHARED,
			  "arm-smmu-context-fault", domain);
	if (IS_ERR_VALUE(ret)) {
		dev_err(smmu->dev, "failed to request context IRQ %d (%u)\n",
			cfg->irptndx, irq);
		cfg->irptndx = INVALID_IRPTNDX;
	}

	return 0;

out_unlock:
	spin_unlock_irqrestore(&smmu_domain->lock, flags);
	return ret;
}

static void arm_smmu_destroy_domain_context(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = domain->priv;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	void __iomem *cb_base;
	int irq;

	if (!smmu)
		return;

	/* Disable the context bank and nuke the TLB before freeing it. */
	cb_base = ARM_SMMU_CB_BASE(smmu) + ARM_SMMU_CB(smmu, cfg->cbndx);
	writel_relaxed(0, cb_base + ARM_SMMU_CB_SCTLR);
	arm_smmu_tlb_inv_context(smmu_domain);

	if (cfg->irptndx != INVALID_IRPTNDX) {
		irq = smmu->irqs[smmu->num_global_irqs + cfg->irptndx];
		free_irq(irq, domain);
	}

	__arm_smmu_free_bitmap(smmu->context_map, cfg->cbndx);
}

static int arm_smmu_domain_init(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain;
	pgd_t *pgd;

	/*
	 * Allocate the domain and initialise some of its data structures.
	 * We can't really do anything meaningful until we've added a
	 * master.
	 */
	smmu_domain = kzalloc(sizeof(*smmu_domain), GFP_KERNEL);
	if (!smmu_domain)
		return -ENOMEM;

	pgd = kcalloc(PTRS_PER_PGD, sizeof(pgd_t), GFP_KERNEL);
	if (!pgd)
		goto out_free_domain;
	smmu_domain->cfg.pgd = pgd;

	spin_lock_init(&smmu_domain->lock);
	domain->priv = smmu_domain;
	return 0;

out_free_domain:
	kfree(smmu_domain);
	return -ENOMEM;
}

static void arm_smmu_free_ptes(pmd_t *pmd)
{
	pgtable_t table = pmd_pgtable(*pmd);

	__free_page(table);
}

static void arm_smmu_free_pmds(pud_t *pud)
{
	int i;
	pmd_t *pmd, *pmd_base = pmd_offset(pud, 0);

	pmd = pmd_base;
	for (i = 0; i < PTRS_PER_PMD; ++i) {
		if (pmd_none(*pmd))
			continue;

		arm_smmu_free_ptes(pmd);
		pmd++;
	}

	pmd_free(NULL, pmd_base);
}

static void arm_smmu_free_puds(pgd_t *pgd)
{
	int i;
	pud_t *pud, *pud_base = pud_offset(pgd, 0);

	pud = pud_base;
	for (i = 0; i < PTRS_PER_PUD; ++i) {
		if (pud_none(*pud))
			continue;

		arm_smmu_free_pmds(pud);
		pud++;
	}

	pud_free(NULL, pud_base);
}

static void arm_smmu_free_pgtables(struct arm_smmu_domain *smmu_domain)
{
	int i;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	pgd_t *pgd, *pgd_base = cfg->pgd;

	/*
	 * Recursively free the page tables for this domain. We don't
	 * care about speculative TLB filling because the tables should
	 * not be active in any context bank at this point (SCTLR.M is 0).
	 */
	pgd = pgd_base;
	for (i = 0; i < PTRS_PER_PGD; ++i) {
		if (pgd_none(*pgd))
			continue;
		arm_smmu_free_puds(pgd);
		pgd++;
	}

	kfree(pgd_base);
}

static void arm_smmu_domain_destroy(struct iommu_domain *domain)
{
	struct arm_smmu_domain *smmu_domain = domain->priv;

	/*
	 * Free the domain resources. We assume that all devices have
	 * already been detached.
	 */
	arm_smmu_destroy_domain_context(domain);
	arm_smmu_free_pgtables(smmu_domain);
	kfree(smmu_domain);
}

static int arm_smmu_master_configure_smrs(struct arm_smmu_device *smmu,
					  struct arm_smmu_master_cfg *cfg)
{
	int i;
	struct arm_smmu_smr *smrs;
	void __iomem *gr0_base = ARM_SMMU_GR0(smmu);

	if (!(smmu->features & ARM_SMMU_FEAT_STREAM_MATCH))
		return 0;

	if (cfg->smrs)
		return -EEXIST;

	smrs = kmalloc_array(cfg->num_streamids, sizeof(*smrs), GFP_KERNEL);
	if (!smrs) {
		dev_err(smmu->dev, "failed to allocate %d SMRs\n",
			cfg->num_streamids);
		return -ENOMEM;
	}

	/* Allocate the SMRs on the SMMU */
	for (i = 0; i < cfg->num_streamids; ++i) {
		int idx = __arm_smmu_alloc_bitmap(smmu->smr_map, 0,
						  smmu->num_mapping_groups);
		if (IS_ERR_VALUE(idx)) {
			dev_err(smmu->dev, "failed to allocate free SMR\n");
			goto err_free_smrs;
		}

		smrs[i] = (struct arm_smmu_smr) {
			.idx	= idx,
			.mask	= 0, /* We don't currently share SMRs */
			.id	= cfg->streamids[i],
		};
	}

	/* It worked! Now, poke the actual hardware */
	for (i = 0; i < cfg->num_streamids; ++i) {
		u32 reg = SMR_VALID | smrs[i].id << SMR_ID_SHIFT |
			  smrs[i].mask << SMR_MASK_SHIFT;
		writel_relaxed(reg, gr0_base + ARM_SMMU_GR0_SMR(smrs[i].idx));
	}

	cfg->smrs = smrs;
	return 0;

err_free_smrs:
	while (--i >= 0)
		__arm_smmu_free_bitmap(smmu->smr_map, smrs[i].idx);
	kfree(smrs);
	return -ENOSPC;
}

static void arm_smmu_master_free_smrs(struct arm_smmu_device *smmu,
				      struct arm_smmu_master_cfg *cfg)
{
	int i;
	void __iomem *gr0_base = ARM_SMMU_GR0(smmu);
	struct arm_smmu_smr *smrs = cfg->smrs;

	if (!smrs)
		return;

	/* Invalidate the SMRs before freeing back to the allocator */
	for (i = 0; i < cfg->num_streamids; ++i) {
		u8 idx = smrs[i].idx;

		writel_relaxed(~SMR_VALID, gr0_base + ARM_SMMU_GR0_SMR(idx));
		__arm_smmu_free_bitmap(smmu->smr_map, idx);
	}

	cfg->smrs = NULL;
	kfree(smrs);
}

static int arm_smmu_domain_add_master(struct arm_smmu_domain *smmu_domain,
				      struct arm_smmu_master_cfg *cfg)
{
	int i, ret;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	void __iomem *gr0_base = ARM_SMMU_GR0(smmu);

	ret = arm_smmu_master_configure_smrs(smmu, cfg);
	if (ret)
		return ret;

	for (i = 0; i < cfg->num_streamids; ++i) {
		u32 idx, s2cr;

		idx = cfg->smrs ? cfg->smrs[i].idx : cfg->streamids[i];
		s2cr = S2CR_TYPE_TRANS |
		       (smmu_domain->cfg.cbndx << S2CR_CBNDX_SHIFT);
		writel_relaxed(s2cr, gr0_base + ARM_SMMU_GR0_S2CR(idx));
	}

	return 0;
}

static void arm_smmu_domain_remove_master(struct arm_smmu_domain *smmu_domain,
					  struct arm_smmu_master_cfg *cfg)
{
	int i;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	void __iomem *gr0_base = ARM_SMMU_GR0(smmu);

	/*
	 * We *must* clear the S2CR first, because freeing the SMR means
	 * that it can be re-allocated immediately.
	 */
	for (i = 0; i < cfg->num_streamids; ++i) {
		u32 idx = cfg->smrs ? cfg->smrs[i].idx : cfg->streamids[i];

		writel_relaxed(S2CR_TYPE_BYPASS,
			       gr0_base + ARM_SMMU_GR0_S2CR(idx));
	}

	arm_smmu_master_free_smrs(smmu, cfg);
}

static int arm_smmu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	int ret;
	struct arm_smmu_domain *smmu_domain = domain->priv;
	struct arm_smmu_device *smmu, *dom_smmu;
	struct arm_smmu_master_cfg *cfg;

	smmu = dev_get_master_dev(dev)->archdata.iommu;
	if (!smmu) {
		dev_err(dev, "cannot attach to SMMU, is it on the same bus?\n");
		return -ENXIO;
	}

	/*
	 * Sanity check the domain. We don't support domains across
	 * different SMMUs.
	 */
	dom_smmu = ACCESS_ONCE(smmu_domain->smmu);
	if (!dom_smmu) {
		/* Now that we have a master, we can finalise the domain */
		ret = arm_smmu_init_domain_context(domain, smmu);
		if (IS_ERR_VALUE(ret))
			return ret;

		dom_smmu = smmu_domain->smmu;
	}

	if (dom_smmu != smmu) {
		dev_err(dev,
			"cannot attach to SMMU %s whilst already attached to domain on SMMU %s\n",
			dev_name(smmu_domain->smmu->dev), dev_name(smmu->dev));
		return -EINVAL;
	}

	/* Looks ok, so add the device to the domain */
	cfg = find_smmu_master_cfg(smmu_domain->smmu, dev);
	if (!cfg)
		return -ENODEV;

	return arm_smmu_domain_add_master(smmu_domain, cfg);
}

static void arm_smmu_detach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct arm_smmu_domain *smmu_domain = domain->priv;
	struct arm_smmu_master_cfg *cfg;

	cfg = find_smmu_master_cfg(smmu_domain->smmu, dev);
	if (cfg)
		arm_smmu_domain_remove_master(smmu_domain, cfg);
}

static bool arm_smmu_pte_is_contiguous_range(unsigned long addr,
					     unsigned long end)
{
	return !(addr & ~ARM_SMMU_PTE_CONT_MASK) &&
		(addr + ARM_SMMU_PTE_CONT_SIZE <= end);
}

static int arm_smmu_alloc_init_pte(struct arm_smmu_device *smmu, pmd_t *pmd,
				   unsigned long addr, unsigned long end,
				   unsigned long pfn, int prot, int stage)
{
	pte_t *pte, *start;
	pteval_t pteval = ARM_SMMU_PTE_PAGE | ARM_SMMU_PTE_AF | ARM_SMMU_PTE_XN;

	if (pmd_none(*pmd)) {
		/* Allocate a new set of tables */
		pgtable_t table = alloc_page(GFP_ATOMIC|__GFP_ZERO);

		if (!table)
			return -ENOMEM;

		arm_smmu_flush_pgtable(smmu, page_address(table), PAGE_SIZE);
		pmd_populate(NULL, pmd, table);
		arm_smmu_flush_pgtable(smmu, pmd, sizeof(*pmd));
	}

	if (stage == 1) {
		pteval |= ARM_SMMU_PTE_AP_UNPRIV | ARM_SMMU_PTE_nG;
		if (!(prot & IOMMU_WRITE) && (prot & IOMMU_READ))
			pteval |= ARM_SMMU_PTE_AP_RDONLY;

		if (prot & IOMMU_CACHE)
			pteval |= (MAIR_ATTR_IDX_CACHE <<
				   ARM_SMMU_PTE_ATTRINDX_SHIFT);
	} else {
		pteval |= ARM_SMMU_PTE_HAP_FAULT;
		if (prot & IOMMU_READ)
			pteval |= ARM_SMMU_PTE_HAP_READ;
		if (prot & IOMMU_WRITE)
			pteval |= ARM_SMMU_PTE_HAP_WRITE;
		if (prot & IOMMU_CACHE)
			pteval |= ARM_SMMU_PTE_MEMATTR_OIWB;
		else
			pteval |= ARM_SMMU_PTE_MEMATTR_NC;
	}

	/* If no access, create a faulting entry to avoid TLB fills */
	if (prot & IOMMU_EXEC)
		pteval &= ~ARM_SMMU_PTE_XN;
	else if (!(prot & (IOMMU_READ | IOMMU_WRITE)))
		pteval &= ~ARM_SMMU_PTE_PAGE;

	pteval |= ARM_SMMU_PTE_SH_IS;
	start = pmd_page_vaddr(*pmd) + pte_index(addr);
	pte = start;

	/*
	 * Install the page table entries. This is fairly complicated
	 * since we attempt to make use of the contiguous hint in the
	 * ptes where possible. The contiguous hint indicates a series
	 * of ARM_SMMU_PTE_CONT_ENTRIES ptes mapping a physically
	 * contiguous region with the following constraints:
	 *
	 *   - The region start is aligned to ARM_SMMU_PTE_CONT_SIZE
	 *   - Each pte in the region has the contiguous hint bit set
	 *
	 * This complicates unmapping (also handled by this code, when
	 * neither IOMMU_READ or IOMMU_WRITE are set) because it is
	 * possible, yet highly unlikely, that a client may unmap only
	 * part of a contiguous range. This requires clearing of the
	 * contiguous hint bits in the range before installing the new
	 * faulting entries.
	 *
	 * Note that re-mapping an address range without first unmapping
	 * it is not supported, so TLB invalidation is not required here
	 * and is instead performed at unmap and domain-init time.
	 */
	do {
		int i = 1;

		pteval &= ~ARM_SMMU_PTE_CONT;

		if (arm_smmu_pte_is_contiguous_range(addr, end)) {
			i = ARM_SMMU_PTE_CONT_ENTRIES;
			pteval |= ARM_SMMU_PTE_CONT;
		} else if (pte_val(*pte) &
			   (ARM_SMMU_PTE_CONT | ARM_SMMU_PTE_PAGE)) {
			int j;
			pte_t *cont_start;
			unsigned long idx = pte_index(addr);

			idx &= ~(ARM_SMMU_PTE_CONT_ENTRIES - 1);
			cont_start = pmd_page_vaddr(*pmd) + idx;
			for (j = 0; j < ARM_SMMU_PTE_CONT_ENTRIES; ++j)
				pte_val(*(cont_start + j)) &=
					~ARM_SMMU_PTE_CONT;

			arm_smmu_flush_pgtable(smmu, cont_start,
					       sizeof(*pte) *
					       ARM_SMMU_PTE_CONT_ENTRIES);
		}

		do {
			*pte = pfn_pte(pfn, __pgprot(pteval));
		} while (pte++, pfn++, addr += PAGE_SIZE, --i);
	} while (addr != end);

	arm_smmu_flush_pgtable(smmu, start, sizeof(*pte) * (pte - start));
	return 0;
}

static int arm_smmu_alloc_init_pmd(struct arm_smmu_device *smmu, pud_t *pud,
				   unsigned long addr, unsigned long end,
				   phys_addr_t phys, int prot, int stage)
{
	int ret;
	pmd_t *pmd;
	unsigned long next, pfn = __phys_to_pfn(phys);

#ifndef __PAGETABLE_PMD_FOLDED
	if (pud_none(*pud)) {
		pmd = (pmd_t *)get_zeroed_page(GFP_ATOMIC);
		if (!pmd)
			return -ENOMEM;

		arm_smmu_flush_pgtable(smmu, pmd, PAGE_SIZE);
		pud_populate(NULL, pud, pmd);
		arm_smmu_flush_pgtable(smmu, pud, sizeof(*pud));

		pmd += pmd_index(addr);
	} else
#endif
		pmd = pmd_offset(pud, addr);

	do {
		next = pmd_addr_end(addr, end);
		ret = arm_smmu_alloc_init_pte(smmu, pmd, addr, next, pfn,
					      prot, stage);
		phys += next - addr;
	} while (pmd++, addr = next, addr < end);

	return ret;
}

static int arm_smmu_alloc_init_pud(struct arm_smmu_device *smmu, pgd_t *pgd,
				   unsigned long addr, unsigned long end,
				   phys_addr_t phys, int prot, int stage)
{
	int ret = 0;
	pud_t *pud;
	unsigned long next;

#ifndef __PAGETABLE_PUD_FOLDED
	if (pgd_none(*pgd)) {
		pud = (pud_t *)get_zeroed_page(GFP_ATOMIC);
		if (!pud)
			return -ENOMEM;

		arm_smmu_flush_pgtable(smmu, pud, PAGE_SIZE);
		pgd_populate(NULL, pgd, pud);
		arm_smmu_flush_pgtable(smmu, pgd, sizeof(*pgd));

		pud += pud_index(addr);
	} else
#endif
		pud = pud_offset(pgd, addr);

	do {
		next = pud_addr_end(addr, end);
		ret = arm_smmu_alloc_init_pmd(smmu, pud, addr, next, phys,
					      prot, stage);
		phys += next - addr;
	} while (pud++, addr = next, addr < end);

	return ret;
}

static int arm_smmu_handle_mapping(struct arm_smmu_domain *smmu_domain,
				   unsigned long iova, phys_addr_t paddr,
				   size_t size, int prot)
{
	int ret, stage;
	unsigned long end;
	phys_addr_t input_mask, output_mask;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;
	pgd_t *pgd = cfg->pgd;
	unsigned long flags;

	if (cfg->cbar == CBAR_TYPE_S2_TRANS) {
		stage = 2;
		output_mask = (1ULL << smmu->s2_output_size) - 1;
	} else {
		stage = 1;
		output_mask = (1ULL << smmu->s1_output_size) - 1;
	}

	if (!pgd)
		return -EINVAL;

	if (size & ~PAGE_MASK)
		return -EINVAL;

	input_mask = (1ULL << smmu->input_size) - 1;
	if ((phys_addr_t)iova & ~input_mask)
		return -ERANGE;

	if (paddr & ~output_mask)
		return -ERANGE;

	spin_lock_irqsave(&smmu_domain->lock, flags);
	pgd += pgd_index(iova);
	end = iova + size;
	do {
		unsigned long next = pgd_addr_end(iova, end);

		ret = arm_smmu_alloc_init_pud(smmu, pgd, iova, next, paddr,
					      prot, stage);
		if (ret)
			goto out_unlock;

		paddr += next - iova;
		iova = next;
	} while (pgd++, iova != end);

out_unlock:
	spin_unlock_irqrestore(&smmu_domain->lock, flags);

	return ret;
}

static int arm_smmu_map(struct iommu_domain *domain, unsigned long iova,
			phys_addr_t paddr, size_t size, int prot)
{
	struct arm_smmu_domain *smmu_domain = domain->priv;

	if (!smmu_domain)
		return -ENODEV;

	return arm_smmu_handle_mapping(smmu_domain, iova, paddr, size, prot);
}

static size_t arm_smmu_unmap(struct iommu_domain *domain, unsigned long iova,
			     size_t size)
{
	int ret;
	struct arm_smmu_domain *smmu_domain = domain->priv;

	ret = arm_smmu_handle_mapping(smmu_domain, iova, 0, size, 0);
	arm_smmu_tlb_inv_context(smmu_domain);
	return ret ? 0 : size;
}

static phys_addr_t arm_smmu_iova_to_phys(struct iommu_domain *domain,
					 dma_addr_t iova)
{
	pgd_t *pgdp, pgd;
	pud_t pud;
	pmd_t pmd;
	pte_t pte;
	struct arm_smmu_domain *smmu_domain = domain->priv;
	struct arm_smmu_cfg *cfg = &smmu_domain->cfg;

	pgdp = cfg->pgd;
	if (!pgdp)
		return 0;

	pgd = *(pgdp + pgd_index(iova));
	if (pgd_none(pgd))
		return 0;

	pud = *pud_offset(&pgd, iova);
	if (pud_none(pud))
		return 0;

	pmd = *pmd_offset(&pud, iova);
	if (pmd_none(pmd))
		return 0;

	pte = *(pmd_page_vaddr(pmd) + pte_index(iova));
	if (pte_none(pte))
		return 0;

	return __pfn_to_phys(pte_pfn(pte)) | (iova & ~PAGE_MASK);
}

static int arm_smmu_domain_has_cap(struct iommu_domain *domain,
				   unsigned long cap)
{
	struct arm_smmu_domain *smmu_domain = domain->priv;
	struct arm_smmu_device *smmu = smmu_domain->smmu;
	u32 features = smmu ? smmu->features : 0;

	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		return features & ARM_SMMU_FEAT_COHERENT_WALK;
	case IOMMU_CAP_INTR_REMAP:
		return 1; /* MSIs are just memory writes */
	default:
		return 0;
	}
}

static int __arm_smmu_get_pci_sid(struct pci_dev *pdev, u16 alias, void *data)
{
	*((u16 *)data) = alias;
	return 0; /* Continue walking */
}

static int arm_smmu_add_device(struct device *dev)
{
	struct arm_smmu_device *smmu;
	struct iommu_group *group;
	int ret;

	if (dev->archdata.iommu) {
		dev_warn(dev, "IOMMU driver already assigned to device\n");
		return -EINVAL;
	}

	smmu = find_smmu_for_device(dev);
	if (!smmu)
		return -ENODEV;

	group = iommu_group_alloc();
	if (IS_ERR(group)) {
		dev_err(dev, "Failed to allocate IOMMU group\n");
		return PTR_ERR(group);
	}

	if (dev_is_pci(dev)) {
		struct arm_smmu_master_cfg *cfg;
		struct pci_dev *pdev = to_pci_dev(dev);

		cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
		if (!cfg) {
			ret = -ENOMEM;
			goto out_put_group;
		}

		cfg->num_streamids = 1;
		/*
		 * Assume Stream ID == Requester ID for now.
		 * We need a way to describe the ID mappings in FDT.
		 */
		pci_for_each_dma_alias(pdev, __arm_smmu_get_pci_sid,
				       &cfg->streamids[0]);
		dev->archdata.iommu = cfg;
	} else {
		dev->archdata.iommu = smmu;
	}

	ret = iommu_group_add_device(group, dev);

out_put_group:
	iommu_group_put(group);
	return ret;
}

static void arm_smmu_remove_device(struct device *dev)
{
	if (dev_is_pci(dev))
		kfree(dev->archdata.iommu);

	dev->archdata.iommu = NULL;
	iommu_group_remove_device(dev);
}

static struct iommu_ops arm_smmu_ops = {
	.domain_init	= arm_smmu_domain_init,
	.domain_destroy	= arm_smmu_domain_destroy,
	.attach_dev	= arm_smmu_attach_dev,
	.detach_dev	= arm_smmu_detach_dev,
	.map		= arm_smmu_map,
	.unmap		= arm_smmu_unmap,
	.map_sg		= default_iommu_map_sg,
	.iova_to_phys	= arm_smmu_iova_to_phys,
	.domain_has_cap	= arm_smmu_domain_has_cap,
	.add_device	= arm_smmu_add_device,
	.remove_device	= arm_smmu_remove_device,
	.pgsize_bitmap	= (SECTION_SIZE |
			   ARM_SMMU_PTE_CONT_SIZE |
			   PAGE_SIZE),
};

static void arm_smmu_device_reset(struct arm_smmu_device *smmu)
{
	void __iomem *gr0_base = ARM_SMMU_GR0(smmu);
	void __iomem *cb_base;
	int i = 0;
	u32 reg;

	/* clear global FSR */
	reg = readl_relaxed(ARM_SMMU_GR0_NS(smmu) + ARM_SMMU_GR0_sGFSR);
	writel(reg, ARM_SMMU_GR0_NS(smmu) + ARM_SMMU_GR0_sGFSR);

	/* Mark all SMRn as invalid and all S2CRn as bypass */
	for (i = 0; i < smmu->num_mapping_groups; ++i) {
		writel_relaxed(~SMR_VALID, gr0_base + ARM_SMMU_GR0_SMR(i));
		writel_relaxed(S2CR_TYPE_BYPASS,
			gr0_base + ARM_SMMU_GR0_S2CR(i));
	}

	/* Make sure all context banks are disabled and clear CB_FSR  */
	for (i = 0; i < smmu->num_context_banks; ++i) {
		cb_base = ARM_SMMU_CB_BASE(smmu) + ARM_SMMU_CB(smmu, i);
		writel_relaxed(0, cb_base + ARM_SMMU_CB_SCTLR);
		writel_relaxed(FSR_FAULT, cb_base + ARM_SMMU_CB_FSR);
	}

	/* Invalidate the TLB, just in case */
	writel_relaxed(0, gr0_base + ARM_SMMU_GR0_STLBIALL);
	writel_relaxed(0, gr0_base + ARM_SMMU_GR0_TLBIALLH);
	writel_relaxed(0, gr0_base + ARM_SMMU_GR0_TLBIALLNSNH);

	reg = readl_relaxed(ARM_SMMU_GR0_NS(smmu) + ARM_SMMU_GR0_sCR0);

	/* Enable fault reporting */
	reg |= (sCR0_GFRE | sCR0_GFIE | sCR0_GCFGFRE | sCR0_GCFGFIE);

	/* Disable TLB broadcasting. */
	reg |= (sCR0_VMIDPNE | sCR0_PTM);

	/* Enable client access, but bypass when no mapping is found */
	reg &= ~(sCR0_CLIENTPD | sCR0_USFCFG);

	/* Disable forced broadcasting */
	reg &= ~sCR0_FB;

	/* Don't upgrade barriers */
	reg &= ~(sCR0_BSU_MASK << sCR0_BSU_SHIFT);

	/* Push the button */
	arm_smmu_tlb_sync(smmu);
	writel(reg, ARM_SMMU_GR0_NS(smmu) + ARM_SMMU_GR0_sCR0);
}

static int arm_smmu_id_size_to_bits(int size)
{
	switch (size) {
	case 0:
		return 32;
	case 1:
		return 36;
	case 2:
		return 40;
	case 3:
		return 42;
	case 4:
		return 44;
	case 5:
	default:
		return 48;
	}
}

static int arm_smmu_device_cfg_probe(struct arm_smmu_device *smmu)
{
	unsigned long size;
	void __iomem *gr0_base = ARM_SMMU_GR0(smmu);
	u32 id;

	dev_notice(smmu->dev, "probing hardware configuration...\n");

	/* Primecell ID */
	id = readl_relaxed(gr0_base + ARM_SMMU_GR0_PIDR2);
	smmu->version = ((id >> PIDR2_ARCH_SHIFT) & PIDR2_ARCH_MASK) + 1;
	dev_notice(smmu->dev, "SMMUv%d with:\n", smmu->version);

	/* ID0 */
	id = readl_relaxed(gr0_base + ARM_SMMU_GR0_ID0);
#ifndef CONFIG_64BIT
	if (((id >> ID0_PTFS_SHIFT) & ID0_PTFS_MASK) == ID0_PTFS_V8_ONLY) {
		dev_err(smmu->dev, "\tno v7 descriptor support!\n");
		return -ENODEV;
	}
#endif
	if (id & ID0_S1TS) {
		smmu->features |= ARM_SMMU_FEAT_TRANS_S1;
		dev_notice(smmu->dev, "\tstage 1 translation\n");
	}

	if (id & ID0_S2TS) {
		smmu->features |= ARM_SMMU_FEAT_TRANS_S2;
		dev_notice(smmu->dev, "\tstage 2 translation\n");
	}

	if (id & ID0_NTS) {
		smmu->features |= ARM_SMMU_FEAT_TRANS_NESTED;
		dev_notice(smmu->dev, "\tnested translation\n");
	}

	if (!(smmu->features &
		(ARM_SMMU_FEAT_TRANS_S1 | ARM_SMMU_FEAT_TRANS_S2 |
		 ARM_SMMU_FEAT_TRANS_NESTED))) {
		dev_err(smmu->dev, "\tno translation support (id0=%x)!\n", id);
		return -ENODEV;
	}

	if (id & ID0_CTTW) {
		smmu->features |= ARM_SMMU_FEAT_COHERENT_WALK;
		dev_notice(smmu->dev, "\tcoherent table walk\n");
	}

	if (id & ID0_SMS) {
		u32 smr, sid, mask = 0;

		smmu->features |= ARM_SMMU_FEAT_STREAM_MATCH;
		smmu->num_mapping_groups = (id >> ID0_NUMSMRG_SHIFT) &
					   ID0_NUMSMRG_MASK;
		if (smmu->num_mapping_groups == 0) {
			dev_err(smmu->dev,
				"stream-matching supported, but no SMRs present!\n");
			return -ENODEV;
		}

		smr = SMR_MASK_MASK << SMR_MASK_SHIFT;
		smr |= (SMR_ID_MASK << SMR_ID_SHIFT);
		writel_relaxed(smr, gr0_base + ARM_SMMU_GR0_SMR(0));
		smr = readl_relaxed(gr0_base + ARM_SMMU_GR0_SMR(0));

		mask = (smr >> SMR_MASK_SHIFT) & SMR_MASK_MASK;
		sid = (smr >> SMR_ID_SHIFT) & SMR_ID_MASK;
		if ((mask & sid) != sid) {
			dev_err(smmu->dev,
				"SMR mask bits (0x%x) insufficient for ID field (0x%x)\n",
				mask, sid);
			return -ENODEV;
		}

		dev_notice(smmu->dev,
			   "\tstream matching with %u register groups, mask 0x%x",
			   smmu->num_mapping_groups, mask);
	}

	/* ID1 */
	id = readl_relaxed(gr0_base + ARM_SMMU_GR0_ID1);
	smmu->pagesize = (id & ID1_PAGESIZE) ? SZ_64K : SZ_4K;

	/* Check for size mismatch of SMMU address space from mapped region */
	size = 1 <<
		(((id >> ID1_NUMPAGENDXB_SHIFT) & ID1_NUMPAGENDXB_MASK) + 1);
	size *= (smmu->pagesize << 1);
	if (smmu->size != size)
		dev_warn(smmu->dev,
			"SMMU address space size (0x%lx) differs from mapped region size (0x%lx)!\n",
			size, smmu->size);

	smmu->num_s2_context_banks = (id >> ID1_NUMS2CB_SHIFT) &
				      ID1_NUMS2CB_MASK;
	smmu->num_context_banks = (id >> ID1_NUMCB_SHIFT) & ID1_NUMCB_MASK;
	if (smmu->num_s2_context_banks > smmu->num_context_banks) {
		dev_err(smmu->dev, "impossible number of S2 context banks!\n");
		return -ENODEV;
	}
	dev_notice(smmu->dev, "\t%u context banks (%u stage-2 only)\n",
		   smmu->num_context_banks, smmu->num_s2_context_banks);

	/* ID2 */
	id = readl_relaxed(gr0_base + ARM_SMMU_GR0_ID2);
	size = arm_smmu_id_size_to_bits((id >> ID2_IAS_SHIFT) & ID2_IAS_MASK);

	/*
	 * Stage-1 output limited by stage-2 input size due to pgd
	 * allocation (PTRS_PER_PGD).
	 */
	if (smmu->features & ARM_SMMU_FEAT_TRANS_NESTED) {
#ifdef CONFIG_64BIT
		smmu->s1_output_size = min_t(unsigned long, VA_BITS, size);
#else
		smmu->s1_output_size = min(32UL, size);
#endif
	} else {
		smmu->s1_output_size = min_t(unsigned long, PHYS_MASK_SHIFT,
					     size);
	}

	/* The stage-2 output mask is also applied for bypass */
	size = arm_smmu_id_size_to_bits((id >> ID2_OAS_SHIFT) & ID2_OAS_MASK);
	smmu->s2_output_size = min_t(unsigned long, PHYS_MASK_SHIFT, size);

	/*
	 * What the page table walker can address actually depends on which
	 * descriptor format is in use, but since a) we don't know that yet,
	 * and b) it can vary per context bank, this will have to do...
	 */
	dma_set_mask_and_coherent(smmu->dev, DMA_BIT_MASK(size));

	if (smmu->version == 1) {
		smmu->input_size = 32;
	} else {
#ifdef CONFIG_64BIT
		size = (id >> ID2_UBS_SHIFT) & ID2_UBS_MASK;
		size = min(VA_BITS, arm_smmu_id_size_to_bits(size));
#else
		size = 32;
#endif
		smmu->input_size = size;

		if ((PAGE_SIZE == SZ_4K && !(id & ID2_PTFS_4K)) ||
		    (PAGE_SIZE == SZ_64K && !(id & ID2_PTFS_64K)) ||
		    (PAGE_SIZE != SZ_4K && PAGE_SIZE != SZ_64K)) {
			dev_err(smmu->dev, "CPU page size 0x%lx unsupported\n",
				PAGE_SIZE);
			return -ENODEV;
		}
	}

	dev_notice(smmu->dev,
		   "\t%lu-bit VA, %lu-bit IPA, %lu-bit PA\n",
		   smmu->input_size, smmu->s1_output_size,
		   smmu->s2_output_size);
	return 0;
}

static int arm_smmu_device_dt_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct arm_smmu_device *smmu;
	struct device *dev = &pdev->dev;
	struct rb_node *node;
	struct of_phandle_args masterspec;
	int num_irqs, i, err;

	smmu = devm_kzalloc(dev, sizeof(*smmu), GFP_KERNEL);
	if (!smmu) {
		dev_err(dev, "failed to allocate arm_smmu_device\n");
		return -ENOMEM;
	}
	smmu->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	smmu->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(smmu->base))
		return PTR_ERR(smmu->base);
	smmu->size = resource_size(res);

	if (of_property_read_u32(dev->of_node, "#global-interrupts",
				 &smmu->num_global_irqs)) {
		dev_err(dev, "missing #global-interrupts property\n");
		return -ENODEV;
	}

	num_irqs = 0;
	while ((res = platform_get_resource(pdev, IORESOURCE_IRQ, num_irqs))) {
		num_irqs++;
		if (num_irqs > smmu->num_global_irqs)
			smmu->num_context_irqs++;
	}

	if (!smmu->num_context_irqs) {
		dev_err(dev, "found %d interrupts but expected at least %d\n",
			num_irqs, smmu->num_global_irqs + 1);
		return -ENODEV;
	}

	smmu->irqs = devm_kzalloc(dev, sizeof(*smmu->irqs) * num_irqs,
				  GFP_KERNEL);
	if (!smmu->irqs) {
		dev_err(dev, "failed to allocate %d irqs\n", num_irqs);
		return -ENOMEM;
	}

	for (i = 0; i < num_irqs; ++i) {
		int irq = platform_get_irq(pdev, i);

		if (irq < 0) {
			dev_err(dev, "failed to get irq index %d\n", i);
			return -ENODEV;
		}
		smmu->irqs[i] = irq;
	}

	i = 0;
	smmu->masters = RB_ROOT;
	while (!of_parse_phandle_with_args(dev->of_node, "mmu-masters",
					   "#stream-id-cells", i,
					   &masterspec)) {
		err = register_smmu_master(smmu, dev, &masterspec);
		if (err) {
			dev_err(dev, "failed to add master %s\n",
				masterspec.np->name);
			goto out_put_masters;
		}

		i++;
	}
	dev_notice(dev, "registered %d master devices\n", i);

	err = arm_smmu_device_cfg_probe(smmu);
	if (err)
		goto out_put_masters;

	parse_driver_options(smmu);

	if (smmu->version > 1 &&
	    smmu->num_context_banks != smmu->num_context_irqs) {
		dev_err(dev,
			"found only %d context interrupt(s) but %d required\n",
			smmu->num_context_irqs, smmu->num_context_banks);
		err = -ENODEV;
		goto out_put_masters;
	}

	for (i = 0; i < smmu->num_global_irqs; ++i) {
		err = request_threaded_irq(smmu->irqs[i],
					NULL, arm_smmu_global_fault,
					IRQF_ONESHOT | IRQF_SHARED,
					"arm-smmu global fault", smmu);
		if (err) {
			dev_err(dev, "failed to request global IRQ %d (%u)\n",
				i, smmu->irqs[i]);
			goto out_free_irqs;
		}
	}

	INIT_LIST_HEAD(&smmu->list);
	spin_lock(&arm_smmu_devices_lock);
	list_add(&smmu->list, &arm_smmu_devices);
	spin_unlock(&arm_smmu_devices_lock);

	arm_smmu_device_reset(smmu);
	return 0;

out_free_irqs:
	while (i--)
		free_irq(smmu->irqs[i], smmu);

out_put_masters:
	for (node = rb_first(&smmu->masters); node; node = rb_next(node)) {
		struct arm_smmu_master *master
			= container_of(node, struct arm_smmu_master, node);
		of_node_put(master->of_node);
	}

	return err;
}

static int arm_smmu_device_remove(struct platform_device *pdev)
{
	int i;
	struct device *dev = &pdev->dev;
	struct arm_smmu_device *curr, *smmu = NULL;
	struct rb_node *node;

	spin_lock(&arm_smmu_devices_lock);
	list_for_each_entry(curr, &arm_smmu_devices, list) {
		if (curr->dev == dev) {
			smmu = curr;
			list_del(&smmu->list);
			break;
		}
	}
	spin_unlock(&arm_smmu_devices_lock);

	if (!smmu)
		return -ENODEV;

	for (node = rb_first(&smmu->masters); node; node = rb_next(node)) {
		struct arm_smmu_master *master
			= container_of(node, struct arm_smmu_master, node);
		of_node_put(master->of_node);
	}

	if (!bitmap_empty(smmu->context_map, ARM_SMMU_MAX_CBS))
		dev_err(dev, "removing device with active domains!\n");

	for (i = 0; i < smmu->num_global_irqs; ++i)
		free_irq(smmu->irqs[i], smmu);

	/* Turn the thing off */
	writel(sCR0_CLIENTPD, ARM_SMMU_GR0_NS(smmu) + ARM_SMMU_GR0_sCR0);
	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id arm_smmu_of_match[] = {
	{ .compatible = "arm,smmu-v1", },
	{ .compatible = "arm,smmu-v2", },
	{ .compatible = "arm,mmu-400", },
	{ .compatible = "arm,mmu-500", },
	{ },
};
MODULE_DEVICE_TABLE(of, arm_smmu_of_match);
#endif

static struct platform_driver arm_smmu_driver = {
	.driver	= {
		.owner		= THIS_MODULE,
		.name		= "arm-smmu",
		.of_match_table	= of_match_ptr(arm_smmu_of_match),
	},
	.probe	= arm_smmu_device_dt_probe,
	.remove	= arm_smmu_device_remove,
};

static int __init arm_smmu_init(void)
{
	int ret;

	ret = platform_driver_register(&arm_smmu_driver);
	if (ret)
		return ret;

	/* Oh, for a proper bus abstraction */
	if (!iommu_present(&platform_bus_type))
		bus_set_iommu(&platform_bus_type, &arm_smmu_ops);

#ifdef CONFIG_ARM_AMBA
	if (!iommu_present(&amba_bustype))
		bus_set_iommu(&amba_bustype, &arm_smmu_ops);
#endif

#ifdef CONFIG_PCI
	if (!iommu_present(&pci_bus_type))
		bus_set_iommu(&pci_bus_type, &arm_smmu_ops);
#endif

	return 0;
}

static void __exit arm_smmu_exit(void)
{
	return platform_driver_unregister(&arm_smmu_driver);
}

subsys_initcall(arm_smmu_init);
module_exit(arm_smmu_exit);

MODULE_DESCRIPTION("IOMMU API for ARM architected SMMU implementations");
MODULE_AUTHOR("Will Deacon <will.deacon@arm.com>");
MODULE_LICENSE("GPL v2");
