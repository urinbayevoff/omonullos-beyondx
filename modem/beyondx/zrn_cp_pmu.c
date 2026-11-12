// SPDX-License-Identifier: GPL-2.0
/*
 * The CP's power sequences on the Exynos 9820 (Galaxy S10 5G), as Samsung's
 * PMU CAL runs them (drivers/soc/samsung/cal-if: pmucal_cp.c, pmucal_rae.c and
 * exynos9820/pmucal_cp_exynos9820.h): writes and waits on PMU registers, the
 * two CP_CTRL registers through the secure monitor, which owns them.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>

#define PMU_BASE	0x15860000
#define GICD_BASE	0x10100000	/* unused on the 9820, kept for base_of() */
#define CP_CTRL_NS	0x3010
#define CP_CTRL_S	0x3014
#define READ_CTRL	0x3
#define WRITE_CTRL	0x4

enum op { OP_WRITE, OP_READ, OP_WAIT, OP_DELAY };

struct step {
	enum op op;
	const char *name;
	u32 base, offset, mask, value;
};

#define W(n, o, m, v)	{ OP_WRITE, n, PMU_BASE, o, m, v }
#define R(n, o, m)	{ OP_READ, n, PMU_BASE, o, m, 0 }
#define T(n, b, o, m, v){ OP_WAIT, n, b, o, m, v }
#define D(us)		{ OP_DELAY, "DELAY", 0, 0, 0, us }

/* exynos9820/pmucal_cp_exynos9820.h */
static const struct step cp_init[] = {
	W("CP_CTRL_S", CP_CTRL_S, 1 << 3, 1 << 3),
	W("CP_CONFIGURATION", 0x3000, 1 << 0, 1 << 0),
	T("CP_STATUS", PMU_BASE, 0x3004, 1 << 0, 1 << 0),
	T("CP_IN", PMU_BASE, 0x3024, 1 << 4, 1 << 4),
};
static const struct step cp_status[] = {
	R("CP_STATUS", 0x3004, 1 << 0),
};
static const struct step cp_reset_assert[] = {
	W("CP_CONFIGURATION", 0x3000, 1 << 0, 0),
	T("CP_STATUS", PMU_BASE, 0x3004, 1 << 0, 0),
};
static const struct step cp_reset_release[] = {
	W("CP_CONFIGURATION", 0x3000, 1 << 0, 1 << 0),
	T("CP_STATUS", PMU_BASE, 0x3004, 1 << 0, 1 << 0),
	T("CP_IN", PMU_BASE, 0x3024, 1 << 4, 1 << 4),
};
/* the 9820 list has no cp_active_clear / cp_reset_req_clear: Samsung's CAL
 * returns -ENOENT for them, which its callers ignore */
static const struct step cp_active_clear[] = {};
static const struct step cp_reset_req_clear[] = {};
static const struct step cp_dump_pc_on[] = {
	W("CP_OUT", 0x3020, 1 << 16, 1 << 16),
};
static const struct step cp_dump_pc_off[] = {
	W("CP_OUT", 0x3020, 1 << 16, 0),
};

static void __iomem *pmu, *gicd;

static void __iomem *base_of(u32 base)
{
	if (!pmu) {
		pmu = ioremap(PMU_BASE, 0x4000);
		gicd = ioremap(GICD_BASE, 0x2000);
	}
	return base == GICD_BASE ? gicd : pmu;
}

/* CP_CTRL_S / CP_CTRL_NS through the monitor: 16 bits at a time, the answer
 * in the upper half of x0 and an error in the lower */
static int ctrl_read(u32 offset, u32 *out)
{
	u32 reg = offset == CP_CTRL_S ? 0 : 1, lo, hi;
	int r = exynos_smc(SMC_ID, READ_CTRL, 0, reg);
	if (r & 0xffff)
		return -EIO;
	lo = (u32)r >> 16;
	r = exynos_smc(SMC_ID, READ_CTRL, 1, reg);
	if (r & 0xffff)
		return -EIO;
	hi = (u32)r >> 16;
	*out = hi << 16 | lo;
	return 0;
}

static int ctrl_write(u32 offset, u32 mask, u32 value)
{
	u32 reg = offset == CP_CTRL_S ? 0 : 1, now = 0;
	if (mask != U32_MAX && ctrl_read(offset, &now))
		return -EIO;
	value = (value & mask) | (now & ~mask);
	if (exynos_smc(SMC_ID, WRITE_CTRL, value, reg) > 0)
		return -EIO;
	pr_info("zrn-modem: pmu %s = 0x%08x\n", offset == CP_CTRL_S ? "CP_CTRL_S" : "CP_CTRL_NS", value);
	return 0;
}

static int run(const struct step *s, int n, u32 *read)
{
	for (int i = 0; i < n; i++, s++) {
		void __iomem *reg = base_of(s->base) + s->offset;
		bool secure = s->base == PMU_BASE && (s->offset == CP_CTRL_S || s->offset == CP_CTRL_NS);
		switch (s->op) {
		case OP_WRITE:
			if (secure) {
				if (ctrl_write(s->offset, s->mask, s->value))
					return -EIO;
			} else {
				writel((readl(reg) & ~s->mask) | s->value, reg);
			}
			break;
		case OP_READ:
			if (secure) {
				u32 v;
				if (ctrl_read(s->offset, &v))
					return -EIO;
				*read = v & s->mask;
			} else {
				*read = readl(reg) & s->mask;
			}
			break;
		case OP_WAIT: {
			int us = 0;
			while ((readl(reg) & s->mask) != s->value) {
				if (++us > 1000) {
					pr_err("zrn-modem: pmu %s timed out (0x%08x)\n", s->name, readl(reg));
					return -ETIMEDOUT;
				}
				udelay(1);
			}
			break;
		}
		case OP_DELAY:
			udelay(s->value);
			break;
		}
	}
	return 0;
}

#define RUN(seq)	run(seq, ARRAY_SIZE(seq), NULL)

int cal_cp_init(void)			{ pr_info("zrn-modem: cp init\n"); return RUN(cp_init); }
int cal_cp_reset_assert(void)		{ pr_info("zrn-modem: cp reset assert\n"); return RUN(cp_reset_assert); }
int cal_cp_reset_release(void)		{ pr_info("zrn-modem: cp reset release\n"); return RUN(cp_reset_release); }
void cal_cp_active_clear(void)		{ RUN(cp_active_clear); }
void cal_cp_reset_req_clear(void)	{ RUN(cp_reset_req_clear); }
void cal_cp_enable_dump_pc_no_pg(void)	{ RUN(cp_dump_pc_on); }
void cal_cp_disable_dump_pc_no_pg(void)	{ RUN(cp_dump_pc_off); }

/* 1 when the CP is on (CP_STATUS bit 0), else 0 */
int cal_cp_status(void)
{
	u32 v = 0;
	int ret = run(cp_status, ARRAY_SIZE(cp_status), &v);
	return ret < 0 ? ret : v == 0x1;
}

