// SPDX-License-Identifier: GPL-2.0-only
/*
 * zrn-system-sleep-bx: Exynos 9820 (Galaxy S10 5G) SYS_SLEEP backend.
 * Added for OmonullOS, November 2026; structure from power/suspend/platform.c
 * (the S9's, Exynos 9810), addresses and sequence from Samsung's G977N
 * sources: exynos-powermode.c, pmucal_system.c, flexpmu_cal_*_exynos9820.h,
 * exynos9820.dts (exynos-powermode wakeup-masks, suspend_psci_idx 1024).
 *
 * Stock SYS_SLEEP, in order:
 *   PMU 0x60c/0x61c   EINT wake mask (a 0 bit lets that ALIVE EINT wake)
 *   PMU 0x3950/0x3954 wake status cleared
 *   PMU 0x3944        WAKEUP_INT_EN  = 0x1f6f   (usbl2: 0x81f6f)
 *   PMU 0x3964        WAKEUP2_INT_EN = 0xf      (usbl2: 0xc)
 *   PMU 0x860 + 4cpu  CPU_INFORM = 4 (SLEEP; usbl2 5)
 *   save_sleep SAVE, enter_sleep HANDLE
 *   SMC 0xC4000001 (CPU_SUSPEND) x1 = 0x03000000 (PSCI_SYSTEM_SLEEP:
 *     psci_power_state_pack(0, 0, 3)), x2 = cpu_resume's physical address
 *   woken: exit_sleep HANDLE + save_sleep RESTORE
 *   refused (SMC returned): earlywkup_sleep HANDLE + RESTORE
 *
 * Steps, each with the owner's «ha» (docs/s10-5g/ROADMAP.md "tizim uyqusi"):
 *   load            validate + ioremap only, no register access
 *   prepare_test=1  step 3: the whole sequence above minus the SMC, on CPU 0
 *                   inside stop_machine (other CPUs spin with IRQs off),
 *                   then earlywkup + restore. PMU writes, no power change.
 *   enable_deep=1   (load time) registers the suspend backend: "deep" in
 *                   /sys/power/mem_sleep. dry_run (default) refuses at enter,
 *                   before any register write: tests Linux's own suspend.
 *   dry_run=0       prepare_only (default) runs prepare + earlywkup in the
 *                   real suspend finisher, still no SMC.
 *   prepare_only=0  the firmware call.
 *
 * A TOP-domain pin controller (PERIC0/1, FSYS0/1) loses its registers in
 * SYS_SLEEP and exit_sleep releases the pads (TOP_OUT). Our DT drives none of
 * them through pinctrl (only ALIVE 0x15850000 is in exynos9820.dtsi), so their
 * CON/DAT/PUD/DRV come back here, before the exit table, as the S9 learned.
 */
#include <linux/arm-smccc.h>
#include <linux/cpu_pm.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/stop_machine.h>
#include <linux/suspend.h>
#include <asm/memory.h>
#include "sequence.h"
#include "sleep-tables-9820.h"

#define PMU			0x15860000u
#define BID			0x15870000u	/* PMU_INTR_GEN */
#define PMU_EINT_WAKEUP_MASK	0x060c
#define PMU_EINT_WAKEUP_MASK2	0x061c
#define PMU_CPU_INFORM(c)	(0x0860 + 4 * (c))
#define PMU_WAKEUP_INT_EN	0x3944
#define PMU_WAKEUP_STAT		0x3950
#define PMU_WAKEUP2_STAT	0x3954
#define PMU_WAKEUP2_INT_EN	0x3964
#define PMU_G3D_STATUS		0x2384
#define BID_GRP4_PEND		0x0408
#define INFORM_SLEEP		4
#define INFORM_SLEEP_USBL2	5
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_SYSTEM_SLEEP	0x03000000UL
#define MAX_REGIONS		64

/* --- parameters --------------------------------------------------------- */
static bool dry_run = true, prepare_only = true, enable_deep, usbl2;
static bool restore_gpio = true;
/* gpa2-4 (power key) = EINT bit 8 x 2 + 4 open; everything else masked.
 * Later steps add the modem's and the alarm's wake lines. */
static unsigned int eint_wake = (u32)~BIT(20), eint_wake2 = ~0U;
static unsigned long suspend_enter, resume;
static int set_locked_bool(const char *value, const struct kernel_param *kp)
{
	unsigned int flags = lock_system_sleep();
	int ret = param_set_bool(value, kp);

	unlock_system_sleep(flags);
	return ret;
}
static const struct kernel_param_ops locked_bool_ops = {
	.set = set_locked_bool, .get = param_get_bool,
};
module_param_cb(dry_run, &locked_bool_ops, &dry_run, 0600);
MODULE_PARM_DESC(dry_run, "suspend backend refuses at enter, no register touched (default 1)");
module_param_cb(prepare_only, &locked_bool_ops, &prepare_only, 0600);
MODULE_PARM_DESC(prepare_only, "prepare + early-wakeup restore, no SMC (default 1)");
module_param_cb(usbl2, &locked_bool_ops, &usbl2, 0600);
module_param(enable_deep, bool, 0444);
MODULE_PARM_DESC(enable_deep, "load time: register the suspend backend (pins the module)");
module_param(restore_gpio, bool, 0644);
module_param(eint_wake, uint, 0600);
module_param(eint_wake2, uint, 0600);
module_param(suspend_enter, ulong, 0400);
MODULE_PARM_DESC(suspend_enter, "cpu_suspend's address (/proc/kallsyms), enable_deep only");
module_param(resume, ulong, 0400);
MODULE_PARM_DESC(resume, "cpu_resume's address (/proc/kallsyms), enable_deep only");

/* --- results ------------------------------------------------------------ */
static unsigned int region_count, rows, prepare_tests, saved_rows, test_us;
static unsigned int last_table, last_index, wake_stat, wake_stat2, grp4_at_smc;
static unsigned int g3d_status;
static int last_error, last_restore_error, last_smc;
static unsigned long attempts, entries;
module_param(region_count, uint, 0444);
module_param(rows, uint, 0444);
module_param(prepare_tests, uint, 0444);
module_param(saved_rows, uint, 0444);
module_param(test_us, uint, 0444);
module_param(last_table, uint, 0444);
module_param(last_index, uint, 0444);
module_param(last_error, int, 0444);
module_param(last_restore_error, int, 0444);
module_param(last_smc, int, 0444);
module_param(wake_stat, uint, 0444);
module_param(wake_stat2, uint, 0444);
module_param(grp4_at_smc, uint, 0444);
module_param(g3d_status, uint, 0444);
module_param(attempts, ulong, 0444);
module_param(entries, ulong, 0444);

/* --- tables and register access ---------------------------------------- */
enum { T_ENTER, T_SAVE, T_EXIT, T_EARLY };
struct table { struct zrn_seq *seq; unsigned int count; };
#define TABLE(n) { n, ARRAY_SIZE(n) }
static struct table modes[2][4] = {
	{ TABLE(enter_sleep), TABLE(save_sleep), TABLE(exit_sleep), TABLE(earlywkup_sleep) },
	{ TABLE(enter_sleep_usbl2), TABLE(save_sleep_usbl2), TABLE(exit_sleep_usbl2),
	  TABLE(earlywkup_sleep_usbl2) },
};
static bool mode_usbl2;	/* usbl2 latched at prepare: restore uses the same tables */

static struct { u32 physical; void __iomem *mapped; } regions[MAX_REGIONS];

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

static void reg_write(void *context, u32 base, u32 offset, u32 value)
{
	writel_relaxed(value, mapped_base(base) + offset);
}

static void reg_delay(void *context, unsigned int usec) { udelay(usec); }
static const struct zrn_seq_io io = {
	.read = reg_read, .write = reg_write, .delay_us = reg_delay,
};

static int run_table(unsigned int index, enum zrn_seq_phase phase)
{
	struct table *t = &modes[mode_usbl2][index];
	int ret = zrn_sequence_run(t->seq, t->count, phase, &io, &last_index);

	if (ret) {
		last_table = index;
		pr_err("zrn-system-sleep-bx: table %u row %u (%s) phase %d: %d\n",
		       index, last_index, t->seq[last_index].name, phase, ret);
	}
	return ret;
}

/* --- TOP-domain pins ---------------------------------------------------- */
static const struct { u32 base; unsigned int banks; } gpio_banks[] = {
	{ 0x10430000, 8 },	/* PERIC0: gpp0-3, gpg0-2, gpg4 */
	{ 0x10830000, 9 },	/* PERIC1: gpp4-6, gpc0-1, gpd0, gpg3, gph0-1 */
	{ 0x13030000, 1 },	/* FSYS0: gpf0 */
	{ 0x13c40000, 3 },	/* FSYS1: gpf1-3 */
};
static u32 saved_gpio[ARRAY_SIZE(gpio_banks)][9][6];

/*
 * What a TOP pin does while the SoC sleeps: CON_PDN (+0x10, 2 bits a pin:
 * 0 out 0, 1 out 1, 2 input, 3 hold the level it had) and PUD_PDN (+0x14,
 * 4 bits a pin: 0 none, 1 down, 3 up). Nothing in our DT sets them, so the
 * first real SYS_SLEEP (12.11 14:32, 4c) ran these pins at their reset
 * values: the S5100 came back reset (PHONE_ACTIVE low, CP CRASH_EXIT), the
 * UFS failed its hibern8 exit (-110, host reset), the bottom speaker amp
 * NAKed (-6) and the Wi-Fi chip was off. The rows are stock's
 * samsung,pin-con-pdn entries for the TOP banks (exynos9820-pinctrl.dtsi,
 * exynos9820-beyondx_common.dtsi); CMGP (gpm) keeps its pads. Written in
 * prepare before gpio_save(), so the restore after a wake writes them back
 * too. pud 0xff: PUD_PDN left as found (stock sets none there).
 */
static const struct pdn_pin {
	u32 base;		/* pin controller, one of gpio_banks[] */
	u8 bank, pin;		/* bank index in it (0x20 apart), pin in the bank */
	u8 con, pud;		/* CON_PDN, PUD_PDN */
	const char *name;
} pdn_pins[] = {
	{ 0x10430000, 4, 5, 3, 0,    "gpg0-5" },	/* WL_REG_ON */
	{ 0x10430000, 5, 0, 3, 0,    "gpg1-0" },	/* codec reset */
	{ 0x10830000, 4, 0, 3, 0,    "gpc1-0" },	/* spi0: the S5100's SPI boot */
	{ 0x10830000, 4, 1, 3, 0,    "gpc1-1" },
	{ 0x10830000, 4, 2, 3, 0,    "gpc1-2" },
	{ 0x10830000, 4, 3, 3, 0,    "gpc1-3" },
	{ 0x10830000, 8, 1, 3, 0,    "gph1-1" },	/* bottom speaker amp reset */
	{ 0x10830000, 8, 2, 3, 0,    "gph1-2" },	/* receiver amp reset */
	{ 0x10830000, 8, 3, 3, 0xff, "gph1-3" },	/* AP2CP PDA_ACTIVE */
	{ 0x10830000, 8, 4, 3, 0,    "gph1-4" },	/* AP2CP NRESET_N */
	{ 0x13030000, 0, 0, 3, 3,    "gpf0-0" },	/* PCIe ch1 (modem) CLKREQ# */
	{ 0x13030000, 0, 2, 3, 0xff, "gpf0-2" },	/* PCIe ch1 PERST# */
	{ 0x13c40000, 1, 0, 3, 3,    "gpf2-0" },	/* PCIe ch0 (Wi-Fi) CLKREQ# */
	{ 0x13c40000, 1, 2, 3, 0xff, "gpf2-2" },	/* PCIe ch0 PERST# */
	{ 0x13c40000, 2, 0, 3, 0xff, "gpf3-0" },	/* UFS REFCLK */
	{ 0x13c40000, 2, 1, 3, 0xff, "gpf3-1" },	/* UFS RST_N */
};
static bool pdn = true;
module_param(pdn, bool, 0644);
MODULE_PARM_DESC(pdn, "hold the stock TOP pins through a sleep (CON_PDN 3) (default 1)");
static unsigned int pdn_applied;	/* rows written by the last prepare */
module_param(pdn_applied, uint, 0444);

/*
 * wl_off: the Wi-Fi chip off through the sleep (WL_REG_ON driven low,
 * CON_PDN 0) instead of held high, to measure what the BCM4375 draws there
 * (12.11 15:36: ~90-110 mA asleep). zsleep unbinds brcmfmac first. After a
 * real wake the pin is kept low (DAT cleared before the pads are released),
 * so pcie-exynos9820's resume_noirq sees it off and powers the module up
 * again with probe's delays. wl_dropped counts those wakes.
 */
static bool wl_off;
module_param(wl_off, bool, 0644);
MODULE_PARM_DESC(wl_off, "drive WL_REG_ON (gpg0-5) low through a sleep (default 0)");
static unsigned int wl_dropped;
module_param(wl_dropped, uint, 0444);
#define WL_BASE	0x10430000	/* PERIC0 pin controller */
#define WL_BANK	4		/* gpg0 */
#define WL_PIN	5

static void pdn_apply(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(pdn_pins); i++) {
		const struct pdn_pin *p = &pdn_pins[i];
		void __iomem *bank = mapped_base(p->base) + p->bank * 0x20;
		u32 v;

		v = readl_relaxed(bank + 0x10);
		v &= ~(0x3u << (2 * p->pin));
		writel_relaxed(v | (u32)(wl_off && p->base == WL_BASE &&
				 p->bank == WL_BANK && p->pin == WL_PIN ?
				 0 : p->con) << (2 * p->pin), bank + 0x10);
		if (p->pud != 0xff) {
			v = readl_relaxed(bank + 0x14);
			v &= ~(0xfu << (4 * p->pin));
			writel_relaxed(v | (u32)p->pud << (4 * p->pin), bank + 0x14);
		}
		n++;
	}
	wmb();
	pdn_applied = n;
}

static void gpio_save(void)
{
	unsigned int c, b, r;

	for (c = 0; c < ARRAY_SIZE(gpio_banks); c++)
		for (b = 0; b < gpio_banks[c].banks; b++)
			for (r = 0; r < 6; r++)	/* CON DAT PUD DRV CON_PDN PUD_PDN */
				saved_gpio[c][b][r] = readl_relaxed(
					mapped_base(gpio_banks[c].base) + b * 0x20 + r * 4);
}

static void gpio_restore(void)
{
	/* DAT first: an output comes back at its level, not at reset's 0 */
	static const unsigned int order[] = { 1, 2, 3, 4, 5, 0 };
	unsigned int c, b, i;

	for (c = 0; c < ARRAY_SIZE(gpio_banks); c++)
		for (b = 0; b < gpio_banks[c].banks; b++)
			for (i = 0; i < ARRAY_SIZE(order); i++)
				writel_relaxed(saved_gpio[c][b][order[i]],
					       mapped_base(gpio_banks[c].base) +
					       b * 0x20 + order[i] * 4);
	wmb();
}

/* --- what a real SYS_SLEEP loses outside the vendor tables -------------- *
 * The TOP domains come back from reset. The tables put the CMUs back; these
 * are the registers Linux and zboot set up themselves (12.11 4c review):
 *  - MCT 0x10040000: zrn-cpuidle-bx's broadcast timer. Its free-running
 *    counter stops (G_TCON START 0, as mainline exynos_mct restarts it in
 *    resume); zrn_bc_set_next keeps the START bit it reads, so a stopped
 *    counter never reaches a comparator and the idle cores never wake.
 *  - USI03 (touch, hsi2c_10) and USI_CMGP3 (MUIC): zboot's pre-usb.sh writes
 *    SW_CONF and releases USI_CON's reset; i2c-exynos5 knows neither. Order
 *    and offsets as the S9's platform.c (sysreg mode, USI_OPTION, USI_CON).
 *    gpm12-0/gpm13-0 (USI_CMGP3's pins) are CMGP pins, not in gpio_banks.
 *  - uniLoader's cluster-0 watchdog 0x10050000 (zboot pets WTCNT): after a
 *    reset its WTCON default runs it at another rate; put the boot setup back.
 * Saved in prepare, written after the exit table and the CMU restore on a
 * real wake only (an early wakeup loses nothing). post_lost: bit 0 MCT
 * stopped, 1 USI03 mode, 2 USI_CMGP3 mode, 3 watchdog, 4 gpm12/13 CON.
 */
#define MCT_BASE		0x10040000
#define MCT_G_TCON		0x240
#define MCT_G_WSTAT		0x24c
#define MCT_TCON_START		BIT(8)
#define WDT_BASE		0x10050000
static const struct { u32 base, offset; } keep_regs[] = {
	{ 0x15c30000, 0x184 }, { 0x15c30000, 0x188 }, { 0x15c30000, 0x180 },	/* gpm12 DAT PUD CON */
	{ 0x15c30000, 0x1a4 }, { 0x15c30000, 0x1a8 }, { 0x15c30000, 0x1a0 },	/* gpm13 */
	{ 0x10420000, 0x101c },	/* SYSREG_PERIC0 USI03 SW_CONF */
	{ 0x104b0000, 0xc8 }, { 0x104b0000, 0xc4 },	/* hsi2c_10 USI_OPTION, USI_CON */
	{ 0x15c20000, 0x2030 },	/* SYSREG_CMGP USI3 SW_CONF */
	{ 0x15d60000, 0xc8 }, { 0x15d60000, 0xc4 },	/* hsi2c_cmgp3 */
};
static u32 saved_keep[ARRAY_SIZE(keep_regs)], saved_wtcon, saved_wtdat;
static bool post_fixups = true;
static unsigned int post_lost;
module_param(post_fixups, bool, 0644);
MODULE_PARM_DESC(post_fixups, "after a real wake: restart the MCT, put USI03/USI_CMGP3/watchdog back (default 1)");
module_param(post_lost, uint, 0444);

static void keep_save(void)
{
	void __iomem *wdt = mapped_base(WDT_BASE);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(keep_regs); i++)
		saved_keep[i] = readl_relaxed(mapped_base(keep_regs[i].base) + keep_regs[i].offset);
	saved_wtcon = readl_relaxed(wdt);
	saved_wtdat = readl_relaxed(wdt + 4);
}

static void keep_restore(void)
{
	void __iomem *mct = mapped_base(MCT_BASE), *wdt = mapped_base(WDT_BASE);
	unsigned int i, spins;
	u32 tcon;

	post_lost = 0;
	tcon = readl_relaxed(mct + MCT_G_TCON);
	if (!(tcon & MCT_TCON_START)) {
		post_lost |= BIT(0);
		writel_relaxed(tcon | MCT_TCON_START, mct + MCT_G_TCON);
		for (spins = 0; spins < 1000 && !(readl_relaxed(mct + MCT_G_WSTAT) & BIT(16)); spins++)
			udelay(1);
		writel_relaxed(BIT(16), mct + MCT_G_WSTAT);
	}
	if (readl_relaxed(mapped_base(keep_regs[6].base) + keep_regs[6].offset) != saved_keep[6])
		post_lost |= BIT(1);
	if (readl_relaxed(mapped_base(keep_regs[9].base) + keep_regs[9].offset) != saved_keep[9])
		post_lost |= BIT(2);
	if (readl_relaxed(mapped_base(keep_regs[2].base) + keep_regs[2].offset) != saved_keep[2])
		post_lost |= BIT(4);
	for (i = 0; i < ARRAY_SIZE(keep_regs); i++)
		writel_relaxed(saved_keep[i], mapped_base(keep_regs[i].base) + keep_regs[i].offset);
	if (readl_relaxed(wdt) != saved_wtcon || readl_relaxed(wdt + 4) != saved_wtdat) {
		post_lost |= BIT(3);
		writel_relaxed(0, wdt);
		writel_relaxed(saved_wtdat, wdt + 4);
		writel_relaxed(saved_wtdat, wdt + 8);
		writel_relaxed(saved_wtcon, wdt);
	}
	wmb();
}

/* --- prepare / restore -------------------------------------------------- */
static u32 saved_wake, saved_wake2, saved_eint, saved_eint2, saved_hint;
static unsigned int prepared_cpu;
static bool prepared;

static void restore_platform(bool early)
{
	void __iomem *pmu = mapped_base(PMU);
	int ret;

	if (!prepared)
		return;
	if (restore_gpio && !early)
		gpio_restore();	/* before exit_sleep releases the pads */
	if (wl_off && pdn && !early) {	/* keep the chip off: pcie powers it */
		void __iomem *dat = mapped_base(WL_BASE) + WL_BANK * 0x20 + 0x04;

		writel_relaxed(readl_relaxed(dat) & ~BIT(WL_PIN), dat);
		wmb();
		wl_dropped++;
	}
	ret = run_table(early ? T_EARLY : T_EXIT, ZRN_HANDLE);
	last_restore_error = run_table(T_SAVE, ZRN_RESTORE);
	if (!last_restore_error)
		last_restore_error = ret;
	if (post_fixups && !early)
		keep_restore();	/* the CMUs are back: these blocks answer again */
	writel_relaxed(saved_wake, pmu + PMU_WAKEUP_INT_EN);
	writel_relaxed(saved_wake2, pmu + PMU_WAKEUP2_INT_EN);
	writel_relaxed(saved_eint, pmu + PMU_EINT_WAKEUP_MASK);
	writel_relaxed(saved_eint2, pmu + PMU_EINT_WAKEUP_MASK2);
	writel_relaxed(saved_hint, pmu + PMU_CPU_INFORM(prepared_cpu));
	wmb();
	prepared = false;
}

static int prepare_platform(void)
{
	void __iomem *pmu = mapped_base(PMU);
	struct table *save;
	unsigned int i;
	int ret;

	/* save_sleep reads CMU_G3D unguarded (audit-tables-9820.py) */
	g3d_status = readl_relaxed(pmu + PMU_G3D_STATUS);
	if (!(g3d_status & 1))
		return -EBUSY;
	mode_usbl2 = usbl2;
	prepared_cpu = smp_processor_id();
	pdn_applied = 0;
	if (pdn)
		pdn_apply();	/* before the save: a wake writes them back too */
	gpio_save();
	keep_save();
	saved_wake = readl_relaxed(pmu + PMU_WAKEUP_INT_EN);
	saved_wake2 = readl_relaxed(pmu + PMU_WAKEUP2_INT_EN);
	saved_eint = readl_relaxed(pmu + PMU_EINT_WAKEUP_MASK);
	saved_eint2 = readl_relaxed(pmu + PMU_EINT_WAKEUP_MASK2);
	saved_hint = readl_relaxed(pmu + PMU_CPU_INFORM(prepared_cpu));

	writel_relaxed(eint_wake, pmu + PMU_EINT_WAKEUP_MASK);
	writel_relaxed(eint_wake2, pmu + PMU_EINT_WAKEUP_MASK2);
	writel_relaxed(0, pmu + PMU_WAKEUP_STAT);
	writel_relaxed(0, pmu + PMU_WAKEUP2_STAT);
	writel_relaxed(mode_usbl2 ? 0x81f6f : 0x1f6f, pmu + PMU_WAKEUP_INT_EN);
	writel_relaxed(mode_usbl2 ? 0xc : 0xf, pmu + PMU_WAKEUP2_INT_EN);
	writel_relaxed(mode_usbl2 ? INFORM_SLEEP_USBL2 : INFORM_SLEEP,
		       pmu + PMU_CPU_INFORM(prepared_cpu));
	prepared = true;
	last_restore_error = 0;
	ret = run_table(T_SAVE, ZRN_SAVE);
	save = &modes[mode_usbl2][T_SAVE];
	for (i = saved_rows = 0; i < save->count; i++)
		saved_rows += save->seq[i].valid;
	if (!ret)
		ret = run_table(T_ENTER, ZRN_HANDLE);
	wmb();
	if (ret)
		restore_platform(true);
	return ret;
}

/* --- step 3: prepare + early wakeup, no SMC, no Linux suspend ----------- */
static DEFINE_MUTEX(test_lock);

static int prepare_test_fn(void *unused)
{
	ktime_t start = ktime_get();
	int ret = prepare_platform();

	restore_platform(true);	/* no-op when prepare already undid itself */
	test_us = ktime_us_delta(ktime_get(), start);
	return ret;
}

static int prepare_test_set(const char *value, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(value, &on);

	if (ret || !on)
		return ret;
	mutex_lock(&test_lock);
	ret = stop_machine(prepare_test_fn, NULL, cpumask_of(0));
	prepare_tests++;
	last_error = ret;
	pr_info("zrn-system-sleep-bx: prepare test ret=%d restore=%d saved=%u g3d=%x %u us\n",
		ret, last_restore_error, saved_rows, g3d_status, test_us);
	mutex_unlock(&test_lock);
	return ret;
}
static const struct kernel_param_ops prepare_test_ops = { .set = prepare_test_set };
module_param_cb(prepare_test, &prepare_test_ops, NULL, 0200);
MODULE_PARM_DESC(prepare_test, "1: prepare + early-wakeup restore on CPU 0 in stop_machine, no SMC");

/* --- the suspend backend (enable_deep) ---------------------------------- */
static int (*cpu_suspend_p)(unsigned long, int (*)(unsigned long));
static unsigned long resume_pa;

static int sleep_finisher(unsigned long state)
{
	struct arm_smccc_res res;
	int ret = prepare_platform();

	if (ret)
		return ret;
	if (prepare_only) {
		restore_platform(true);
		return -EAGAIN;
	}
	grp4_at_smc = readl_relaxed(mapped_base(BID) + BID_GRP4_PEND);
	arm_smccc_smc(PSCI_CPU_SUSPEND, state, resume_pa, 0, 0, 0, 0, 0, &res);
	last_smc = (int)res.a0;
	return last_smc ? last_smc : -EAGAIN;	/* returned = did not sleep */
}

static int enter_sleep_state(suspend_state_t state)
{
	void __iomem *pmu = mapped_base(PMU);
	int ret;

	if (dry_run) {
		pr_info("zrn-system-sleep-bx: dry run reached enter; nothing touched\n");
		return -EAGAIN;
	}
	if (num_online_cpus() != 1)
		return -EBUSY;
	ret = cpu_pm_enter();
	if (ret)
		return ret;
	ret = cpu_cluster_pm_enter();
	if (ret)
		goto cpu_exit;
	attempts++;
	ret = cpu_suspend_p(PSCI_SYSTEM_SLEEP, sleep_finisher);
	last_error = ret;
	wake_stat = readl_relaxed(pmu + PMU_WAKEUP_STAT);
	wake_stat2 = readl_relaxed(pmu + PMU_WAKEUP2_STAT);
	if (!ret)
		entries++;
	pr_info("zrn-system-sleep-bx: ret=%d smc=%d wake=%08x/%08x grp4=%08x restore=%d\n",
		ret, last_smc, wake_stat, wake_stat2, grp4_at_smc, last_restore_error);
	restore_platform(ret != 0);	/* before CPU PM notifiers touch clocks */
	cpu_cluster_pm_exit();
cpu_exit:
	cpu_pm_exit();
	return ret == -EAGAIN ? -EAGAIN : (ret ? -EIO : 0);
}

static const struct platform_suspend_ops sleep_ops = {
	.valid = suspend_valid_only_mem,
	.enter = enter_sleep_state,
};

/* --- load --------------------------------------------------------------- */
/*
 * No VT switch at suspend: with none registered, pm_prepare_console() moves
 * to VT 63, logind then takes the seat from ztmetro, wlroots drops the DRM
 * master and rebuilds the output on resume, and the new scanout buffers do
 * not fit in CMA (12.11 13:24: DSI at scale 1, touch-sized UI 4x). The
 * switch still happens with console_suspend off (kernel/power/console.c).
 */
static struct platform_device *vt_dev;

static void unmap_all(void)
{
	while (region_count)
		iounmap(regions[--region_count].mapped);
}

static int __init system_sleep_init(void)
{
	unsigned int m, t, i;
	struct zrn_seq *s;
	int ret = 0;

	for (m = 0; m < 2 && !ret; m++)
		for (t = 0; t < 4 && !ret; t++)
			for (i = 0; i < modes[m][t].count && !ret; i++, rows++) {
				s = &modes[m][t].seq[i];
				if (s->offset >= 0x10000 || s->cond_offset >= 0x10000)
					ret = -EINVAL;
				if (!ret)
					ret = map_base(s->base);
				if (!ret)
					ret = map_base(s->cond_base);
			}
	for (i = 0; i < ARRAY_SIZE(gpio_banks) && !ret; i++)
		ret = map_base(gpio_banks[i].base);
	for (i = 0; i < ARRAY_SIZE(keep_regs) && !ret; i++)
		ret = map_base(keep_regs[i].base);
	if (!ret)
		ret = map_base(MCT_BASE);
	if (!ret)
		ret = map_base(WDT_BASE);
	if (!ret)
		ret = map_base(PMU);
	if (!ret)
		ret = map_base(BID);
	if (ret)
		goto fail;
	if (enable_deep) {
		if (!suspend_enter || !resume) {
			pr_err("zrn-system-sleep-bx: enable_deep needs suspend_enter= and resume=\n");
			ret = -EINVAL;
			goto fail;
		}
		cpu_suspend_p = (void *)suspend_enter;
		resume_pa = resume >= 0xffff000000000000UL ? __pa_symbol(resume) : resume;
		vt_dev = platform_device_register_simple("zrn-system-sleep-bx", -1, NULL, 0);
		if (IS_ERR(vt_dev))
			vt_dev = NULL;
		else
			pm_vt_switch_required(&vt_dev->dev, false);
		__module_get(THIS_MODULE);	/* the suspend core keeps our ops */
		suspend_set_ops(&sleep_ops);
	}
	pr_info("zrn-system-sleep-bx: %u rows, %u regions mapped, deep %s, no access%s\n",
		rows, region_count, enable_deep ? "registered (dry run)" : "off",
		vt_dev ? ", no VT switch" : "");
	return 0;
fail:
	pr_err("zrn-system-sleep-bx: load failed %d\n", ret);
	unmap_all();
	return ret;
}

static void __exit system_sleep_exit(void)
{
	if (vt_dev) {
		pm_vt_switch_unregister(&vt_dev->dev);
		platform_device_unregister(vt_dev);
	}
	unmap_all();
}

module_init(system_sleep_init);
module_exit(system_sleep_exit);
MODULE_DESCRIPTION("Exynos 9820 SYS_SLEEP backend (staged, default no access)");
MODULE_LICENSE("GPL");
