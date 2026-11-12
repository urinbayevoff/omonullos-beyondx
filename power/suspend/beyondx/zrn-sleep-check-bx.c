// SPDX-License-Identifier: GPL-2.0-only
/*
 * zrn-sleep-check-bx: Exynos 9820 (S10 5G) system-sleep tables, check only.
 *
 * Step 1/2 of the SYS_SLEEP port (docs/s10-5g/ROADMAP.md "tizim uyqusi").
 * Loading validates the vendor PMUCAL tables (sleep-tables-9820.h) and
 * ioremaps the 64K regions they name. ioremap does not touch the bus: no
 * register is read or written at load.
 *
 * probe_save (write 1, root, owner's «ha»): runs the save_sleep table in
 * SAVE mode = reads only, never writes. Every guarded block is read only
 * when its PMU domain STATUS says on; the one switchable domain the table
 * reads without a guard is G3D (CMU_G3D 0x18400000), so the probe refuses
 * unless G3D_STATUS (PMU 0x15862384) bit 0 is set. The probe touches the
 * PMU status registers first and nothing else when G3D is off.
 *
 * Nothing here calls the firmware or changes power state.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include "sequence.h"
#include "sleep-tables-9820.h"

#define PMU		0x15860000u
#define G3D_STATUS	0x2384u
#define MAX_REGIONS	64

struct table { const char *name; struct zrn_seq *seq; unsigned int count; };
#define TABLE(n) { #n, n, ARRAY_SIZE(n) }
static struct table tables[] = {
	TABLE(enter_sleep), TABLE(save_sleep), TABLE(exit_sleep),
	TABLE(earlywkup_sleep), TABLE(enter_sleep_usbl2), TABLE(save_sleep_usbl2),
	TABLE(exit_sleep_usbl2), TABLE(earlywkup_sleep_usbl2),
};

static struct { u32 physical; void __iomem *mapped; } regions[MAX_REGIONS];
static unsigned int region_count, rows, probes, saved_rows, guarded_skipped;
static int last_error, failed_index = -1;
static u32 g3d_status;
static DEFINE_MUTEX(lock);
module_param(region_count, uint, 0444);
module_param(rows, uint, 0444);
module_param(probes, uint, 0444);
module_param(saved_rows, uint, 0444);
module_param(guarded_skipped, uint, 0444);
module_param(last_error, int, 0444);
module_param(failed_index, int, 0444);
module_param(g3d_status, uint, 0444);

static void __iomem *mapped_base(u32 base)
{
	unsigned int i;

	for (i = 0; i < region_count; i++)
		if (regions[i].physical == base)
			return regions[i].mapped;
	return NULL;
}

static int map_base(u32 base)
{
	void __iomem *mapped;

	if (!base || mapped_base(base))
		return 0;
	if (region_count == MAX_REGIONS || (base & 0xffff))
		return -EINVAL;
	mapped = ioremap(base, 0x10000);
	if (!mapped)
		return -ENOMEM;
	regions[region_count].physical = base;
	regions[region_count++].mapped = mapped;
	return 0;
}

static u32 reg_read(void *context, u32 base, u32 offset)
{
	return readl_relaxed(mapped_base(base) + offset);
}

/* SAVE never writes (sequence.c); refuse loudly if that ever changes. */
static void reg_write(void *context, u32 base, u32 offset, u32 value)
{
	WARN_ONCE(1, "zrn-sleep-check-bx: write %08x+%04x blocked\n", base, offset);
}

static void reg_delay(void *context, unsigned int usec) { udelay(usec); }
static const struct zrn_seq_io io = {
	.read = reg_read, .write = reg_write, .delay_us = reg_delay,
};

static bool known_op(enum zrn_seq_op op)
{
	switch (op) {
	case PMUCAL_READ: case PMUCAL_WRITE: case PMUCAL_COND_READ:
	case PMUCAL_COND_WRITE: case PMUCAL_SAVE_RESTORE:
	case PMUCAL_COND_SAVE_RESTORE: case PMUCAL_WAIT: case PMUCAL_WAIT_TWO:
	case PMUCAL_COND_CHECK_SKIP: case PMUCAL_CLEAR_PEND:
	case PMUCAL_SET_BIT_ATOMIC: case PMUCAL_CLR_BIT_ATOMIC:
		return true;
	default:
		return false;
	}
}

static int check_tables(void)
{
	unsigned int t, i;
	struct zrn_seq *s;
	int ret;

	for (t = 0; t < ARRAY_SIZE(tables); t++) {
		for (i = 0; i < tables[t].count; i++) {
			s = &tables[t].seq[i];
			if (!known_op(s->op) || s->offset >= 0x10000 ||
			    s->cond_offset >= 0x10000) {
				pr_err("zrn-sleep-check-bx: %s[%u] %s rejected\n",
				       tables[t].name, i, s->name);
				return -EINVAL;
			}
			ret = map_base(s->base);
			if (!ret)
				ret = map_base(s->cond_base);
			if (ret) {
				pr_err("zrn-sleep-check-bx: %s[%u] %s map %d\n",
				       tables[t].name, i, s->name, ret);
				return ret;
			}
			rows++;
		}
	}
	return map_base(PMU);
}

static int probe_set(const char *value, const struct kernel_param *kp)
{
	struct table *save = &tables[1];
	unsigned int i, failed = 0;
	bool on;
	int ret;

	ret = kstrtobool(value, &on);
	if (ret || !on)
		return ret;
	mutex_lock(&lock);
	probes++;
	g3d_status = readl_relaxed(mapped_base(PMU) + G3D_STATUS);
	if (!(g3d_status & 1)) {
		pr_warn("zrn-sleep-check-bx: G3D off (%08x): save probe refused\n",
			g3d_status);
		ret = -EBUSY;
		goto out;
	}
	ret = zrn_sequence_run(save->seq, save->count, ZRN_SAVE, &io, &failed);
	saved_rows = guarded_skipped = 0;
	for (i = 0; i < save->count; i++) {
		saved_rows += save->seq[i].valid;
		guarded_skipped += save->seq[i].op == PMUCAL_COND_SAVE_RESTORE &&
			!save->seq[i].valid;
		save->seq[i].valid = false;	/* nothing is ever restored here */
	}
	last_error = ret;
	failed_index = ret ? failed : -1;
	pr_info("zrn-sleep-check-bx: save probe ret=%d rows saved=%u guarded-off=%u\n",
		ret, saved_rows, guarded_skipped);
out:
	mutex_unlock(&lock);
	return ret;
}
static const struct kernel_param_ops probe_ops = { .set = probe_set };
module_param_cb(probe_save, &probe_ops, NULL, 0200);
MODULE_PARM_DESC(probe_save, "1: read-only save_sleep pass (needs G3D on)");

static void unmap_all(void)
{
	while (region_count)
		iounmap(regions[--region_count].mapped);
}

static int __init check_init(void)
{
	int ret = check_tables();

	if (ret) {
		unmap_all();
		return ret;
	}
	pr_info("zrn-sleep-check-bx: %u rows in %zu tables, %u regions mapped, no access\n",
		rows, ARRAY_SIZE(tables), region_count);
	return 0;
}

static void __exit check_exit(void)
{
	unmap_all();
}

module_init(check_init);
module_exit(check_exit);
MODULE_DESCRIPTION("Exynos 9820 system-sleep table check (read-only)");
MODULE_LICENSE("GPL");
