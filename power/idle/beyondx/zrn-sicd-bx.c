// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * zrn-sicd-bx: SICD, Samsung's System Idle Clock Down, on the Exynos 9820
 * (Galaxy S10 5G). Upstream ztOS's zrn-sicd.c (Exynos 9810) moved to the
 * 9820's flexible PMU, from Samsung's G977N sources:
 *
 *   cal-if/exynos9820/flexpmu_cal_system_exynos9820.h  enter/exit/earlywkup_sicd
 *   exynos9820.dts  cpupm "sicd" (psci-index 256, residency 10000 us,
 *                   entry-allowed 0-5), exynos-powermode wakeup-masks
 *
 * With every online core down in C2 and the earliest timer of them all at
 * least `residency` us away, the last core in asks the firmware for SICD
 * instead of its own C2: the same PSCI CPU_SUSPEND (SMC 0xC4000001) with
 * Samsung's psci_power_state_pack(1, 0, 1) = 0x01000001 (`sicd_state`), and the APM then runs its
 * sequencers (soc/mif_sequencer_sicd_down: PLLs off, the memory interface
 * down). What the kernel writes around the call:
 *
 *   before  WAKEUP_INT_EN  PMU 0x3944 = 0x7ff81fff  (stock SICD column)
 *           WAKEUP2_INT_EN PMU 0x3964 = 0 (stock for SICD; the DT column says 0xf)
 *           IDLE_IP_MASK0..3 PMU 0x3f0.. (see `idle_mask` below)
 *           CP_INT_EN      PMU 0x3044 bit 6 set     (enter_sicd: SET_BIT_ATOMIC
 *                                                     0x6, a bit number)
 *           CPU_INFORM[core] = 3 (SICD)
 *   after   CP_INT_EN bit 6 clear, WAKEUP(2)_INT_EN = 0, pending GRP26 bit 6,
 *           GRP27, GRP31 cleared (BID 0x15870000 + 0x1a08/0x1b08/0x1f08);
 *           on an early wakeup (the call returned) also SYSTEM_CTRL 0x3a10
 *           bit 14 clear and GRP4 bit 0 cleared.
 *
 * The stock kernel also waits for its IDLE_IP blocks to report idle; that
 * check is left out. It does write IDLE_IP_MASK0..3 (PMU 0x3f0..0x3fc, set
 * bit = the APM ignores that block) before the call and 0 after, and the
 * firmware refused every trial without it (11.11: SMC back at once, x0
 * unchanged, no wake status). `idle_mask` picks what goes there for the
 * call (the previous values come back afterwards): 0 leave it, 1 the stock
 * set (every idle-ip except pd-aud, pd-dpu, pd-vts, pd-fsys0a and
 * 99..110), 2 everything (upstream's relax=1). `last_idle_ip` shows the
 * IDLE_IP status words and masks seen at the last attempt. For SICD the
 * stock kernel leaves WAKEUP2_INT_EN at 0 (exynos_set_wakeupmask), not the
 * DT column's 0xf. The EINT wake mask is still not touched.
 *
 * Off by default: `sicd` (0644) turns it on and off at run time. For a first
 * trial `tries` (0644) gives the number of attempts allowed, after which it
 * switches itself off again (0 = no limit). Counters: attempts, entries,
 * early (aborted calls), last_sleep_us, max_sleep_us (from the MCT, which
 * runs off the oscillator), last_block, and the PMU values found at load.
 *
 * Added for OmonullOS, November 2026.
 */
#include <linux/cpuidle.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/arm-smccc.h>

#include "../zrn-idle.h"
#include "zrn-sicd-bx.h"

#define PMU_CPU_INFORM(c)	(0x0860 + 4 * (c))
#define PMU_CP_INT_EN		0x3044
#define CP_INT_EN_SICD		BIT(6)
#define PMU_WAKEUP_INT_EN	0x3944
#define PMU_WAKEUP_STAT		0x3950
#define PMU_WAKEUP2_STAT	0x3954
#define PMU_WAKEUP2_INT_EN	0x3964
#define PMU_SYSTEM_CTRL		0x3a10
#define SYSTEM_CTRL_EARLY	BIT(14)
#define WAKEUP_SICD		0x7ff81fff
#define WAKEUP2_SICD		0x0	/* exynos_set_wakeupmask: 0 for SICD */
#define PMU_IDLE_IP(n)		(0x03e0 + 4 * (n))
#define PMU_IDLE_IP_MASK(n)	(0x03f0 + 4 * (n))
#define PMU_EINT_WAKEUP_MASK	0x060c
#define PMU_EINT_WAKEUP_MASK2	0x061c
#define INFORM_SICD		3

#define BID_BASE		0x15870000
#define GRP_PEND(n)		(0x0008 + 0x100 * (n))
#define GRP_CLEAR(n)		(0x000c + 0x100 * (n))

/*
 * The power-state word. Stock exynos_cpu_pm_enter() ORs in every mode it
 * enters: whenever SICD (psci-index 256, PSCI_SYSTEM_IDLE) is allowed,
 * cpd_cl0 (128, PSCI_CLUSTER_SLEEP, siblings 0-5) is too and comes first,
 * so psci_suspend_customized_finisher() packs id 1 with affinity level 1:
 * 0x01000001. 0x00000001 (id 1 alone) was refused on every trial (11.11).
 */
#define PSCI_SYSTEM_IDLE_STATE	0x01000001u

static bool sicd;
static int sicd_set(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops sicd_ops = {
	.set = sicd_set,
	.get = param_get_bool,
};
module_param_cb(sicd, &sicd_ops, &sicd, 0644);
MODULE_PARM_DESC(sicd, "offer SICD when every online core is in C2 (off by default; with disp_guard only while the screen is off)");
static bool s2idle_sicd;
module_param(s2idle_sicd, bool, 0644);
MODULE_PARM_DESC(s2idle_sicd, "offer SICD from suspend-to-idle too");
static unsigned int residency = 10000;
module_param(residency, uint, 0644);
MODULE_PARM_DESC(residency, "least us to the earliest timer before SICD (stock 10000)");
static unsigned long tries;
module_param(tries, ulong, 0644);
MODULE_PARM_DESC(tries, "attempts left before sicd switches itself off (0: no limit)");

static unsigned long attempts, entries, early, blocked_short, blocked_cpu;
module_param(attempts, ulong, 0444);
module_param(entries, ulong, 0444);
module_param(early, ulong, 0444);
module_param(blocked_short, ulong, 0444);
module_param(blocked_cpu, ulong, 0444);
static int last_error, last_core = -1;
module_param(last_error, int, 0444);
module_param(last_core, int, 0444);
static unsigned long last_sleep_us, max_sleep_us;
module_param(last_sleep_us, ulong, 0444);
module_param(max_sleep_us, ulong, 0444);
static unsigned int last_wake_stat, last_wake2_stat;
/* read before the clears: did the PMU see an early wakeup (SYSTEM_CTRL
 * bit 14, BID GRP4 bit 0), i.e. did the APM start SICD at all? */
static unsigned int last_sys_ctrl, last_grp4_pend, last_grp26_pend, early_hw;
module_param(last_sys_ctrl, uint, 0444);
module_param(last_grp4_pend, uint, 0444);
module_param(last_grp26_pend, uint, 0444);
module_param(early_hw, uint, 0444);
module_param(last_wake_stat, uint, 0444);
module_param(last_wake2_stat, uint, 0444);
static char last_block[48];
module_param_string(last_block, last_block, sizeof(last_block), 0444);
/* exynos9820.dts idle-ip-mask: 0..73, 75..77, 79..87, 89..93, 95..98, 111 */
static const u32 stock_idle_ip_mask[4] = { 0xffffffff, 0xffffffff, 0xbeffbbff, 0x00008007 };
static unsigned int idle_mask = 1;
module_param(idle_mask, uint, 0644);
MODULE_PARM_DESC(idle_mask, "IDLE_IP_MASK for the call: 0 leave, 1 stock (default), 2 all");
static unsigned int sicd_state = PSCI_SYSTEM_IDLE_STATE;
module_param(sicd_state, uint, 0644);
MODULE_PARM_DESC(sicd_state, "PSCI power-state word for SICD (stock 0x01000001; 0x1 was refused)");
static char last_idle_ip[128];
module_param_string(last_idle_ip, last_idle_ip, sizeof(last_idle_ip), 0444);
static char at_load[240];
module_param_string(at_load, at_load, sizeof(at_load), 0444);

static void __iomem *bid2;

/*
 * Display guard. The panel is in command mode: bx-decon flips a buffer by
 * programming IDMA_IN_BASE_ADDR_Y and a shadow update, and DECON fetches it
 * from DRAM at the panel's next TE. SICD with the screen coming back on hung
 * the phone (12.11 09:39: the screen-on came 60 ms after the last good SICD
 * counter line). bx-decon lives in the initramfs and tells nobody, so this
 * reads its registers: when sicd is switched on (the screen off, the black
 * frame programmed) the scanned-out address is noted, and SICD is refused,
 * and switched off, as soon as that address changes or a shadow update is
 * pending, i.e. from bx-decon's first register write of a screen-on.
 * Only DECON and the one live IDMA channel are read, as bx-decon's probe
 * does (reading an idle DPU block can wedge the SoC). Whoever turns sicd on
 * does so with the screen off; zgov turns it on again after each screen-off.
 */
#define DECON_PHYS		0x19030000
#define DPU_DMA_PHYS		0x19070000
#define DECON_SHADOW_REQ	0x0060
#define DECON_PATH0		0x0214
#define DECON_PATH1		0x0218
#define DECON_WIN_START(w)	(0x1008 + (w) * 0x30)
#define DECON_WIN_END(w)	(0x100c + (w) * 0x30)
#define DECON_FULL_END		((3040 - 1) << 16 | (1440 - 1))
#define IDMA_BASE_Y		0x0040
static const u32 idma_offset[6] = { 0x1000, 0x6000, 0x2000, 0x5000, 0x3000, 0x4000 };

static bool disp_guard = true;
module_param(disp_guard, bool, 0644);
MODULE_PARM_DESC(disp_guard, "refuse SICD and switch it off once the display scans out a new buffer (default on)");
static unsigned long disp_blocked;
module_param(disp_blocked, ulong, 0444);
static unsigned int disp_base;
module_param(disp_base, uint, 0444);
static int disp_win = -1;
module_param(disp_win, int, 0444);
static void __iomem *decon, *idma;	/* idma: the live channel's page */

static bool disp_busy(void)
{
	return !idma ||
	       (readl_relaxed(decon + DECON_SHADOW_REQ) & BIT(disp_win)) ||
	       readl_relaxed(idma + IDMA_BASE_Y) != disp_base;
}

static int sicd_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	if (on && READ_ONCE(disp_guard)) {
		if (!idma)
			return -ENODEV;
		if (readl_relaxed(decon + DECON_SHADOW_REQ) & BIT(disp_win))
			return -EBUSY;	/* a flip in flight: the screen is not off */
		WRITE_ONCE(disp_base, readl_relaxed(idma + IDMA_BASE_Y));
	}
	WRITE_ONCE(sicd, on);
	return 0;
}

static void disp_init(void)
{
	u32 path0, path1, end;
	int w;

	decon = ioremap(DECON_PHYS, 0x2000);
	if (!decon)
		return;
	path0 = readl_relaxed(decon + DECON_PATH0);
	path1 = readl_relaxed(decon + DECON_PATH1);
	for (w = 0; w < 6; w++) {
		if (!(path0 & BIT(4 * w)))
			continue;
		end = readl_relaxed(decon + DECON_WIN_END(w));
		if (!readl_relaxed(decon + DECON_WIN_START(w)) && end == DECON_FULL_END &&
		    ((path1 >> (4 * w)) & 0x7) < 6)
			disp_win = w;
	}
	if (disp_win < 0)
		return;
	idma = ioremap(DPU_DMA_PHYS + idma_offset[(path1 >> (4 * disp_win)) & 0x7], 0x100);
}

static ktime_t deadline[8];	/* each core's next timer as it went down */
static bool sicd_active;	/* under zrn_c2_lock */

static void blocked(unsigned long *counter, const char *fmt, ...)
{
	va_list ap;

	if (counter)
		(*counter)++;
	va_start(ap, fmt);
	vsnprintf(last_block, sizeof(last_block), fmt, ap);
	va_end(ap);
}

/* pmucal CLEAR_PEND: write back to the clear register what is pending */
static void clear_pend(unsigned int grp, u32 mask)
{
	u32 p = readl_relaxed(bid2 + GRP_PEND(grp)) & mask;

	if (p)
		writel_relaxed(p, bid2 + GRP_CLEAR(grp));
}

static void rmw(void __iomem *reg, u32 mask, u32 val)
{
	writel_relaxed((readl_relaxed(reg) & ~mask) | val, reg);
	readl_relaxed(reg);
}

static int zrn_sicd_finisher(unsigned long state)
{
	struct arm_smccc_res res;

	arm_smccc_smc(0xC4000001, state, zrn_cpu_resume_pa(), 0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

/* enter_powerdown_common(), before marking the core down */
void zrn_sicd_note(unsigned int core, ktime_t next)
{
	if (core < 8)
		WRITE_ONCE(deadline[core], next);
}

/*
 * CPD, cluster power down: exynos9820.dts cpupm cpd_cl0 (cores 0-5, the
 * A55s and A75s behind one DSU) and cpd_cl1 (6-7, the M4s), psci-index 128,
 * residency 10000 us. The kernel's whole share (pmucal cluster_disable:
 * these clusters have no kernel-side off sequence) is CPU_INFORM = 2 for
 * the group's last core and PSCI affinity level 1, 0x01000000; the APM's
 * own flexpmu copy (CPUCL0_HCHGEN_CLKMUX, cluster0_noncpu_down) is not
 * ours to run. On the way up cluster2_on rewrites SYSREG_CPUCL2
 * BUS_COMPONENT_DRCG_EN (0x1d100104) to ~0: it goes with the M4 cluster's
 * power. Stock enters cpd_cl1 before SICD, cpd_cl0 with it (0x01000001).
 *
 * `cpd` (0644, off by default) is a mask: 1 cores 0-5, 2 cores 6-7.
 * `cpd_tries` as `tries`. `sicd_need_cpd` (default on) lets SICD wait for
 * the M4 cluster to be down in CPD, as it always is in stock first.
 */
#define INFORM_CPD		2
#define PSCI_CLUSTER_STATE	0x01000000u
#define SYSREG_CPUCL2		0x1d100000
#define DRCG_EN			0x0104

static const unsigned long cpd_group[2] = { 0x3f, 0xc0 };
static unsigned int cpd;
module_param(cpd, uint, 0644);
MODULE_PARM_DESC(cpd, "cluster power down: mask, 1 cores 0-5, 2 cores 6-7 (off by default)");
static bool s2idle_cpd;
module_param(s2idle_cpd, bool, 0644);
MODULE_PARM_DESC(s2idle_cpd, "cluster power down for both groups from suspend-to-idle too");
static unsigned int cpd_residency = 10000;
module_param(cpd_residency, uint, 0644);
MODULE_PARM_DESC(cpd_residency, "least us to the group's earliest timer (stock 10000)");
static unsigned long cpd_tries;
module_param(cpd_tries, ulong, 0644);
MODULE_PARM_DESC(cpd_tries, "attempts left before cpd switches itself off (0: no limit)");
static bool sicd_need_cpd = true;
module_param(sicd_need_cpd, bool, 0644);
MODULE_PARM_DESC(sicd_need_cpd, "SICD only while cores 6-7 are down in CPD");
/* per group: asked, came back through cpu_resume, call returned, a sibling woke first */
static unsigned long cpd_attempts[2], cpd_entries[2], cpd_early[2], cpd_sibling_up[2];
module_param_array(cpd_attempts, ulong, NULL, 0444);
module_param_array(cpd_entries, ulong, NULL, 0444);
module_param_array(cpd_early, ulong, NULL, 0444);
module_param_array(cpd_sibling_up, ulong, NULL, 0444);
static int cpd_last_error;
module_param(cpd_last_error, int, 0444);
static unsigned int drcg_at_load, sicd_blocked_cpd;
module_param(drcg_at_load, uint, 0444);
module_param(sicd_blocked_cpd, uint, 0444);

static void __iomem *sysreg_cl2;
static int cpd_owner[2] = { -1, -1 };	/* under zrn_c2_lock */

static unsigned int cpd_g(unsigned int core)
{
	return core < 6 ? 0 : 1;
}

/* enter_powerdown_common(), core marked down: the state word, 0 for plain C2 */
unsigned int zrn_cpd_try(unsigned int core, bool suspended)
{
	void __iomem *pmu = zrn_pmu_base();
	unsigned int g = cpd_g(core), state = 0, mask = READ_ONCE(cpd);
	ktime_t first = KTIME_MAX, now;
	unsigned long flags;
	int cpu;

	if (core > 7 || !pmu || !(mask & BIT(g) || (suspended && READ_ONCE(s2idle_cpd))))
		return 0;
	if (g == 1 && !sysreg_cl2)
		return 0;
	now = ktime_get();
	raw_spin_lock_irqsave(&zrn_c2_lock, flags);
	if (cpd_owner[g] >= 0)
		goto out;
	for_each_online_cpu(cpu) {
		if (cpu > 7)
			goto out;
		if (!(cpd_group[g] & BIT(cpu)))
			continue;
		if (!test_bit(cpu, zrn_c2_mask))
			goto out;
		if (deadline[cpu] && deadline[cpu] < first)
			first = deadline[cpu];
	}
	if (!suspended && (first == KTIME_MAX ||
			   ktime_sub(first, now) < (s64)READ_ONCE(cpd_residency) * NSEC_PER_USEC))
		goto out;
	if (READ_ONCE(cpd_tries)) {
		if (--cpd_tries == 0)
			WRITE_ONCE(cpd, 0);	/* this one is the last */
	}
	cpd_owner[g] = core;
	cpd_attempts[g]++;
	writel_relaxed(INFORM_CPD, pmu + PMU_CPU_INFORM(core));
	readl_relaxed(pmu + PMU_CPU_INFORM(core));
	state = PSCI_CLUSTER_STATE;
out:
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
	return state;
}

/* every core of the group, back from its call: the first one up undoes it */
void zrn_cpd_done(unsigned int core, unsigned int state, int ret)
{
	unsigned int g = cpd_g(core);
	unsigned long flags;

	if (core > 7)
		return;
	raw_spin_lock_irqsave(&zrn_c2_lock, flags);
	if (cpd_owner[g] >= 0) {
		if (state && cpd_owner[g] == (int)core) {
			cpd_last_error = ret;
			if (ret)
				cpd_early[g]++;
			else
				cpd_entries[g]++;
		} else {
			cpd_sibling_up[g]++;
		}
		cpd_owner[g] = -1;
		if (g == 1 && sysreg_cl2) {	/* cluster2_on */
			writel_relaxed(~0u, sysreg_cl2 + DRCG_EN);
			readl_relaxed(sysreg_cl2 + DRCG_EN);
		}
	}
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
}

/* cluster2_on for CPU hotplug (zrn-idle-bx.c): the M4 pair is back from off */
void zrn_cl2_drcg_on(void)
{
	if (!sysreg_cl2)
		return;
	writel_relaxed(~0u, sysreg_cl2 + DRCG_EN);
	readl_relaxed(sysreg_cl2 + DRCG_EN);
}

bool zrn_sicd_try(struct cpuidle_device *dev, unsigned int core, bool suspended, int *ret)
{
	void __iomem *pmu = zrn_pmu_base();
	unsigned long flags;
	ktime_t first = KTIME_MAX, now;
	u64 before, after;
	u32 ip[4], saved_mask[4];
	unsigned int mask_mode, state;
	int cpu, n;

	*ret = 0;
	if (suspended ? !READ_ONCE(s2idle_sicd) : !READ_ONCE(sicd))
		return false;
	if (core > 5 || !bid2)		/* entry-allowed 0-5 */
		return false;

	raw_spin_lock_irqsave(&zrn_c2_lock, flags);
	if (sicd_active)
		goto no;
	for_each_online_cpu(cpu) {
		if (cpu > 7 || !test_bit(cpu, zrn_c2_mask)) {
			blocked(&blocked_cpu, "cpu%d up", cpu);
			goto no;
		}
		if (deadline[cpu] && deadline[cpu] < first)
			first = deadline[cpu];
	}
	if (READ_ONCE(disp_guard) && disp_busy()) {
		WRITE_ONCE(sicd, false);
		WRITE_ONCE(s2idle_sicd, false);
		blocked(&disp_blocked, "display active");
		goto no;
	}
	if (READ_ONCE(sicd_need_cpd) && cpd_owner[1] < 0 &&
	    (cpu_online(6) || cpu_online(7))) {
		sicd_blocked_cpd++;
		blocked(NULL, "cores 6-7 not in CPD");
		goto no;
	}
	now = ktime_get();
	if (!suspended && (first == KTIME_MAX ||
			   ktime_sub(first, now) < (s64)READ_ONCE(residency) * NSEC_PER_USEC)) {
		blocked(&blocked_short, "short %lld us",
			first == KTIME_MAX ? -1LL : ktime_to_us(ktime_sub(first, now)));
		goto no;
	}
	if (READ_ONCE(tries)) {
		if (--tries == 0)
			WRITE_ONCE(sicd, false);	/* this one is the last */
	}
	sicd_active = true;
	attempts++;
	last_core = core;
	writel_relaxed(0, pmu + PMU_WAKEUP_STAT);
	writel_relaxed(0, pmu + PMU_WAKEUP2_STAT);
	writel_relaxed(WAKEUP_SICD, pmu + PMU_WAKEUP_INT_EN);
	writel_relaxed(WAKEUP2_SICD, pmu + PMU_WAKEUP2_INT_EN);
	rmw(pmu + PMU_CP_INT_EN, CP_INT_EN_SICD, CP_INT_EN_SICD);
	mask_mode = READ_ONCE(idle_mask);
	for (n = 0; n < 4; n++) {
		ip[n] = readl_relaxed(pmu + PMU_IDLE_IP(n));
		saved_mask[n] = readl_relaxed(pmu + PMU_IDLE_IP_MASK(n));
		if (mask_mode == 1)
			writel_relaxed(stock_idle_ip_mask[n], pmu + PMU_IDLE_IP_MASK(n));
		else if (mask_mode == 2)
			writel_relaxed(~0u, pmu + PMU_IDLE_IP_MASK(n));
	}
	writel_relaxed(INFORM_SICD, pmu + PMU_CPU_INFORM(core));
	readl_relaxed(pmu + PMU_CPU_INFORM(core));
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
	wmb();

	before = zrn_mct_count_now();
	state = READ_ONCE(sicd_state);
	*ret = zrn_cpu_suspend_call(state, zrn_sicd_finisher);
	after = zrn_mct_count_now();

	raw_spin_lock_irqsave(&zrn_c2_lock, flags);
	last_error = *ret;
	last_wake_stat = readl_relaxed(pmu + PMU_WAKEUP_STAT);
	last_wake2_stat = readl_relaxed(pmu + PMU_WAKEUP2_STAT);
	last_sys_ctrl = readl_relaxed(pmu + PMU_SYSTEM_CTRL);
	last_grp4_pend = readl_relaxed(bid2 + GRP_PEND(4));
	last_grp26_pend = readl_relaxed(bid2 + GRP_PEND(26));
	if (last_sys_ctrl & SYSTEM_CTRL_EARLY)
		early_hw++;
	/* exit_sicd, and on an early wakeup earlywkup_sicd's two more */
	rmw(pmu + PMU_CP_INT_EN, CP_INT_EN_SICD, 0);
	writel_relaxed(0, pmu + PMU_WAKEUP_INT_EN);
	writel_relaxed(0, pmu + PMU_WAKEUP2_INT_EN);
	clear_pend(26, BIT(6));
	clear_pend(27, ~0u);
	clear_pend(31, ~0u);
	if (mask_mode == 1 || mask_mode == 2)
		for (n = 0; n < 4; n++)
			writel_relaxed(saved_mask[n], pmu + PMU_IDLE_IP_MASK(n));
	snprintf(last_idle_ip, sizeof(last_idle_ip),
		 "ip %#x %#x %#x %#x mask(before) %#x %#x %#x %#x mode %u state %#x cpd %d/%d",
		 ip[0], ip[1], ip[2], ip[3], saved_mask[0], saved_mask[1],
		 saved_mask[2], saved_mask[3], mask_mode, state, cpd_owner[0], cpd_owner[1]);
	if (*ret) {
		early++;
		rmw(pmu + PMU_SYSTEM_CTRL, SYSTEM_CTRL_EARLY, 0);
		clear_pend(4, BIT(0));
	} else {
		entries++;
	}
	if (zrn_mct_rate_now()) {
		last_sleep_us = div64_u64((after - before) * 1000000ull, zrn_mct_rate_now());
		if (!*ret && last_sleep_us > max_sleep_us)
			max_sleep_us = last_sleep_us;
	}
	wmb();
	sicd_active = false;
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
	return true;
no:
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
	return false;
}

/*
 * lpm: the always-on part of stock's pmucal_lpm_init (cal-if/exynos9820/
 * flexpmu_cal_system_exynos9820.h), which the Samsung kernel writes once at
 * boot and mainline never does (seen: SYSREG_CPUCL2 DRCG_EN 0 at load,
 * stock ~0). Blocks behind power domains (G3D, AUD, DPU, ISP, NPU, IVA,
 * DSP, MFC, G2D, VTS, VRA2, FSYS) are left out: zpdoff may have them off.
 *
 *   1  PMU BURNIN_CTRL; 16 TCXO_DURATION3; 32 EXT_REGULATOR_MAIN/SUB/G3D_
 *      DURATION; 64 EXT_REGULATOR_CPUCL2_DURATION (1+16+32+64 = the old 1:
 *      with CPD of cores 6-7 it hung the phone, 12.11)
 *   2  SYSREG BUS_COMPONENT_DRCG_EN + MEMCLK (APM, BUSC, CMGP, CORE,
 *      CPUCL0, MIF0-3, PERIC0/1, PERIS; CPUCL2's goes with CPD)
 *   4  CMU hardware clock gating: HCHGEN_CLKMUX, CLKDIV2PH, SHORTSTOP,
 *      the CPUCL0/CPUCL2 PPC/PPMU Q-channels
 *   8  EARLY_WAKEUP_DPU_CTRL/DEST (CMU_TOP)
 *
 * `lpm_peek` (write a mask) only reads them into `lpm_log`; `lpm` (write a
 * mask, 0644, nothing by default) writes the groups not written yet, old and
 * new values into `lpm_log`. Under zrn_c2_lock and never while cores 6-7
 * are in CPD (CMU_CPUCL2). A reboot undoes it all.
 */
struct lpm_reg {
	u32 pa;
	u16 off;
	u8 grp;
	u32 mask, val;
};
#define LR(g, pa, off, m, v)	{ pa, off, g, m, v }
#define DRCG(pa)		LR(2, pa, 0x0104, ~0u, ~0u)
#define MEMCLK(pa)		LR(2, pa, 0x0108, 0x1, 0x0)
static const struct lpm_reg lpm_regs[] = {
	LR(1, 0x15860000, 0x0a08, 0x3, 0x2),		/* BURNIN_CTRL */
	LR(1, 0x15860000, 0x0a08, 0xff << 16, 0x0),
	LR(32, 0x15860000, 0x0e00, ~0u, 0x7d0),		/* EXT_REGULATOR_MAIN_DURATION */
	LR(32, 0x15860000, 0x0e04, ~0u, 0x528),		/* _SUB_ */
	LR(64, 0x15860000, 0x0e08, ~0u, 0x8c),		/* _CPUCL2_ */
	LR(32, 0x15860000, 0x0e0c, ~0u, 0x0),		/* _G3D_ */
	LR(16, 0x15860000, 0x0284, ~0u, 0x66c),		/* TCXO_DURATION3 */
	DRCG(0x15820000), DRCG(0x1a220000), DRCG(0x15c20000), DRCG(0x1a020000),
	DRCG(0x1d000000), LR(2, 0x1bc20000, 0x0104, ~0u, 0x7fffffff),
	DRCG(0x1bd20000), DRCG(0x1be20000), DRCG(0x1bf20000),
	DRCG(0x10420000), DRCG(0x10820000), DRCG(0x10030000),
	MEMCLK(0x15820000), MEMCLK(0x1a220000), MEMCLK(0x1a020000), MEMCLK(0x1d000000),
	MEMCLK(0x10420000), MEMCLK(0x10820000), MEMCLK(0x10030000),
	LR(4, 0x1a240000, 0x0850, 0x1, 0x1),		/* CMU_HCHGEN_CLKMUX */
	LR(4, 0x1a030000, 0x0850, 0x1, 0x1),		/* CORE_ */
	LR(4, 0x1d020000, 0x0850, 0x7f, 0x01),		/* CPUCL0_ */
	LR(4, 0x1d020000, 0x0840, 0x3fffff, 0x3ff000),	/* CPUCL0_CLKDIV2PH */
	LR(4, 0x1d030000, 0x0840, 0x7f, 0x01),		/* CPUCL1_HCHGEN_CLKMUX */
	LR(4, 0x1d030000, 0x0830, 0x3fffff, 0x3ff181),	/* CPUCL1_CLKDIV2PH */
	LR(4, 0x1d120000, 0x0838, 0x3fffff, 0x3ff181),	/* CPUCL2_CTRL_EXT_EVENT */
	LR(4, 0x1bc00000, 0x0850, 0x1, 0x1), LR(4, 0x1bd00000, 0x0850, 0x1, 0x1),
	LR(4, 0x1be00000, 0x0850, 0x1, 0x1), LR(4, 0x1bf00000, 0x0850, 0x1, 0x1),
	LR(4, 0x10020000, 0x0850, 0x1f, 0x0),		/* PERIS_HCHGEN_CLKMUX */
	LR(4, 0x1d020000, 0x0834, ~0u, 0x1),		/* CPUCL0_SHORTSTOP_DBG */
	LR(4, 0x1d030000, 0x0820, ~0u, ~0u),		/* CPUCL1_SHORTSTOP */
	LR(4, 0x1d020000, 0x0820, ~0u, ~0u),		/* CPUCL0_SHORTSTOP */
	LR(4, 0x1a030000, 0x0820, ~0u, 0x1),		/* CORE_SHORTSTOP */
	LR(4, 0x1d020000, 0x30b4, 0x7, 0x6),		/* QCH_CON_CPUCL0_QCH_SCLK */
	LR(4, 0x1a030000, 0x30b4, 0x7, 0x6), LR(4, 0x1a030000, 0x30b8, 0x7, 0x6),
	LR(4, 0x1a030000, 0x30bc, 0x7, 0x6), LR(4, 0x1a030000, 0x30c0, 0x7, 0x6),
	LR(4, 0x1a030000, 0x30e0, 0x7, 0x6), LR(4, 0x1a030000, 0x30e4, 0x7, 0x6),
	LR(4, 0x1a030000, 0x30e8, 0x7, 0x6), LR(4, 0x1a030000, 0x30ec, 0x7, 0x6),
	LR(8, 0x1a240000, 0x0884, 0x1, 0x1),		/* EARLY_WAKEUP_DPU_CTRL */
	LR(8, 0x1a240000, 0x08a0, ~0u, 0x0f001048),	/* EARLY_WAKEUP_DPU_DEST */
};
static unsigned int lpm_done;
static char lpm_log[3600];

/* write: 0 only reads (lpm_peek); returns the groups done, or -errno */
static int lpm_run(unsigned int mask, bool write)
{
	void __iomem *va[ARRAY_SIZE(lpm_regs)] = { };
	unsigned long flags;
	size_t n = 0;
	int i, ret = 0;

	mask &= 0x7f;
	if (write)
		mask &= ~lpm_done;
	for (i = 0; i < ARRAY_SIZE(lpm_regs); i++) {
		if (!(lpm_regs[i].grp & mask))
			continue;
		va[i] = ioremap(lpm_regs[i].pa + lpm_regs[i].off, 4);
		if (!va[i]) {
			ret = -ENOMEM;
			goto unmap;
		}
	}
	raw_spin_lock_irqsave(&zrn_c2_lock, flags);
	if ((mask & 4) && (cpd_owner[1] >= 0 || READ_ONCE(cpd))) {
		raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
		ret = -EBUSY;	/* CMU_CPUCL2 may be down */
		goto unmap;
	}
	lpm_log[0] = 0;
	for (i = 0; i < ARRAY_SIZE(lpm_regs); i++) {
		const struct lpm_reg *r = &lpm_regs[i];
		u32 old, new;

		if (!va[i])
			continue;
		old = readl_relaxed(va[i]);
		new = old;
		if (write) {
			new = (old & ~r->mask) | (r->val & r->mask);
			writel_relaxed(new, va[i]);
			new = readl_relaxed(va[i]);
		}
		n += scnprintf(lpm_log + n, sizeof(lpm_log) - n, "%d %#x %#x>%#x\n",
			       r->grp, r->pa + r->off, old, new);
	}
	if (write)
		lpm_done |= mask;
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
	ret = mask;
unmap:
	for (i = 0; i < ARRAY_SIZE(lpm_regs); i++)
		if (va[i])
			iounmap(va[i]);
	return ret;
}

static int lpm_set(const char *val, bool write)
{
	unsigned int mask;
	int ret = kstrtouint(val, 0, &mask);

	if (ret)
		return ret;
	ret = lpm_run(mask, write);
	if (ret < 0)
		return ret;
	pr_info("zrn-sicd: lpm %s %#x\n", write ? "wrote" : "read", ret);
	return 0;
}
static int lpm_set_w(const char *val, const struct kernel_param *kp)
{
	return lpm_set(val, true);
}
static int lpm_set_r(const char *val, const struct kernel_param *kp)
{
	return lpm_set(val, false);
}
static int lpm_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n", lpm_done);
}
static int lpm_get_log(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%s", lpm_log);
}
static const struct kernel_param_ops lpm_ops = { .set = lpm_set_w, .get = lpm_get };
static const struct kernel_param_ops lpm_peek_ops = { .set = lpm_set_r, .get = lpm_get };
static const struct kernel_param_ops lpm_log_ops = { .get = lpm_get_log };
module_param_cb(lpm, &lpm_ops, NULL, 0644);
MODULE_PARM_DESC(lpm, "write stock lpm_init groups (mask 1 BURNIN, 2 SYSREG, 4 CMU gating, 8 DPU early wakeup, 16 TCXO, 32 EXT_REG, 64 EXT_REG_CPUCL2)");
module_param_cb(lpm_peek, &lpm_peek_ops, NULL, 0644);
MODULE_PARM_DESC(lpm_peek, "read the lpm_init registers of a group mask into lpm_log");
module_param_cb(lpm_log, &lpm_log_ops, NULL, 0444);

int zrn_sicd_init(void)
{
	void __iomem *pmu = zrn_pmu_base();

	bid2 = ioremap(BID_BASE, 0x2000);
	if (!bid2 || !pmu)
		return -ENOMEM;
	disp_init();
	sysreg_cl2 = ioremap(SYSREG_CPUCL2, 0x1000);
	if (sysreg_cl2)		/* a read: the M4 cluster is up, nothing asked CPD yet */
		drcg_at_load = readl_relaxed(sysreg_cl2 + DRCG_EN);
	/* reads only: what the PMU holds before anything here writes */
	snprintf(at_load, sizeof(at_load),
		 "cp_int_en %#x wake %#x wake2 %#x sysctrl %#x stat %#x/%#x"
		 " ip %#x %#x %#x %#x ipmask %#x %#x %#x %#x eint %#x %#x",
		 readl_relaxed(pmu + PMU_CP_INT_EN), readl_relaxed(pmu + PMU_WAKEUP_INT_EN),
		 readl_relaxed(pmu + PMU_WAKEUP2_INT_EN), readl_relaxed(pmu + PMU_SYSTEM_CTRL),
		 readl_relaxed(pmu + PMU_WAKEUP_STAT), readl_relaxed(pmu + PMU_WAKEUP2_STAT),
		 readl_relaxed(pmu + PMU_IDLE_IP(0)), readl_relaxed(pmu + PMU_IDLE_IP(1)),
		 readl_relaxed(pmu + PMU_IDLE_IP(2)), readl_relaxed(pmu + PMU_IDLE_IP(3)),
		 readl_relaxed(pmu + PMU_IDLE_IP_MASK(0)), readl_relaxed(pmu + PMU_IDLE_IP_MASK(1)),
		 readl_relaxed(pmu + PMU_IDLE_IP_MASK(2)), readl_relaxed(pmu + PMU_IDLE_IP_MASK(3)),
		 readl_relaxed(pmu + PMU_EINT_WAKEUP_MASK), readl_relaxed(pmu + PMU_EINT_WAKEUP_MASK2));
	pr_info("zrn-sicd: off until sicd=1 (cpd: cpd=1/2/3); PMU at load: %s; DRCG_EN %#x; display window %d%s\n",
		at_load, drcg_at_load, disp_win, idma ? "" : " (no guard: SICD stays off)");
	return 0;
}

void zrn_sicd_exit(void)
{
	WRITE_ONCE(sicd, false);
	WRITE_ONCE(s2idle_sicd, false);
	WRITE_ONCE(cpd, 0);
	WRITE_ONCE(s2idle_cpd, false);
	if (sysreg_cl2)
		iounmap(sysreg_cl2);
	sysreg_cl2 = NULL;
	if (idma)
		iounmap(idma);
	idma = NULL;
	if (decon)
		iounmap(decon);
	decon = NULL;
	if (bid2)
		iounmap(bid2);
	bid2 = NULL;
}
