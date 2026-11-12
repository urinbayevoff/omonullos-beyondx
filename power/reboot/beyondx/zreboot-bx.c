// SPDX-License-Identifier: GPL-2.0
/*
 * zreboot-bx: a restart handler for the Exynos 9820 (Galaxy S10 5G) under
 * mainline, which has none: `reboot` printed "reboot failed" and the phone
 * waited for uniLoader's watchdog (~2.5 min) instead.
 *
 * Added for OmonullOS, October 2026. As Samsung's G977N kernel does it
 * (drivers/samsung/sec_reboot.c, drivers/soc/samsung/exynos-reboot.c):
 *
 *   PMU 0x15860000
 *   INFORM2 0x0808  bits 23:16 = 0x4e (SEC_REBOOT_NORMAL: no LPM charging)
 *   INFORM3 0x080c  where S-Boot goes next: 0x12345670 boot, ...74
 *                   recovery, ...71 download (reboot download|bootloader)
 *   RST_STAT 0x0404 cleared, then SWRESET 0x3a00 bit 1
 *
 * uniLoader leaves INFORM3 at recovery on every boot (kernel/make-dts.py,
 * the failsafe: a hang ends in TWRP). That stays so: INFORM3 is written
 * only here, on the way down. ACPM is not told (exynos_acpm_reboot); the
 * PS_HOLD power off does without it as well.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/string.h>

#define PMU_BASE	0x15860000
#define PMU_SIZE	0x4000
#define RST_STAT	0x0404
#define INFORM2		0x0808
#define INFORM3		0x080c
#define SWRESET		0x3a00
#define SWRESET_TRIGGER	(1 << 1)

#define REASON_BOOT	0x12345670
#define REASON_DOWNLOAD	0x12345671
#define REASON_RECOVERY	0x12345674
#define SEC_REBOOT_NORMAL 0x4e

static void __iomem *pmu;

static int zreboot_restart(struct notifier_block *nb, unsigned long mode, void *data)
{
	const char *cmd = data;
	u32 reason = REASON_BOOT, v;

	if (cmd && !strncmp(cmd, "recovery", 8))
		reason = REASON_RECOVERY;
	else if (cmd && (!strcmp(cmd, "download") || !strcmp(cmd, "bootloader")))
		reason = REASON_DOWNLOAD;
	pr_emerg("zreboot-bx: %s, INFORM3 %#x, SWRESET\n", cmd ? cmd : "(null)", reason);

	v = readl(pmu + INFORM2);
	writel((v & ~(0xff << 16)) | (SEC_REBOOT_NORMAL << 16), pmu + INFORM2);
	writel(reason, pmu + INFORM3);
	writel(0, pmu + RST_STAT);
	writel(SWRESET_TRIGGER, pmu + SWRESET);
	mdelay(1000);
	pr_emerg("zreboot-bx: SWRESET did not reset\n");
	return NOTIFY_DONE;
}

static struct notifier_block zreboot_nb = {
	.notifier_call = zreboot_restart,
	.priority = 192,	/* before anything generic (128) */
};

static int __init zreboot_init(void)
{
	int ret;

	pmu = ioremap(PMU_BASE, PMU_SIZE);
	if (!pmu)
		return -ENOMEM;
	ret = register_restart_handler(&zreboot_nb);
	if (ret) {
		iounmap(pmu);
		return ret;
	}
	pr_info("zreboot-bx: restart handler on, INFORM3 now %#x\n", readl(pmu + INFORM3));
	return 0;
}

static void __exit zreboot_exit(void)
{
	unregister_restart_handler(&zreboot_nb);
	iounmap(pmu);
}

module_init(zreboot_init);
module_exit(zreboot_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Exynos 9820 restart through PMU SWRESET, Samsung's reboot reasons");
