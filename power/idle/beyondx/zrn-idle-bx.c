// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-idle-bx: upstream ztOS's zrn-idle (power/idle/zrn-idle.c) for the
 * Exynos 9820 (Galaxy S10 5G). What changed, from Samsung's G977N cal-if
 * (exynos9820/flexpmu_cal_cpu_exynos9820.h, the !ACPM_FRAMEWORK tables the
 * kernel runs, and flexpmu_cal_powermode_exynos9820.h):
 *
 *   PMU 0x15860000, CPU_INFORM 0x0860 + 4 * core; BID 0x15870000
 *   three clusters: cores 0-3 A55, 4-5 A75 (MPIDR Aff0 0..5), 6-7 M4
 *   (Aff1 1); the BID bit is the core itself; off also clears GRP1's
 *   bit core + 8; instead of the 9810's PMU_EVENT_INTERRUPT_ENABLE_GRP2,
 *   bit 3 of each core's CLUSTERx_CPUy_INT_EN
 *   latencies from the stock device tree (exit 160/190/220 us,
 *   residency 2000/2500/3500 us); MCT, its SPIs and PSCI as on the 9810
 *   SICD in zrn-sicd-bx.c (off until its sicd=1); no cluster power down
 *
 * Upstream's description follows.
 *
 * zrn-idle: CPU idle for the Exynos 9810 (Galaxy S9) under mainline.
 *
 * Without it the cores only ever pause in WFI: powered, clocked, caches on.
 * The firmware does offer per-core power-down through PSCI CPU_SUSPEND
 * (state 0x10000, as the stock device tree has it), but on this SoC the PMU
 * is helped by a small "flexible PMU" controller that has to be told first.
 * Samsung's kernel (exynos-powermode.c, cal-if pmucal_cpu.c) does, around
 * every core power-down:
 *
 *   before   CPU_INFORM[core] = C2 (1)            PMU 0x14060860 + 4 * core
 *            GRP2_INTR_BID_ENABLE |= bit           0x14070200
 *            if GRP1 pending, GRP1_INTR_BID_CLEAR  0x14070108 / 0x1407010c
 *            PMU_EVENT_INTERRUPT_ENABLE_GRP2 |= bit  PMU 0x14067f08
 *   after    CPU_INFORM[core] = 0, the three again with the bit cleared
 *            (GRP2 pending cleared instead: 0x14070208 / 0x1407020c)
 *
 * where core is cluster * 4 + cpu (little cluster 0; see zrn_core()) and bit is core's
 * interrupt line into the controller: 4..7 for the little cores, 0..3 for
 * the big ones. Without that, a core that powers down is not woken again,
 * which is also why an offlined core never comes back on this kernel.
 *
 * This registers a cpuidle driver per cluster: WFI, and the power-down state
 * with the stock latencies. psci_cpu_suspend_enter() is not exported, so its
 * address is passed in from /proc/kallsyms:
 *
 *   insmod zrn-cpuidle.ko suspend_enter=0x$(awk '$3=="psci_cpu_suspend_enter"{print $1}' /proc/kallsyms) cpus=0xff
 *
 * cpus is a mask of the CPUs to drive (default 0xff); the others keep WFI.
 */
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/cpu_pm.h>
#include <linux/cpuidle.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/arm-smccc.h>
#include <linux/tick.h>
#include <linux/suspend.h>
#include <linux/psci.h>
#include <uapi/linux/psci.h>
#include <linux/delay.h>
#include <asm/cputype.h>
#include <asm/memory.h>

#include "../zrn-idle.h"
#include "zrn-sicd-bx.h"

#define PMU_BASE		0x15860000
#define PMU_CPU_INFORM(c)	(0x0860 + 4 * (c))
#define BID_BASE		0x15870000
/* CLUSTERx_CPUy_INT_EN, bit 3: 1 while the core is down */
static const u16 pmu_int_en[8] = { 0x1044, 0x10c4, 0x1144, 0x11c4, 0x1344, 0x13c4, 0x1544, 0x15c4 };
#define INT_EN_BIT		BIT(3)
#define GRP1_PEND		0x0108
#define GRP1_CLEAR		0x010c
#define GRP2_ENABLE		0x0200
#define GRP2_PEND		0x0208
#define GRP2_CLEAR		0x020c
#define INFORM_C2		1
#define INFORM_CPD		2	/* cpu_inform_cpd: the cluster goes with this core */
#define PSCI_CORE_POWERDOWN	0x10000

static unsigned long suspend_fn, resume_fn;
module_param_named(suspend_enter, suspend_fn, ulong, 0444);
MODULE_PARM_DESC(suspend_enter, "address of cpu_suspend (from /proc/kallsyms)");
module_param_named(resume, resume_fn, ulong, 0444);
MODULE_PARM_DESC(resume, "address of cpu_resume (from /proc/kallsyms)");
static unsigned int cpus = 0x0c;
module_param(cpus, uint, 0444);
MODULE_PARM_DESC(cpus, "mask of the CPUs to give the power-down state");

static int (*cpu_suspend_p)(unsigned long arg, int (*fn)(unsigned long));
static unsigned long cpu_resume_pa;
static void __iomem *pmu, *bid;
/* C2 calls that came back instead of resuming, and the last x0: tells
 * whether SICD's 0xC4000001 is the firmware's ordinary "aborted" answer */
static unsigned long c2_returned;
static int c2_last_ret;
module_param(c2_returned, ulong, 0444);
module_param(c2_last_ret, int, 0444);

/*
 * The finisher: one SMC, the function id Samsung's firmware implements
 * (from the stock device tree's psci node), with the state and the physical
 * address cpu_resume runs at. It must not touch the stack: cpu_suspend has
 * already saved the register frame it will be resumed into.
 */
static int zrn_suspend_finisher(unsigned long state)
{
	struct arm_smccc_res res;

	arm_smccc_smc(0xC4000001, state, cpu_resume_pa, 0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}
static DEFINE_RAW_SPINLOCK(pmu_lock);

/*
 * The PMU's core number. The hardware MPIDRs differ from the device tree's
 * reg values: the A55s and A75s share one DSU and are Aff1 0..5 (Aff0 0),
 * the M4s are Aff1 1, Aff0 0..1 -- so cpu1 and cpu6 both read 0x100.
 * The M4s (Samsung, MIDR implementer 0x53) are PMU cores 6 and 7.
 */
static unsigned int zrn_core(u64 mpidr)
{
	if (MIDR_IMPLEMENTOR(read_cpuid_id()) == 0x53)	/* Samsung */
		return 6 + MPIDR_AFFINITY_LEVEL(mpidr, 0);
	return MPIDR_AFFINITY_LEVEL(mpidr, 1);
}

/*
 * The read back makes the write land before the lock goes: with two cores
 * in C2 at once, a core's GRP2 enable bit went missing (bidEN 0x0a -> 0x08
 * with core 1 still down and its GRP1 interrupt pending, 2026-10-18), so the
 * APM never woke it -- the next core's rmw had read the register stale.
 */
static void rmw(void __iomem *reg, u32 mask, u32 value)
{
	writel_relaxed((readl_relaxed(reg) & ~mask) | value, reg);
	readl_relaxed(reg);
}

/* GRP2 enable bits of cores still down (zrn_c2_mask) found cleared: put back */
static unsigned long bid_lost, bid_lost_bits;
module_param(bid_lost, ulong, 0444);
module_param(bid_lost_bits, ulong, 0444);

static u32 hp_down;	/* cores offline through `hotplug`, lines armed (pmu_lock) */

static void bid_guard(u32 except)
{
	u32 want = ((u32)zrn_c2_mask[0] | hp_down) & 0xff & ~except;
	u32 en = readl_relaxed(bid + GRP2_ENABLE);

	if (want & ~en) {
		bid_lost++;
		bid_lost_bits |= want & ~en;
		writel_relaxed(en | want, bid + GRP2_ENABLE);
		readl_relaxed(bid + GRP2_ENABLE);
	}
}

/* Samsung's coreNN_off: GRP2 enable, GRP1 pending (core and core + 8) cleared, INT_EN */
static void core_off(unsigned int core)
{
	u32 bit = BIT(core), bit8 = BIT(core + 8);

	if (WARN_ON_ONCE(core > 7))
		return;
	raw_spin_lock(&pmu_lock);
	writel_relaxed(INFORM_C2, pmu + PMU_CPU_INFORM(core));
	readl_relaxed(pmu + PMU_CPU_INFORM(core));
	rmw(bid + GRP2_ENABLE, bit, bit);
	if (readl_relaxed(bid + GRP1_PEND) & bit)
		writel_relaxed(bit, bid + GRP1_CLEAR);
	if (readl_relaxed(bid + GRP1_PEND) & bit8)
		writel_relaxed(bit8, bid + GRP1_CLEAR);
	rmw(pmu + pmu_int_en[core], INT_EN_BIT, INT_EN_BIT);
	bid_guard(bit);
	raw_spin_unlock(&pmu_lock);
	wmb();
}

/* coreNN_on: GRP2 enable off, GRP2 pending cleared, INT_EN off */
static void core_on(unsigned int core)
{
	u32 bit = BIT(core);

	if (core > 7)
		return;
	raw_spin_lock(&pmu_lock);
	writel_relaxed(0, pmu + PMU_CPU_INFORM(core));
	readl_relaxed(pmu + PMU_CPU_INFORM(core));
	rmw(bid + GRP2_ENABLE, bit, 0);
	if (readl_relaxed(bid + GRP2_PEND) & bit)
		writel_relaxed(bit, bid + GRP2_CLEAR);
	rmw(pmu + pmu_int_en[core], INT_EN_BIT, 0);
	bid_guard(bit);
	raw_spin_unlock(&pmu_lock);
}

static int enter_wfi(struct cpuidle_device *dev, struct cpuidle_driver *drv, int idx)
{
	dsb(sy);
	wfi();
	return idx;
}

static int enter_powerdown_common(struct cpuidle_device *dev, struct cpuidle_driver *drv,
				 int idx, bool suspended)
{
	u64 mpidr = read_cpuid_mpidr();
	unsigned int core = zrn_core(mpidr);
	unsigned int cpd;
	int ret;

	if (core > 7)	/* not a core the PMU knows: stay in WFI */
		return enter_wfi(dev, drv, 0);
	ret = cpu_pm_enter();
	if (ret)
		return -EBUSY;
	core_off(core);
	zrn_sicd_note(core, READ_ONCE(dev->next_hrtimer));
	/* in C2 from here: SICD (zrn-sicd.c) looks for every online core set */
	zrn_c2_mark(core, true);
	/* cluster power down (zrn-sicd-bx.c), when this is the group's last core */
	cpd = zrn_cpd_try(core, suspended);
	if (!zrn_sicd_try(dev, core, suspended, &ret)) {
		/*
		 * Not psci_cpu_suspend_enter(): it reads bit 30 of the state
		 * word to tell retention from power-down, and Samsung's
		 * 0x10000 has it clear, so it would pass entry point 0 -- and
		 * a core that powers down resumes through no frame at all
		 * (seen: the first IRQ lands on a corrupted stack).
		 * cpu_suspend saves the frame on this stack and our finisher
		 * hands the firmware cpu_resume's physical address, exactly
		 * what Samsung's own idle path does.
		 */
		ret = cpu_suspend_p(cpd ? cpd : PSCI_CORE_POWERDOWN,
				    zrn_suspend_finisher);
		if (ret) {
			c2_returned++;
			c2_last_ret = ret;
		}
	}
	zrn_cpd_done(core, cpd, ret);
	zrn_c2_mark(core, false);
	core_on(core);
	cpu_pm_exit();
	return ret ? -EBUSY : idx;
}

static int enter_powerdown(struct cpuidle_device *dev, struct cpuidle_driver *drv, int idx)
{
	return enter_powerdown_common(dev, drv, idx, false);
}

static int enter_s2idle(struct cpuidle_device *dev, struct cpuidle_driver *drv, int idx)
{
	/* The PM core has frozen tasks, suspended devices and stopped ticks.
	 * cpu_suspend() still owns the RCU/context-tracking transition. */
	return enter_powerdown_common(dev, drv, idx, true);
}

static struct cpuidle_driver drivers[3] = {
	{
		.name = "zrn_idle_little",
		/*
		 * No .owner: with one, each registered CPU holds the module and
		 * it can never be unloaded. cpuidle_unregister() waits for the
		 * CPUs to leave our states before the module goes.
		 */
		.states = {
			{ .name = "WFI", .desc = "wait for interrupt", .enter = enter_wfi,
			  .exit_latency = 1, .target_residency = 1 },
			{ .name = "C2", .desc = "core power down", .enter = enter_powerdown,
			  .enter_s2idle = enter_s2idle,
			  /* cpu_suspend() owns the context-tracking transition. */
			  .flags = CPUIDLE_FLAG_TIMER_STOP | CPUIDLE_FLAG_RCU_IDLE,
			  .exit_latency = 160, .target_residency = 2000 },
		},
		.state_count = 2,
	},
	{
		.name = "zrn_idle_mid",
		.states = {
			{ .name = "WFI", .desc = "wait for interrupt", .enter = enter_wfi,
			  .exit_latency = 1, .target_residency = 1 },
			{ .name = "C2", .desc = "core power down", .enter = enter_powerdown,
			  .enter_s2idle = enter_s2idle,
			  .flags = CPUIDLE_FLAG_TIMER_STOP | CPUIDLE_FLAG_RCU_IDLE,
			  .exit_latency = 190, .target_residency = 2500 },
		},
		.state_count = 2,
	},
	{
		.name = "zrn_idle_big",
		.states = {
			{ .name = "WFI", .desc = "wait for interrupt", .enter = enter_wfi,
			  .exit_latency = 1, .target_residency = 1 },
			{ .name = "C2", .desc = "core power down", .enter = enter_powerdown,
			  .enter_s2idle = enter_s2idle,
			  .flags = CPUIDLE_FLAG_TIMER_STOP | CPUIDLE_FLAG_RCU_IDLE,
			  .exit_latency = 220, .target_residency = 3500 },
		},
		.state_count = 2,
	},
};
static bool registered[3];

static void zrn_broadcast_exit(void);
static void hp_remove(void);

static void zrn_idle_exit(void)
{
	hp_remove();
	/* off the idle states first: they are what needs the broadcast */
	for (int i = 0; i < 3; i++) {
		if (registered[i])
			cpuidle_unregister(&drivers[i]);
		kfree(drivers[i].cpumask);
	}
	zrn_sicd_exit();
	zrn_broadcast_exit();
	if (bid)
		iounmap(bid);
	if (pmu)
		iounmap(pmu);
}


/*
 * ------------------------------------------------- the MCT's global comparator
 *
 * A core that powers down also stops its arch timer, and this kernel has no
 * hardware broadcast timer: the only one registered is bc_hrtimer, the
 * software fallback, which schedules on a CPU that is itself about to stop
 * ticking. Anything sleeping on a timer on an idled core then never wakes
 * (measured: sshd hung while the phone still answered ping).
 *
 * The Exynos MCT (0x10040000) runs off the oscillator and never stops. Its
 * global comparator 0, GIC SPI 466 (from the stock device tree's mct-map),
 * becomes the broadcast device instead: timers of powered-down cores are
 * served by a G0 interrupt on cpu0, which IPIs the sleeping cores. Samsung's
 * kernel uses the MCT for exactly this.
 *
 * The rate is measured, not assumed: the comparator cycle count comes from
 * delta_ns * rate / NSEC_PER_SEC, and a wrong rate moves every broadcast
 * wakeup.
 */
#define MCT_PHYS		0x10040000UL
#define MCT_G_CNT_L		0x100
#define MCT_G_CNT_U		0x104
#define MCT_G_CNT_WSTAT		0x110
#define MCT_G_COMP0_L		0x200
#define MCT_G_COMP0_U		0x204
#define MCT_G_COMP1_L		0x210
#define MCT_G_COMP1_U		0x214
#define MCT_G_TCON		0x240
#define MCT_G_INT_CSTAT		0x244
#define MCT_G_INT_ENB		0x248
#define MCT_G_WSTAT		0x24c
#define MCT_TCON_START		(1U << 8)
#define MCT_TCON_COMP0_EN	(1U << 0)
#define MCT_G0_SPI		466
#define MCT_G1_SPI		467 /* Samsung DT mct-map entry 1 */

static void __iomem *mct;
static struct clock_event_device zrn_bc;
static unsigned int zrn_bc_irq = ~0U;
static bool zrn_bc_live;
static unsigned long zrn_mct_hz;
static unsigned long mct_late_events;
module_param(mct_late_events, ulong, 0444);
static DEFINE_RAW_SPINLOCK(mct_lock);
static unsigned int alarm_irq = ~0U, alarm_seconds;
static unsigned long alarm_fires;
module_param(alarm_fires, ulong, 0444);

static void mct_wr(u32 v, u32 off, u32 wstat_bit)
{
	u32 stat;

	writel_relaxed(v, mct + off);
	if (readl_poll_timeout_atomic(mct + MCT_G_WSTAT, stat,
				      stat & wstat_bit, 1, 100))
		pr_warn("zrn-idle: MCT write %#x did not land\n", off);
	writel_relaxed(wstat_bit, mct + MCT_G_WSTAT);	/* the status clears on a 1 */
}

static u64 mct_count(void)
{
	u32 hi, hi2, lo;

	hi = readl_relaxed(mct + MCT_G_CNT_U);
	do {
		lo = readl_relaxed(mct + MCT_G_CNT_L);
		hi2 = readl_relaxed(mct + MCT_G_CNT_U);
		if (hi == hi2)
			return ((u64)hi << 32) | lo;
		hi = hi2;
	} while (1);
}

static unsigned long mct_measure_rate(void)
{
	u64 a, b, ns;
	ktime_t t0 = ktime_get();

	a = mct_count();
	while (ktime_to_ns(ktime_sub(ktime_get(), t0)) < 200 * NSEC_PER_MSEC)
		cpu_relax();
	ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	b = mct_count();
	return ns ? (unsigned long)div64_u64((b - a) * NSEC_PER_SEC, ns) : 0;
}

static void mct_comp0_stop(void)
{
	u32 tcon = readl_relaxed(mct + MCT_G_TCON);

	writel_relaxed(readl_relaxed(mct + MCT_G_INT_ENB) & ~1U, mct + MCT_G_INT_ENB);
	tcon &= ~MCT_TCON_COMP0_EN;
	mct_wr(tcon, MCT_G_TCON, 1U << 16);
}

static int zrn_bc_set_next(unsigned long cycles, struct clock_event_device *evt)
{
	u64 comp;
	u32 tcon;
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&mct_lock, flags);
	comp = mct_count() + cycles;
	tcon = readl_relaxed(mct + MCT_G_TCON);

	writel_relaxed(1, mct + MCT_G_INT_CSTAT);
	tcon &= ~MCT_TCON_COMP0_EN;
	mct_wr(tcon, MCT_G_TCON, 1U << 16);
	mct_wr((u32)comp, MCT_G_COMP0_L, 1U << 0);
	mct_wr((u32)(comp >> 32), MCT_G_COMP0_U, 1U << 1);
	writel_relaxed(readl_relaxed(mct + MCT_G_INT_ENB) | 1U, mct + MCT_G_INT_ENB);
	tcon |= MCT_TCON_COMP0_EN;
	mct_wr(tcon, MCT_G_TCON, 1U << 16);
	/* MCT compares for equality. If programming took longer than the
	 * requested interval, the event will never arrive. Returning success
	 * leaves tick_broadcast_force_mask set and every CPU polling forever.
	 * Let the clockevent core retry with its minimum-delta backoff. */
	if ((s64)(mct_count() - comp) >= 0) {
		mct_comp0_stop();
		mct_late_events++;
		ret = -ETIME;
	}
	raw_spin_unlock_irqrestore(&mct_lock, flags);
	return ret;
}

static int zrn_bc_stop(struct clock_event_device *evt)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&mct_lock, flags);
	if (zrn_bc_live)
		mct_comp0_stop();
	raw_spin_unlock_irqrestore(&mct_lock, flags);
	return 0;
}

/* Comparator 1 is independent of the broadcast clockevent (comparator 0).
 * It supplies an oscillator-backed, NO_SUSPEND test wakeup even when every
 * CPU tick and timekeeping have stopped. The IRQ mapping is in Samsung's
 * firmware DT mct-map: G0=466, G1=467. No alarm is armed at module load. */
static void alarm_stop_locked(void)
{
	writel_relaxed(readl_relaxed(mct + MCT_G_INT_ENB) & ~2U, mct + MCT_G_INT_ENB);
	mct_wr(readl_relaxed(mct + MCT_G_TCON) & ~BIT(2), MCT_G_TCON, BIT(16));
	writel_relaxed(2, mct + MCT_G_INT_CSTAT);
}

static irqreturn_t alarm_interrupt(int irq, void *unused)
{
	unsigned long flags;

	if (!(readl_relaxed(mct + MCT_G_INT_CSTAT) & 2))
		return IRQ_NONE;
	raw_spin_lock_irqsave(&mct_lock, flags);
	alarm_stop_locked();
	alarm_seconds = 0;
	alarm_fires++;
	raw_spin_unlock_irqrestore(&mct_lock, flags);
	pm_system_wakeup();
	pr_info("zrn-idle: MCT G1 wake alarm fired\n");
	return IRQ_HANDLED;
}

static int alarm_set(const char *value, const struct kernel_param *kp)
{
	unsigned int seconds;
	unsigned long flags;
	u64 target;
	int ret = kstrtouint(value, 0, &seconds);

	if (ret || seconds > 86400)
		return ret ? ret : -EINVAL;
	if (!mct || alarm_irq == ~0U)
		return -ENODEV;
	raw_spin_lock_irqsave(&mct_lock, flags);
	alarm_stop_locked();
	alarm_seconds = seconds;
	if (seconds) {
		target = mct_count() + (u64)seconds * zrn_mct_hz;
		/* Four write-status bits per comparator (L, U, ADD_INCR, pad). */
		mct_wr(lower_32_bits(target), MCT_G_COMP1_L, BIT(4));
		mct_wr(upper_32_bits(target), MCT_G_COMP1_U, BIT(5));
		writel_relaxed(readl_relaxed(mct + MCT_G_INT_ENB) | 2U, mct + MCT_G_INT_ENB);
		mct_wr(readl_relaxed(mct + MCT_G_TCON) | BIT(2), MCT_G_TCON, BIT(16));
	}
	raw_spin_unlock_irqrestore(&mct_lock, flags);
	return 0;
}

static const struct kernel_param_ops alarm_ops = {
	.set = alarm_set,
	.get = param_get_uint,
};
module_param_cb(alarm_seconds, &alarm_ops, &alarm_seconds, 0600);
MODULE_PARM_DESC(alarm_seconds, "one-shot hardware wake alarm, 1..86400 s; 0 cancels");

static void __init zrn_alarm_init(struct irq_fwspec *spec)
{
	int virq, ret;

	if (readl_relaxed(mct + MCT_G_TCON) & BIT(2)) {
		pr_warn("zrn-idle: MCT G1 already active; leaving it alone\n");
		return;
	}
	spec->param[1] = MCT_G1_SPI;
	virq = irq_create_fwspec_mapping(spec);
	if (virq <= 0)
		return;
	ret = request_irq(virq, alarm_interrupt, IRQF_NO_SUSPEND,
			  "zrn-mct-wake", &alarm_seconds);
	if (ret)
		goto dispose;
	ret = irq_set_irq_wake(virq, 1);
	if (ret) {
		free_irq(virq, &alarm_seconds);
		goto dispose;
	}
	irq_set_affinity(virq, cpumask_of(0));
	alarm_irq = virq;
	pr_info("zrn-idle: MCT G1 wake alarm on IRQ %d\n", virq);
	return;
dispose:
	irq_dispose_mapping(virq);
}

static irqreturn_t zrn_bc_isr(int irq, void *dev)
{
	struct clock_event_device *evt = dev;

	/* only comparator 0's flag is ours; a stray line (wrong SPI guess)
	 * shows up as spurious instead of waking cores at random */
	if (!(readl_relaxed(mct + MCT_G_INT_CSTAT) & 1))
		return IRQ_NONE;
	writel_relaxed(1, mct + MCT_G_INT_CSTAT);
	evt->event_handler(evt);
	return IRQ_HANDLED;
}

/*
 * Register the comparator as a global oneshot clockevent. Rating 300: above
 * bc_hrtimer (0), so the framework installs it as the broadcast device, and
 * below the arch timers (~400), so the per-CPU tick devices are untouched.
 */
static int __init zrn_broadcast_init(void)
{
	struct device_node *gic;
	struct irq_fwspec spec = { };
	unsigned long rate;
	u32 tcon;
	int virq, ret;

	mct = ioremap(MCT_PHYS, 0x800);
	if (!mct)
		return -ENOMEM;

	tcon = readl_relaxed(mct + MCT_G_TCON);
	if (!(tcon & MCT_TCON_START)) {	/* the counter is not even running */
		mct_wr(tcon | MCT_TCON_START, MCT_G_TCON, 1U << 16);
	}
	rate = mct_measure_rate();
	if (rate < 1000000) {
		pr_err("zrn-idle: MCT not ticking (rate %lu Hz): no broadcast, no C2\n", rate);
		ret = -ENODEV;
		goto err;
	}
	pr_info("zrn-idle: MCT G0 at %lu Hz is the broadcast timer\n", rate);
	zrn_mct_hz = rate;

	gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	if (!gic) {
		ret = -ENODEV;
		goto err;
	}
	spec.fwnode = of_fwnode_handle(gic);
	spec.param_count = 3;
	spec.param[0] = 0;			/* GIC_SPI */
	spec.param[1] = MCT_G0_SPI;
	spec.param[2] = IRQ_TYPE_LEVEL_HIGH;
	of_node_put(gic);
	virq = irq_create_fwspec_mapping(&spec);
	if (virq <= 0) {
		ret = -ENOENT;
		goto err;
	}

	zrn_bc = (struct clock_event_device){
		.name = "zrn-mct-g0",
		.features = CLOCK_EVT_FEAT_ONESHOT,
		.rating = 300,
		.irq = virq,
		.set_next_event = zrn_bc_set_next,
		.set_state_shutdown = zrn_bc_stop,
		.set_state_oneshot = zrn_bc_stop,
		.set_state_oneshot_stopped = zrn_bc_stop,
		.tick_resume = zrn_bc_stop,
		.cpumask = cpumask_of(0),
		.owner = THIS_MODULE,
	};
	ret = request_irq(virq, zrn_bc_isr, IRQF_TIMER | IRQF_IRQPOLL,
			  "zrn-mct-g0", &zrn_bc);
	if (ret)
		goto err_dispose;
	zrn_bc_irq = virq;
	/* Several synchronized MMIO writes precede arming the comparator.
	 * Fifteen cycles (<1 us) can expire before it is enabled. */
	clockevents_config_and_register(&zrn_bc, rate, DIV_ROUND_UP(rate, 5000), UINT_MAX);
	zrn_bc_live = true;
	zrn_alarm_init(&spec);
	return 0;

err_dispose:
	irq_dispose_mapping(virq);
err:
	iounmap(mct);
	mct = NULL;
	return ret;
}

static void zrn_broadcast_exit(void)
{
	/* clockevents cannot be unregistered from a module. While the comparator
	 * is the broadcast timer the framework holds it (dev->owner), so the
	 * module is pinned and this never runs; it only cleans up a device that
	 * registered but was never installed. */
	if (!zrn_bc_live)
		return;
	zrn_bc_live = false;
	if (alarm_irq != ~0U) {
		unsigned long flags;

		raw_spin_lock_irqsave(&mct_lock, flags);
		alarm_stop_locked();
		raw_spin_unlock_irqrestore(&mct_lock, flags);
		irq_set_irq_wake(alarm_irq, 0);
		free_irq(alarm_irq, &alarm_seconds);
		irq_dispose_mapping(alarm_irq);
		alarm_irq = ~0U;
	}
	free_irq(zrn_bc_irq, &zrn_bc);
	irq_dispose_mapping(zrn_bc_irq);
	iounmap(mct);
	mct = NULL;
}

/* ---- CPU hotplug (`hotplug`, off by default) ------------------------------- */
/*
 * An offlined core comes back only if its wake line was armed as it went
 * down (Samsung's exynos_hotplug_out_callback/_in_callback; upstream ztOS's
 * power/hotplug/zrn_hotplug_main.c for the 9810). Without it CPU_ON fails
 * ("CPU4: failed to come online") and system suspend, which takes CPUs 1-7
 * offline, hung the phone (12.11 12:37). The writes are C2's coreNN_off and
 * coreNN_on: on the dying core itself with interrupts off, just before the
 * firmware's CPU_OFF, and on the core that has just come back. Offline
 * cores stay in bid_guard's set so another core's GRP2 write cannot drop
 * their line.
 *
 * The last core of the M4 pair (6-7) takes that cluster off. Stock
 * (exynos-cpupm.c cpuhp_cpupm_offline -> cal_cluster_disable, flexpmu 9820)
 * does it with no sequence of its own: only CPU_INFORM = CPD (2) on that
 * dying core, after its coreNN_off; the first core back runs cluster2_on
 * (SYSREG_CPUCL2 BUS_COMPONENT_DRCG_EN = ~0) and its hint is cleared.
 * big_reset_control() there is DFD dump only. And that core's CPU_OFF
 * carries affinity level 1 (stock arch/arm64/kernel/psci.c cpu_psci_cpu_die:
 * exynos_cpuhp_last_cpu() -> state 0x01010000): only then does the firmware
 * clean the M4 cluster's cache before the cluster goes. Mainline always
 * passes 0x10000, the dirty lines of the dying core were lost and CPU0
 * panicked in select_task_rq_fair on the sched domains cpu7 had just
 * rebuilt (12.11 13:05). So `hp_cluster`=1 swaps psci_ops.cpu_off for
 * zrn_psci_cpu_off(), which adds the level for that core; it needs
 * `psci_ops` and `psci_cpu_off` (addresses from /proc/kallsyms) and checks
 * the pointer is psci_cpu_off before it touches it; 0 or unloading puts it
 * back. Refused unless `hp_cluster` is set (0644, default 0). Cores 0-5 share one DSU and CPU0 never goes, so
 * any of 1-5 may go. `hotplug` (0644): 1 registers the callbacks, 0 removes
 * them (refused while a core is offline). hp_out / hp_in / hp_refused /
 * hp_cl2_off / hp_cl2_on count.
 */
#define ZRN_DYING_STATE		CPUHP_AP_ARM_XEN_STARTING	/* a "dying" step nobody uses here */
static bool hotplug;
static int hp_online_state = -1;
static unsigned long hp_out, hp_in, hp_refused, hp_mismatch, hp_cl2_off, hp_cl2_on;
static bool hp_cluster;
static int hp_cl_last = -1;	/* the core taking the M4 cluster off; under pmu_lock */
static unsigned long psci_ops_addr, psci_cpu_off_addr, hp_cl2_lvl1;
static unsigned int hp_cl2_state;
module_param_named(psci_ops, psci_ops_addr, ulong, 0644);
MODULE_PARM_DESC(psci_ops, "address of psci_ops (/proc/kallsyms), for hp_cluster");
module_param_named(psci_cpu_off, psci_cpu_off_addr, ulong, 0644);
MODULE_PARM_DESC(psci_cpu_off, "address of psci_0_2_cpu_off (/proc/kallsyms), for hp_cluster");
module_param(hp_cl2_lvl1, ulong, 0444);
module_param(hp_cl2_state, uint, 0444);
module_param(hp_cl2_off, ulong, 0444);
module_param(hp_cl2_on, ulong, 0444);
module_param(hp_out, ulong, 0444);
module_param(hp_in, ulong, 0444);
module_param(hp_refused, ulong, 0444);
module_param(hp_mismatch, ulong, 0444);

static unsigned int hp_group(unsigned int cpu)
{
	return cpu < 6 ? 0 : 1;
}

/* early in going offline, while it can still be refused */
static int zrn_hp_may_go(unsigned int cpu)
{
	unsigned int other, left = 0;

	if (cpu > 7)
		return -EBUSY;
	for_each_online_cpu(other)
		left += other != cpu && hp_group(other) == hp_group(cpu);
	if (!left && cpu >= 6 && READ_ONCE(hp_cluster))
		return 0;
	if (!left) {
		hp_refused++;
		pr_info("zrn-idle: cpu%u is the last of its cluster and stays\n", cpu);
		return -EBUSY;
	}
	return 0;
}

/* on the dying core, interrupts off: the hint and its line armed */
static int zrn_hp_dying(unsigned int cpu)
{
	unsigned int core = zrn_core(read_cpuid_mpidr());
	bool last;

	if (core > 7)
		return 0;
	if (core != cpu)
		hp_mismatch++;
	raw_spin_lock(&pmu_lock);
	hp_down |= BIT(core);
	last = core >= 6 && (hp_down & 0xc0) == 0xc0;
	raw_spin_unlock(&pmu_lock);
	if (last)
		WRITE_ONCE(hp_cl_last, core);
	core_off(core);
	if (last) {	/* cal_cluster_disable: the hint, after coreNN_off */
		writel_relaxed(INFORM_CPD, pmu + PMU_CPU_INFORM(core));
		readl_relaxed(pmu + PMU_CPU_INFORM(core));
		hp_cl2_off++;
	}
	hp_out++;
	return 0;
}

/* on the core that has just come back: its line disarmed */
static int zrn_hp_starting(unsigned int cpu)
{
	unsigned int core = zrn_core(read_cpuid_mpidr());
	bool first;

	if (core > 7)
		return 0;
	raw_spin_lock(&pmu_lock);
	first = core >= 6 && (hp_down & 0xc0) == 0xc0;
	hp_down &= ~BIT(core);
	if (first)
		WRITE_ONCE(hp_cl_last, -1);
	raw_spin_unlock(&pmu_lock);
	if (first) {	/* cal_cluster_enable: cluster2_on before cpu_enable */
		zrn_cl2_drcg_on();
		hp_cl2_on++;
	}
	core_on(core);
	hp_in++;
	return 0;
}

/*
 * psci_ops.cpu_off while hp_cluster is set: on the dying core, after
 * cpuhp_ap_report_dead(), as the very last call (it does not return).
 */
static int (*hp_orig_cpu_off)(u32 state);

static int zrn_psci_cpu_off(u32 state)
{
	unsigned int core = zrn_core(read_cpuid_mpidr());

	if (core < 8 && READ_ONCE(hp_cl_last) == (int)core) {
		state |= 1U << PSCI_0_2_POWER_STATE_AFFL_SHIFT;
		hp_cl2_state = state;
		hp_cl2_lvl1++;
	}
	return hp_orig_cpu_off(state);
}

static int (**hp_cpu_off_slot(void))(u32)
{
	return (int (**)(u32))(psci_ops_addr + offsetof(struct psci_operations, cpu_off));
}

/* under cpus_read_lock(): no core is on its way down meanwhile */
static int hp_hook(bool on)
{
	int (**slot)(u32);

	if (!psci_ops_addr || !psci_cpu_off_addr)
		return on ? -EINVAL : 0;
	slot = hp_cpu_off_slot();
	if (on) {
		if (READ_ONCE(*slot) == zrn_psci_cpu_off)
			return 0;
		if ((unsigned long)READ_ONCE(*slot) != psci_cpu_off_addr) {
			pr_err("zrn-idle: psci_ops.cpu_off is %px, not psci_cpu_off: no hp_cluster\n",
			       READ_ONCE(*slot));
			return -EINVAL;
		}
		hp_orig_cpu_off = (void *)psci_cpu_off_addr;
		WRITE_ONCE(*slot, zrn_psci_cpu_off);
		pr_info("zrn-idle: hp_cluster: the last of cores 6-7 goes with CPU_OFF level 1\n");
	} else if (READ_ONCE(*slot) == zrn_psci_cpu_off) {
		WRITE_ONCE(*slot, hp_orig_cpu_off);
	}
	return 0;
}

static int hp_cluster_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	cpus_read_lock();
	if (!on && (hp_down & 0xc0) == 0xc0)
		ret = -EBUSY;	/* the cluster is off: bring 6 or 7 back first */
	else
		ret = hp_hook(on);
	if (!ret)
		WRITE_ONCE(hp_cluster, on);
	cpus_read_unlock();
	return ret;
}

static const struct kernel_param_ops hp_cluster_ops = {
	.set = hp_cluster_set,
	.get = param_get_bool,
};
module_param_cb(hp_cluster, &hp_cluster_ops, &hp_cluster, 0644);
MODULE_PARM_DESC(hp_cluster, "let the last of cores 6-7 go too, cluster off, CPU_OFF level 1 (default 0)");

static void hp_remove(void)
{
	if (psci_ops_addr && READ_ONCE(*hp_cpu_off_slot()) == zrn_psci_cpu_off) {
		cpus_read_lock();
		hp_hook(false);
		WRITE_ONCE(hp_cluster, false);
		cpus_read_unlock();
		msleep(100);	/* a dying core may still be in zrn_psci_cpu_off */
	}
	if (hp_online_state < 0)
		return;
	cpuhp_remove_state_nocalls(ZRN_DYING_STATE);
	cpuhp_remove_state_nocalls(hp_online_state);
	hp_online_state = -1;
}

static int hotplug_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	if (!pmu || !bid)
		return -ENODEV;
	if (on == (hp_online_state >= 0))
		return 0;
	if (!on) {
		if (num_online_cpus() != num_possible_cpus())
			return -EBUSY;	/* an offline core needs the starting callback */
		hp_remove();
		hotplug = false;
		return 0;
	}
	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "soc/zrn-idle-bx:online",
					NULL, zrn_hp_may_go);
	if (ret < 0)
		return ret;
	hp_online_state = ret;
	ret = cpuhp_setup_state_nocalls(ZRN_DYING_STATE, "soc/zrn-idle-bx:dying",
					zrn_hp_starting, zrn_hp_dying);
	if (ret < 0) {
		cpuhp_remove_state_nocalls(hp_online_state);
		hp_online_state = -1;
		return ret;
	}
	hotplug = true;
	pr_info("zrn-idle: hotplug on (cores 1-7 may go offline and come back; last of 6-7 only with hp_cluster)\n");
	return 0;
}

static const struct kernel_param_ops hotplug_ops = {
	.set = hotplug_set,
	.get = param_get_bool,
};
module_param_cb(hotplug, &hotplug_ops, &hotplug, 0644);
MODULE_PARM_DESC(hotplug, "arm offlined cores' wake lines so they come back (default 0)");

static int __init zrn_idle_init(void)
{
	int ret = 0;

	if (!suspend_fn) {
		pr_err("zrn-idle: give suspend_enter= (cpu_suspend from /proc/kallsyms)\n");
		return -EINVAL;
	}
	if (!resume_fn) {
		pr_err("zrn-idle: give resume= (cpu_resume from /proc/kallsyms)\n");
		return -EINVAL;
	}

	cpu_suspend_p = (void *)suspend_fn;
	if (resume_fn >= 0xffff000000000000UL)
		cpu_resume_pa = __pa_symbol(resume_fn);
	else
		cpu_resume_pa = resume_fn;

	pr_info("zrn-idle: cpu_suspend=%px, cpu_resume=%px (PA 0x%lx)\n",
		cpu_suspend_p, (void *)resume_fn, cpu_resume_pa);

	ret = zrn_broadcast_init();
	if (ret) {
		pr_err("zrn-idle: no broadcast timer: staying out of the kernel's way\n");
		return ret;
	}
	pmu = ioremap(PMU_BASE, 0x8000);
	bid = ioremap(BID_BASE, 0x1000);
	if (!pmu || !bid) {
		zrn_idle_exit();
		return -ENOMEM;
	}
	if (zrn_sicd_init())
		pr_warn("zrn-idle: no SICD (its BID mapping failed)\n");

	for (int i = 0; i < 3; i++) {
		struct cpumask *mask = kzalloc(cpumask_size(), GFP_KERNEL);
		unsigned int cpu;

		if (!mask) {
			ret = -ENOMEM;
			break;
		}
		drivers[i].cpumask = mask;
		/* CPUs 0-3 little, 4-5 mid (A75), 6-7 big (M4), as the DT lists them */
		for_each_possible_cpu(cpu) {
			if ((cpu < 4 ? 0 : cpu < 6 ? 1 : 2) == i && (cpus & BIT(cpu)))
				cpumask_set_cpu(cpu, mask);
		}
		if (cpumask_empty(mask))
			continue;
		ret = cpuidle_register(&drivers[i], NULL);
		if (ret) {
			pr_err("zrn-idle: %s: %d\n", drivers[i].name, ret);
			break;
		}
		registered[i] = true;
		pr_info("zrn-idle: %s on CPUs %*pbl\n", drivers[i].name, cpumask_pr_args(mask));
	}
	if (ret)
		zrn_idle_exit();
	return ret;
}

/* ---- shared with zrn-sicd.o, the SICD state of the same module --------------- */

/* which cores are down in C2, for SICD's "every online core" gate (vendor:
 * exynos-powermode.c's c2_mask under c2_lock) */
DEFINE_RAW_SPINLOCK(zrn_c2_lock);
unsigned long zrn_c2_mask[BITS_TO_LONGS(NR_CPUS)];

void zrn_c2_mark(unsigned int core, bool down)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&zrn_c2_lock, flags);
	if (down)
		set_bit(core, zrn_c2_mask);
	else
		clear_bit(core, zrn_c2_mask);
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
}

void __iomem *zrn_pmu_base(void) { return pmu; }
void __iomem *zrn_bid_base(void) { return bid; }
int zrn_cpu_suspend_call(unsigned long state, int (*finisher)(unsigned long))
{
	return cpu_suspend_p(state, finisher);
}
unsigned long zrn_cpu_resume_pa(void)
{
	return cpu_resume_pa;
}
u64 zrn_mct_count_now(void)
{
	return mct ? mct_count() : 0;
}
unsigned long zrn_mct_rate_now(void)
{
	return zrn_mct_hz;
}

module_init(zrn_idle_init);
module_exit(zrn_idle_exit);
MODULE_DESCRIPTION("Exynos 9820 per-core power-down idle state with the flexible PMU's hints");
MODULE_LICENSE("GPL");
