/*
 * @file op_model_amd.c
 * athlon / K7 / K8 / Family 10h model-specific MSR operations
 *
 * @remark Copyright 2002-2008 OProfile authors
 * @remark Read the file COPYING
 *
 * @author John Levon
 * @author Philippe Elie
 * @author Graydon Hoare
 * @author Robert Richter <robert.richter@amd.com>
 * @author Barry Kasindorf
*/

#include <linux/oprofile.h>
#include <linux/device.h>
#include <linux/pci.h>

#include <asm/ptrace.h>
#include <asm/msr.h>
#include <asm/nmi.h>

#include "op_x86_model.h"
#include "op_counter.h"

#define NUM_COUNTERS 4
#define NUM_CONTROLS 4

#define CTR_IS_RESERVED(msrs, c) (msrs->counters[(c)].addr ? 1 : 0)
#define CTR_READ(l, h, msrs, c) do {rdmsr(msrs->counters[(c)].addr, (l), (h)); } while (0)
#define CTR_WRITE(l, msrs, c) do {wrmsr(msrs->counters[(c)].addr, -(unsigned int)(l), -1); } while (0)
#define CTR_OVERFLOWED(n) (!((n) & (1U<<31)))

#define CTRL_IS_RESERVED(msrs, c) (msrs->controls[(c)].addr ? 1 : 0)
#define CTRL_READ(l, h, msrs, c) do {rdmsr(msrs->controls[(c)].addr, (l), (h)); } while (0)
#define CTRL_WRITE(l, h, msrs, c) do {wrmsr(msrs->controls[(c)].addr, (l), (h)); } while (0)
#define CTRL_SET_ACTIVE(n) (n |= (1<<22))
#define CTRL_SET_INACTIVE(n) (n &= ~(1<<22))
#define CTRL_CLEAR_LO(x) (x &= (1<<21))
#define CTRL_CLEAR_HI(x) (x &= 0xfffffcf0)
#define CTRL_SET_ENABLE(val) (val |= 1<<20)
#define CTRL_SET_USR(val, u) (val |= ((u & 1) << 16))
#define CTRL_SET_KERN(val, k) (val |= ((k & 1) << 17))
#define CTRL_SET_UM(val, m) (val |= (m << 8))
#define CTRL_SET_EVENT_LOW(val, e) (val |= (e & 0xff))
#define CTRL_SET_EVENT_HIGH(val, e) (val |= ((e >> 8) & 0xf))
#define CTRL_SET_HOST_ONLY(val, h) (val |= ((h & 1) << 9))
#define CTRL_SET_GUEST_ONLY(val, h) (val |= ((h & 1) << 8))

static unsigned long reset_value[NUM_COUNTERS];

#ifdef CONFIG_OPROFILE_IBS

/* IbsFetchCtl bits/masks */
#define IBS_FETCH_HIGH_VALID_BIT	(1UL << 17)	/* bit 49 */
#define IBS_FETCH_HIGH_ENABLE		(1UL << 16)	/* bit 48 */
#define IBS_FETCH_LOW_MAX_CNT_MASK	0x0000FFFFUL	/* MaxCnt mask */

/*IbsOpCtl bits */
#define IBS_OP_LOW_VALID_BIT		(1ULL<<18)	/* bit 18 */
#define IBS_OP_LOW_ENABLE		(1ULL<<17)	/* bit 17 */

/* Codes used in cpu_buffer.c */
/* This produces duplicate code, need to be fixed */
#define IBS_FETCH_BEGIN 3
#define IBS_OP_BEGIN    4

/* The function interface needs to be fixed, something like add
   data. Should then be added to linux/oprofile.h. */
extern void
oprofile_add_ibs_sample(struct pt_regs *const regs,
			unsigned int *const ibs_sample, int ibs_code);

struct ibs_fetch_sample {
	/* MSRC001_1031 IBS Fetch Linear Address Register */
	unsigned int ibs_fetch_lin_addr_low;
	unsigned int ibs_fetch_lin_addr_high;
	/* MSRC001_1030 IBS Fetch Control Register */
	unsigned int ibs_fetch_ctl_low;
	unsigned int ibs_fetch_ctl_high;
	/* MSRC001_1032 IBS Fetch Physical Address Register */
	unsigned int ibs_fetch_phys_addr_low;
	unsigned int ibs_fetch_phys_addr_high;
};

struct ibs_op_sample {
	/* MSRC001_1034 IBS Op Logical Address Register (IbsRIP) */
	unsigned int ibs_op_rip_low;
	unsigned int ibs_op_rip_high;
	/* MSRC001_1035 IBS Op Data Register */
	unsigned int ibs_op_data1_low;
	unsigned int ibs_op_data1_high;
	/* MSRC001_1036 IBS Op Data 2 Register */
	unsigned int ibs_op_data2_low;
	unsigned int ibs_op_data2_high;
	/* MSRC001_1037 IBS Op Data 3 Register */
	unsigned int ibs_op_data3_low;
	unsigned int ibs_op_data3_high;
	/* MSRC001_1038 IBS DC Linear Address Register (IbsDcLinAd) */
	unsigned int ibs_dc_linear_low;
	unsigned int ibs_dc_linear_high;
	/* MSRC001_1039 IBS DC Physical Address Register (IbsDcPhysAd) */
	unsigned int ibs_dc_phys_low;
	unsigned int ibs_dc_phys_high;
};

/*
 * unitialize the APIC for the IBS interrupts if needed on AMD Family10h+
*/
static void clear_ibs_nmi(void);

static int ibs_allowed;	/* AMD Family10h and later */

struct op_ibs_config {
	unsigned long op_enabled;
	unsigned long fetch_enabled;
	unsigned long max_cnt_fetch;
	unsigned long max_cnt_op;
	unsigned long rand_en;
	unsigned long dispatched_ops;
};

static struct op_ibs_config ibs_config;

#endif

/* functions for op_amd_spec */

static void op_amd_fill_in_addresses(struct op_msrs * const msrs)
{
	int i;

	for (i = 0; i < NUM_COUNTERS; i++) {
		if (reserve_perfctr_nmi(MSR_K7_PERFCTR0 + i))
			msrs->counters[i].addr = MSR_K7_PERFCTR0 + i;
		else
			msrs->counters[i].addr = 0;
	}

	for (i = 0; i < NUM_CONTROLS; i++) {
		if (reserve_evntsel_nmi(MSR_K7_EVNTSEL0 + i))
			msrs->controls[i].addr = MSR_K7_EVNTSEL0 + i;
		else
			msrs->controls[i].addr = 0;
	}
}


static void op_amd_setup_ctrs(struct op_msrs const * const msrs)
{
	unsigned int low, high;
	int i;

	/* clear all counters */
	for (i = 0 ; i < NUM_CONTROLS; ++i) {
		if (unlikely(!CTRL_IS_RESERVED(msrs, i)))
			continue;
		CTRL_READ(low, high, msrs, i);
		CTRL_CLEAR_LO(low);
		CTRL_CLEAR_HI(high);
		CTRL_WRITE(low, high, msrs, i);
	}

	/* avoid a false detection of ctr overflows in NMI handler */
	for (i = 0; i < NUM_COUNTERS; ++i) {
		if (unlikely(!CTR_IS_RESERVED(msrs, i)))
			continue;
		CTR_WRITE(1, msrs, i);
	}

	/* enable active counters */
	for (i = 0; i < NUM_COUNTERS; ++i) {
		if ((counter_config[i].enabled) && (CTR_IS_RESERVED(msrs, i))) {
			reset_value[i] = counter_config[i].count;

			CTR_WRITE(counter_config[i].count, msrs, i);

			CTRL_READ(low, high, msrs, i);
			CTRL_CLEAR_LO(low);
			CTRL_CLEAR_HI(high);
			CTRL_SET_ENABLE(low);
			CTRL_SET_USR(low, counter_config[i].user);
			CTRL_SET_KERN(low, counter_config[i].kernel);
			CTRL_SET_UM(low, counter_config[i].unit_mask);
			CTRL_SET_EVENT_LOW(low, counter_config[i].event);
			CTRL_SET_EVENT_HIGH(high, counter_config[i].event);
			CTRL_SET_HOST_ONLY(high, 0);
			CTRL_SET_GUEST_ONLY(high, 0);

			CTRL_WRITE(low, high, msrs, i);
		} else {
			reset_value[i] = 0;
		}
	}
}

#ifdef CONFIG_OPROFILE_IBS

static inline int
op_amd_handle_ibs(struct pt_regs * const regs,
		  struct op_msrs const * const msrs)
{
	unsigned int low, high;
	struct ibs_fetch_sample ibs_fetch;
	struct ibs_op_sample ibs_op;

	if (!ibs_allowed)
		return 1;

	if (ibs_config.fetch_enabled) {
		rdmsr(MSR_AMD64_IBSFETCHCTL, low, high);
		if (high & IBS_FETCH_HIGH_VALID_BIT) {
			ibs_fetch.ibs_fetch_ctl_high = high;
			ibs_fetch.ibs_fetch_ctl_low = low;
			rdmsr(MSR_AMD64_IBSFETCHLINAD, low, high);
			ibs_fetch.ibs_fetch_lin_addr_high = high;
			ibs_fetch.ibs_fetch_lin_addr_low = low;
			rdmsr(MSR_AMD64_IBSFETCHPHYSAD, low, high);
			ibs_fetch.ibs_fetch_phys_addr_high = high;
			ibs_fetch.ibs_fetch_phys_addr_low = low;

			oprofile_add_ibs_sample(regs,
						(unsigned int *)&ibs_fetch,
						IBS_FETCH_BEGIN);

			/*reenable the IRQ */
			rdmsr(MSR_AMD64_IBSFETCHCTL, low, high);
			high &= ~IBS_FETCH_HIGH_VALID_BIT;
			high |= IBS_FETCH_HIGH_ENABLE;
			low &= IBS_FETCH_LOW_MAX_CNT_MASK;
			wrmsr(MSR_AMD64_IBSFETCHCTL, low, high);
		}
	}

	if (ibs_config.op_enabled) {
		rdmsr(MSR_AMD64_IBSOPCTL, low, high);
		if (low & IBS_OP_LOW_VALID_BIT) {
			rdmsr(MSR_AMD64_IBSOPRIP, low, high);
			ibs_op.ibs_op_rip_low = low;
			ibs_op.ibs_op_rip_high = high;
			rdmsr(MSR_AMD64_IBSOPDATA, low, high);
			ibs_op.ibs_op_data1_low = low;
			ibs_op.ibs_op_data1_high = high;
			rdmsr(MSR_AMD64_IBSOPDATA2, low, high);
			ibs_op.ibs_op_data2_low = low;
			ibs_op.ibs_op_data2_high = high;
			rdmsr(MSR_AMD64_IBSOPDATA3, low, high);
			ibs_op.ibs_op_data3_low = low;
			ibs_op.ibs_op_data3_high = high;
			rdmsr(MSR_AMD64_IBSDCLINAD, low, high);
			ibs_op.ibs_dc_linear_low = low;
			ibs_op.ibs_dc_linear_high = high;
			rdmsr(MSR_AMD64_IBSDCPHYSAD, low, high);
			ibs_op.ibs_dc_phys_low = low;
			ibs_op.ibs_dc_phys_high = high;

			/* reenable the IRQ */
			oprofile_add_ibs_sample(regs,
						(unsigned int *)&ibs_op,
						IBS_OP_BEGIN);
			rdmsr(MSR_AMD64_IBSOPCTL, low, high);
			high = 0;
			low &= ~IBS_OP_LOW_VALID_BIT;
			low |= IBS_OP_LOW_ENABLE;
			wrmsr(MSR_AMD64_IBSOPCTL, low, high);
		}
	}

	return 1;
}

#endif

static int op_amd_check_ctrs(struct pt_regs * const regs,
			     struct op_msrs const * const msrs)
{
	unsigned int low, high;
	int i;

	for (i = 0 ; i < NUM_COUNTERS; ++i) {
		if (!reset_value[i])
			continue;
		CTR_READ(low, high, msrs, i);
		if (CTR_OVERFLOWED(low)) {
			oprofile_add_sample(regs, i);
			CTR_WRITE(reset_value[i], msrs, i);
		}
	}

#ifdef CONFIG_OPROFILE_IBS
	op_amd_handle_ibs(regs, msrs);
#endif

	/* See op_model_ppro.c */
	return 1;
}

static void op_amd_start(struct op_msrs const * const msrs)
{
	unsigned int low, high;
	int i;
	for (i = 0 ; i < NUM_COUNTERS ; ++i) {
		if (reset_value[i]) {
			CTRL_READ(low, high, msrs, i);
			CTRL_SET_ACTIVE(low);
			CTRL_WRITE(low, high, msrs, i);
		}
	}

#ifdef CONFIG_OPROFILE_IBS
	if (ibs_allowed && ibs_config.fetch_enabled) {
		low = (ibs_config.max_cnt_fetch >> 4) & 0xFFFF;
		high = ((ibs_config.rand_en & 0x1) << 25) /* bit 57 */
			+ IBS_FETCH_HIGH_ENABLE;
		wrmsr(MSR_AMD64_IBSFETCHCTL, low, high);
	}

	if (ibs_allowed && ibs_config.op_enabled) {
		low = ((ibs_config.max_cnt_op >> 4) & 0xFFFF)
			+ ((ibs_config.dispatched_ops & 0x1) << 19) /* bit 19 */
			+ IBS_OP_LOW_ENABLE;
		high = 0;
		wrmsr(MSR_AMD64_IBSOPCTL, low, high);
	}
#endif
}


static void op_amd_stop(struct op_msrs const * const msrs)
{
	unsigned int low, high;
	int i;

	/* Subtle: stop on all counters to avoid race with
	 * setting our pm callback */
	for (i = 0 ; i < NUM_COUNTERS ; ++i) {
		if (!reset_value[i])
			continue;
		CTRL_READ(low, high, msrs, i);
		CTRL_SET_INACTIVE(low);
		CTRL_WRITE(low, high, msrs, i);
	}

#ifdef CONFIG_OPROFILE_IBS
	if (ibs_allowed && ibs_config.fetch_enabled) {
		low = 0;		/* clear max count and enable */
		high = 0;
		wrmsr(MSR_AMD64_IBSFETCHCTL, low, high);
	}

	if (ibs_allowed && ibs_config.op_enabled) {
		low = 0;		/* clear max count and enable */
		high = 0;
		wrmsr(MSR_AMD64_IBSOPCTL, low, high);
	}
#endif
}

static void op_amd_shutdown(struct op_msrs const * const msrs)
{
	int i;

	for (i = 0 ; i < NUM_COUNTERS ; ++i) {
		if (CTR_IS_RESERVED(msrs, i))
			release_perfctr_nmi(MSR_K7_PERFCTR0 + i);
	}
	for (i = 0 ; i < NUM_CONTROLS ; ++i) {
		if (CTRL_IS_RESERVED(msrs, i))
			release_evntsel_nmi(MSR_K7_EVNTSEL0 + i);
	}
}

#ifndef CONFIG_OPROFILE_IBS

/* no IBS support */

static int op_amd_init(struct oprofile_operations *ops)
{
	return 0;
}

static void op_amd_exit(void) {}

#else

static u8 ibs_eilvt_off;

static inline void apic_init_ibs_nmi_per_cpu(void *arg)
{
	ibs_eilvt_off = setup_APIC_eilvt_ibs(0, APIC_EILVT_MSG_NMI, 0);
}

static inline void apic_clear_ibs_nmi_per_cpu(void *arg)
{
	setup_APIC_eilvt_ibs(0, APIC_EILVT_MSG_FIX, 1);
}

static int pfm_amd64_setup_eilvt(void)
{
#define IBSCTL_LVTOFFSETVAL		(1 << 8)
#define IBSCTL				0x1cc
	struct pci_dev *cpu_cfg;
	int nodes;
	u32 value = 0;

	/* per CPU setup */
	on_each_cpu(apic_init_ibs_nmi_per_cpu, NULL, 1);

	nodes = 0;
	cpu_cfg = NULL;
	do {
		cpu_cfg = pci_get_device(PCI_VENDOR_ID_AMD,
					 PCI_DEVICE_ID_AMD_10H_NB_MISC,
					 cpu_cfg);
		if (!cpu_cfg)
			break;
		++nodes;
		pci_write_config_dword(cpu_cfg, IBSCTL, ibs_eilvt_off
				       | IBSCTL_LVTOFFSETVAL);
		pci_read_config_dword(cpu_cfg, IBSCTL, &value);
		if (value != (ibs_eilvt_off | IBSCTL_LVTOFFSETVAL)) {
			printk(KERN_DEBUG "Failed to setup IBS LVT offset, "
				"IBSCTL = 0x%08x", value);
			return 1;
		}
	} while (1);

	if (!nodes) {
		printk(KERN_DEBUG "No CPU node configured for IBS");
		return 1;
	}

#ifdef CONFIG_NUMA
	/* Sanity check */
	/* Works only for 64bit with proper numa implementation. */
	if (nodes != num_possible_nodes()) {
		printk(KERN_DEBUG "Failed to setup CPU node(s) for IBS, "
			"found: %d, expected %d",
			nodes, num_possible_nodes());
		return 1;
	}
#endif
	return 0;
}

/*
 * initialize the APIC for the IBS interrupts
 * if available (AMD Family10h rev B0 and later)
 */
static void setup_ibs(void)
{
	ibs_allowed = boot_cpu_has(X86_FEATURE_IBS);

	if (!ibs_allowed)
		return;

	if (pfm_amd64_setup_eilvt()) {
		ibs_allowed = 0;
		return;
	}

	printk(KERN_INFO "oprofile: AMD IBS detected\n");
}


/*
 * unitialize the APIC for the IBS interrupts if needed on AMD Family10h
 * rev B0 and later */
static void clear_ibs_nmi(void)
{
	if (ibs_allowed)
		on_each_cpu(apic_clear_ibs_nmi_per_cpu, NULL, 1);
}

static int (*create_arch_files)(struct super_block *sb, struct dentry *root);

static int setup_ibs_files(struct super_block *sb, struct dentry *root)
{
	struct dentry *dir;
	int ret = 0;

	/* architecture specific files */
	if (create_arch_files)
		ret = create_arch_files(sb, root);

	if (ret)
		return ret;

	if (!ibs_allowed)
		return ret;

	/* model specific files */

	/* setup some reasonable defaults */
	ibs_config.max_cnt_fetch = 250000;
	ibs_config.fetch_enabled = 0;
	ibs_config.max_cnt_op = 250000;
	ibs_config.op_enabled = 0;
	ibs_config.dispatched_ops = 1;

	dir = oprofilefs_mkdir(sb, root, "ibs_fetch");
	oprofilefs_create_ulong(sb, dir, "enable",
				&ibs_config.fetch_enabled);
	oprofilefs_create_ulong(sb, dir, "max_count",
				&ibs_config.max_cnt_fetch);
	oprofilefs_create_ulong(sb, dir, "rand_enable",
				&ibs_config.rand_en);

	dir = oprofilefs_mkdir(sb, root, "ibs_op");
	oprofilefs_create_ulong(sb, dir, "enable",
				&ibs_config.op_enabled);
	oprofilefs_create_ulong(sb, dir, "max_count",
				&ibs_config.max_cnt_op);
	oprofilefs_create_ulong(sb, dir, "dispatched_ops",
				&ibs_config.dispatched_ops);

	return 0;
}

static int op_amd_init(struct oprofile_operations *ops)
{
	setup_ibs();
	create_arch_files = ops->create_files;
	ops->create_files = setup_ibs_files;
	return 0;
}

static void op_amd_exit(void)
{
	clear_ibs_nmi();
}

#endif

struct op_x86_model_spec const op_amd_spec = {
	.init			= op_amd_init,
	.exit			= op_amd_exit,
	.num_counters		= NUM_COUNTERS,
	.num_controls		= NUM_CONTROLS,
	.fill_in_addresses	= &op_amd_fill_in_addresses,
	.setup_ctrs		= &op_amd_setup_ctrs,
	.check_ctrs		= &op_amd_check_ctrs,
	.start			= &op_amd_start,
	.stop			= &op_amd_stop,
	.shutdown		= &op_amd_shutdown
};
