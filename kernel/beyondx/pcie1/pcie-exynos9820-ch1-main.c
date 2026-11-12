// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller channel 1 of the Galaxy S10 5G (beyondx, Exynos
 * 9820): the Gen3 x2 root complex in BLK_FSYS0, wired to the S5100 5G modem.
 * Channel 0 (the Wi-Fi) is kernel/beyondx/pcie.
 *
 * Added for OmonullOS, October 2026, on the channel 0 driver, from Samsung's
 * G977N source: pcie-exynos-host-v1.c (the controller, ELBI layout of
 * pcie-exynos-host-v1.h) and pci-host-v1-exynos9820_cal.c (the PHY sequence
 * host-v1 really uses; pci-exynos9820_cal.c's "ch_num == 1" branch is for
 * host-v0 and hangs the SoC here: the PCS stops answering once its PMA table
 * is written). Stock tree
 * pcie1@13120000: ELBI 0x13120000, PMA 0x13150000, PCS 0x13140000, DBI
 * 0x13400000, config window 0x11ffe000, MEM window CPU 0x11000000 -> PCI
 * 0x12d00000, sysreg 0x13020708, GIC SPI 236, PERST# gpf0-2, CLKREQ# gpf0-0
 * (pinctrl 0x13030000), PMU PCIE_PHY_CONTROL + 4.
 *
 * The device tree node comes with the module (pcie1-overlay.dts), so the
 * boot image does not change. The endpoint must be up before the link is
 * started: the S5100 brings its PCIe up only once its bootloader runs (sent
 * over SPI) and AP2CP_WAKEUP is high. So the node is created on the first
 * exynos_pcie_host_v1_poweron() from the modem driver (zrn-modem.ko), or at
 * load with autostart=1 for manual tests (modem/s5100/s5100boot first).
 *
 * The host API the vendor's modem driver calls (exynos_pcie_host_v1_*,
 * exynos_check_pcie_link_status, pcie_iommu_map) is exported here. poweroff
 * takes the link to L2 and powers the PHY down, as the vendor's (the modem's
 * runtime suspend: the S5100 cannot sleep while the link is in L0, ~300 mA);
 * the next poweron trains it again. L1SS and lane change are no-ops.
 * MSI: the DWC iMSI-RX with the vendor's target address 0xd9800000
 * (s5100_msi, reserved), signalled through ELBI IRQ2 on the shared line.
 *
 * Bring-up order mirrors the vendor's exynos_pcie_host_v1_poweron and
 * exynos_pcie_establish_link: PMU PCIE_PHY_CONTROL on -> PHY power-down
 * clear -> PHY config -> bus number enable -> soft power reset -> EQ off
 * -> PERST# deassert, 20 ms -> L1 exit / link-down reset / Q-channel ELBI
 * setup -> (core: setup_rc, iATU) -> PCS force_pclk and cpm delay -> LTSSM
 * enable. host-v1 has no rx elecidle control (an empty stub there).
 */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "pcie-designware.h"
#include "pcie1-host-api.h"

/* ELBI registers (0x13120000), pcie-exynos-host-v1.h */
#define PCIE_IRQ0			0x000
#define  IRQ_INTX_ASSERT(n)		BIT(14 + 2 * (n))	/* INTA..INTD: 14, 16, 18, 20 */
#define  IRQ_RADM_PM_TO_ACK		BIT(29)
#define PCIE_IRQ1			0x004
#define  IRQ_LINK_DOWN_EVT1_1		BIT(10)
#define PCIE_IRQ2			0x008
#define  IRQ_LINK_DOWN			BIT(14)
#define  IRQ_MSI_RISING_ASSERT		BIT(17)
#define PCIE_IRQ0_EN			0x010
#define PCIE_IRQ1_EN			0x014
#define PCIE_IRQ2_EN			0x018
#define PCIE_APP_LTSSM_ENABLE		0x054
#define PCIE_APP_REQ_EXIT_L1		0x06c
#define XMIT_PME_TURNOFF		0x118
#define  PCIE_ELBI_LTSSM_DISABLE	0x0
#define  PCIE_ELBI_LTSSM_ENABLE		0x1
#define PCIE_ELBI_RDLH_LINKUP		0x2c8
#define  LTSSM_STATE(v)			((v) & 0x1f)	/* L0 .. L2: 0x0d .. 0x15 */
#define  LTSSM_LINK_UP_L0		0x91		/* what the vendor waits for */
#define  LTSSM_L23_READY		0x15
#define PCIE_ELB_MAC_RST		0x1400
#define PCIE_ELB_PCS_G_RST		0x1404
#define PCIE_ELB_PCS_CMN_RST		0x1408
#define PCIE_LINKDOWN_RST_CTRL_SEL	0x3a0
#define  PCIE_LINKDOWN_RST_MANUAL	BIT(1)
#define PCIE_SOFT_RESET			0x3a4
#define  SOFT_PWR_RESET			BIT(1)
#define PCIE_QCH_SEL			0x3a8
#define  CLOCK_GATING_ALL		(0xf << 8 | 0xf << 4 | 0xf)
#define PCIE_APP_REQ_EXIT_L1_MODE	0x3bc
#define  APP_REQ_EXIT_L1_MODE		BIT(0)
#define  L1_REQ_NAK_CONTROL_MASTER	BIT(4)
#define PCIE_SW_WAKE			0x3d4
#define  PCIE_BUS_EN			BIT(1)
#define PCIE_STATE_HISTORY_CHECK	0xc00
#define  HISTORY_BUFFER_ENABLE		0x3
#define PCIE_STATE_POWER_S		0xc04
#define PCIE_STATE_POWER_M		0xc08

/* DBI */
#define PCIE_GEN3_RELATED		0x890	/* vendor "EQ Off": 0x12001 */
#define  GEN3_EQ_OFF			0x12001
#define PCIE_AUX_CLK_FREQ		0xb40
#define  PCIE_AUX_CLK_FREQ_26MHZ	0x1a
#define PCIE_L1_SUBSTATES		0xb44
#define  PCIE_L1_SUB_VAL		0xea

/* PMA */
#define PHY_PWRDN			0x400	/* 0x23 all power down, 0 clear */

/* PCS registers (0x13140000) */
#define PCS_CPM_DELAY			0x114
#define PCS_LANE0_EXTRA			0x154	/* "Additional PCS Setting", 0x700d5 */
#define PCS_FORCE_PCLK			0x180
#define PCS_LANE1_EXTRA			0x954

/* PMU */
#define PMU_PCIE1_PHY_CONTROL		0x15860720	/* PCIE_PHY_CONTROL + ch * 4 */
#define  PCIE_PHY_CONTROL_ON		BIT(0)

/* CMU gates that must already be open (cmucal-sfr.c, read-only here) */
#define CMU_TOP_GATE_FSYS0_PCIE		0x1a242074
#define CMU_FSYS0_BASE			0x13000000
#define  CMU_GATE_CG_VAL		BIT(21)
#define  CMU_GATE_MANUAL		BIT(20)

/* Samsung GPIO bank: CON 4 bits, DAT 1 bit, PUD 4 bits, DRV 4 bits per pin */
#define GPIO_CON			0x00
#define GPIO_DAT			0x04
#define GPIO_PUD			0x08
#define GPIO_DRV			0x0c
#define GPIO_GPF0			0x13030000	/* FSYS0 pin controller, gpf0 at +0 */
#define  GPF0_CLKREQ			0
#define  GPF0_PERST			2
#define GPIO_FUNC_OUT			0x1
#define GPIO_FUNC_PCIE_CLKREQ		0x2
#define GPIO_PUD_UP			0x3
#define GPIO_DRV_STOCK			0x3

/* stock dtbo s5100_msi: the MSI target the S5100 firmware expects */
#define S5100_MSI_TARGET		0xd9800000

struct exynos9820_pcie1 {
	struct dw_pcie		pci;
	void __iomem		*phy;		/* PMA: CMN/TRSV registers */
	void __iomem		*pcs;
	void __iomem		*pmu;
	void __iomem		*gpf0;
	struct irq_domain	*intx_domain;
	struct exynos_pcie_register_event *event;
	struct pci_dev		*rp;		/* root port, config saved over poweroff */
	bool			off;		/* link down by poweroff, not by a fault */
};

static struct exynos9820_pcie1 *pcie1;	/* the one channel, for the host API */

#define to_pcie1(x)	container_of(x, struct exynos9820_pcie1, pci)

/*
 * Bring-up trace: trace_ms=N logs every step and waits N ms after it, so a
 * relay (cat /dev/kmsg > /dev/pmsg0 on another CPU) gets the last step into
 * pstore before a bus hang takes the SoC down.
 */
static unsigned int trace_ms;
module_param(trace_ms, uint, 0444);
MODULE_PARM_DESC(trace_ms, "log each bring-up step and wait this long after it");

static void step(struct exynos9820_pcie1 *ep, const char *what)
{
	if (!trace_ms)
		return;
	dev_info(ep->pci.dev, "step: %s\n", what);
	mdelay(trace_ms);
}

/* traced read: logs before the access, so a read that hangs is the last line */
static void probe(struct exynos9820_pcie1 *ep, void __iomem *base, u32 off, const char *what)
{
	if (!trace_ms)
		return;
	dev_info(ep->pci.dev, "step: read %s +%#x\n", what, off);
	mdelay(trace_ms);
	dev_info(ep->pci.dev, "step:   = %08x\n", readl(base + off));
}

static u32 elb_readl(struct exynos9820_pcie1 *ep, u32 reg)
{
	return readl(ep->pci.elbi_base + reg);
}

static void elb_writel(struct exynos9820_pcie1 *ep, u32 val, u32 reg)
{
	writel(val, ep->pci.elbi_base + reg);
}

static void elb_reset_pulse(struct exynos9820_pcie1 *ep, u32 reg, u32 last_us)
{
	elb_writel(ep, 0x1, reg);
	udelay(10);
	elb_writel(ep, 0x0, reg);
	udelay(10);
	elb_writel(ep, 0x1, reg);
	udelay(last_us);
}

static void bank_field(void __iomem *bank, u32 reg, int pin, u32 width, u32 val)
{
	u32 shift = pin * width, mask = GENMASK(width - 1, 0) << shift;
	u32 v = readl(bank + reg);

	writel((v & ~mask) | (val << shift & mask), bank + reg);
}

static void exynos9820_perst(struct exynos9820_pcie1 *ep, bool assert)
{
	bank_field(ep->gpf0, GPIO_DAT, GPF0_PERST, 1, !assert);
}

static void rmw(void __iomem *reg, u32 clear, u32 set)
{
	writel((readl(reg) & ~clear) | set, reg);
}

/* every gate on the path must already be open: touching a gated block hangs the SoC */
static int exynos9820_check_clocks(struct device *dev)
{
	static const u16 gates[] = {
		0x203c, 0x2040, 0x2044, 0x204c, 0x2050, 0x2054, 0x2058, 0x2060,
		0x2064, 0x2068,
	};
	void __iomem *top, *fsys0;
	int i, ret = 0;
	u32 v;

	top = ioremap(CMU_TOP_GATE_FSYS0_PCIE, 4);
	fsys0 = ioremap(CMU_FSYS0_BASE, 0x4000);
	if (!top || !fsys0) {
		ret = -ENOMEM;
		goto out;
	}
	v = readl(top);
	if ((v & CMU_GATE_MANUAL) && !(v & CMU_GATE_CG_VAL)) {
		dev_err(dev, "CMU GATE_CLKCMU_FSYS0_PCIE closed (%08x)\n", v);
		ret = -ENODEV;
	}
	for (i = 0; i < ARRAY_SIZE(gates); i++) {
		v = readl(fsys0 + gates[i]);
		if (!(v & CMU_GATE_CG_VAL)) {
			dev_err(dev, "FSYS0 gate +%#x closed (%08x)\n", gates[i], v);
			ret = -ENODEV;
		}
	}
out:
	if (fsys0)
		iounmap(fsys0);
	if (top)
		iounmap(top);
	return ret;
}

/*
 * Channel 1 PHY configuration, pci-host-v1-exynos9820_cal.c
 * exynos_host_v1_pcie_phy_config: Samsung's PMA values, unchanged; the OTP
 * tuning step is not done.
 */
static void exynos9820_phy_config(struct exynos9820_pcie1 *ep)
{
	static const u16 pma_config[][2] = {
		{ 0x030, 0xb9 }, { 0x044, 0x03 }, { 0x078, 0x2f }, { 0x078, 0x2f },
		{ 0x07c, 0xf8 }, { 0x110, 0x18 }, { 0x208, 0x18 }, { 0x214, 0x1c },
		{ 0x228, 0x56 }, { 0x830, 0xfc }, { 0x838, 0x12 }, { 0x83c, 0xb8 },
		{ 0x874, 0x80 }, { 0x8ec, 0x3a }, { 0x8f8, 0x1e }, { 0x8fc, 0xf0 },
		{ 0x990, 0xe6 }, { 0x99c, 0x44 }, { 0x9a0, 0x23 }, { 0x9a4, 0x5e },
		{ 0xa18, 0x3e }, { 0xb4c, 0x29 }, { 0xb58, 0x28 }, { 0xba0, 0x05 },
		{ 0xba4, 0x55 }, { 0xba8, 0x6a }, { 0x1030, 0xfc }, { 0x1074, 0x80 },
		{ 0x119c, 0x44 }, { 0x11a0, 0x23 }, { 0x11a4, 0x5e }, { 0x134c, 0x25 },
		{ 0x1358, 0x24 }, { 0x13a0, 0x05 }, { 0x13a4, 0x55 }, { 0x13a8, 0x6a },
		{ 0x44c, 0x20 }, { 0x488, 0x20 },
		{ 0x438, 0x01 },	/* PHY power down delay */
		{ 0x450, 0x8a },	/* mask_refclk_out_en */
	};
	int i;

	step(ep, "pcs_g_rst");
	elb_reset_pulse(ep, PCIE_ELB_PCS_G_RST, 10);

	step(ep, "pma config");
	for (i = 0; i < ARRAY_SIZE(pma_config); i++)
		writel(pma_config[i][1], ep->phy + pma_config[i][0]);

	/* PCIE_MAC RST, then PCIE_PHY PCS&PMA(CMN)_RST */
	step(ep, "mac/cmn rst");
	elb_reset_pulse(ep, PCIE_ELB_MAC_RST, 10);
	elb_reset_pulse(ep, PCIE_ELB_PCS_CMN_RST, 100);

	probe(ep, ep->pcs, PCS_LANE0_EXTRA, "pcs");
	step(ep, "pcs extra");
	writel(0x700d5, ep->pcs + PCS_LANE0_EXTRA);
	writel(0x700d5, ep->pcs + PCS_LANE1_EXTRA);
}

static int exynos9820_pcie1_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos9820_pcie1 *ep = to_pcie1(pci);
	u32 val;

	/* PHY out of isolation */
	step(ep, "pmu phy control");
	rmw(ep->pmu, 0, PCIE_PHY_CONTROL_ON);
	probe(ep, ep->phy, PHY_PWRDN, "pma");
	writel(0x0, ep->phy + PHY_PWRDN);	/* phy all power down clear */
	step(ep, "elbi history");

	/* LTSSM history buffer: cheap and makes failures diagnosable */
	elb_writel(ep, 0x0, PCIE_STATE_HISTORY_CHECK);
	elb_writel(ep, HISTORY_BUFFER_ENABLE, PCIE_STATE_HISTORY_CHECK);
	elb_writel(ep, 0x200000, PCIE_STATE_POWER_S);
	elb_writel(ep, 0xffffffff, PCIE_STATE_POWER_M);

	exynos9820_phy_config(ep);

	/* bus number enable */
	step(ep, "bus enable");
	val = elb_readl(ep, PCIE_SW_WAKE);
	elb_writel(ep, val & ~PCIE_BUS_EN, PCIE_SW_WAKE);

	/* soft power reset */
	val = elb_readl(ep, PCIE_SOFT_RESET);
	elb_writel(ep, val & ~SOFT_PWR_RESET, PCIE_SOFT_RESET);
	udelay(20);
	elb_writel(ep, val | SOFT_PWR_RESET, PCIE_SOFT_RESET);

	/* EQ off */
	step(ep, "dbi eq off");
	dw_pcie_writel_dbi(pci, PCIE_GEN3_RELATED, GEN3_EQ_OFF);

	/* PERST# deassert, then 20 ms */
	step(ep, "perst deassert");
	exynos9820_perst(ep, false);
	usleep_range(18000, 20000);

	step(ep, "elbi l1/qch");
	val = elb_readl(ep, PCIE_APP_REQ_EXIT_L1_MODE);
	val |= APP_REQ_EXIT_L1_MODE | L1_REQ_NAK_CONTROL_MASTER;
	elb_writel(ep, val, PCIE_APP_REQ_EXIT_L1_MODE);
	elb_writel(ep, PCIE_LINKDOWN_RST_MANUAL, PCIE_LINKDOWN_RST_CTRL_SEL);
	val = elb_readl(ep, PCIE_QCH_SEL);
	elb_writel(ep, val & ~CLOCK_GATING_ALL, PCIE_QCH_SEL);

	/* vendor exynos_pcie_setup_rc: 26 MHz aux clock, L1.2 durations */
	step(ep, "dbi aux clk");
	dw_pcie_writel_dbi(pci, PCIE_AUX_CLK_FREQ, PCIE_AUX_CLK_FREQ_26MHZ);
	dw_pcie_writel_dbi(pci, PCIE_L1_SUBSTATES, PCIE_L1_SUB_VAL);

	/*
	 * Root port BAR 0 and expansion ROM off (BAR masks through DBI2), as the
	 * vendor's setup_rc: otherwise they take the start of the 15 MB window
	 * and the bridge window for the modem's BARs is never assigned.
	 */
	step(ep, "dbi2 rc bar/rom off");
	dw_pcie_dbi_ro_wr_en(pci);
	dw_pcie_writel_dbi2(pci, PCI_BASE_ADDRESS_0, 0);
	dw_pcie_writel_dbi2(pci, PCI_ROM_ADDRESS1, 0);
	dw_pcie_dbi_ro_wr_dis(pci);

	/* INTx sources only, stale status cleared (vendor exynos_pcie_enable_interrupts) */
	step(ep, "elbi irq");
	elb_writel(ep, elb_readl(ep, PCIE_IRQ0), PCIE_IRQ0);
	elb_writel(ep, elb_readl(ep, PCIE_IRQ1), PCIE_IRQ1);
	elb_writel(ep, elb_readl(ep, PCIE_IRQ2), PCIE_IRQ2);
	elb_writel(ep, IRQ_INTX_ASSERT(0) | IRQ_INTX_ASSERT(1) |
		   IRQ_INTX_ASSERT(2) | IRQ_INTX_ASSERT(3), PCIE_IRQ0_EN);
	elb_writel(ep, 0x0, PCIE_IRQ1_EN);
	elb_writel(ep, IS_ENABLED(CONFIG_PCI_MSI) ? IRQ_MSI_RISING_ASSERT : 0, PCIE_IRQ2_EN);
	step(ep, "host_init done");

	return 0;
}

/*
 * The DWC iMSI-RX, but not on its own line: msi_irq[0] < 0 keeps the core's
 * chained handler off the shared interrupt, ours demuxes IRQ2. The target is
 * the vendor's, which the S5100's outbound window reaches.
 */
static int exynos9820_pcie1_msi_init(struct dw_pcie_rp *pp)
{
	int ret;

	pp->msi_irq[0] = -ENODEV;
	ret = dw_pcie_msi_host_init(pp);
	if (ret)
		return ret;
	pp->use_imsi_rx = true;		/* so setup_rc programs the target and enables */
	pp->msi_data = S5100_MSI_TARGET;
	/* next in the core: version/iATU/eDMA detect, setup_rc (DBI, ATU) */
	step(to_pcie1(to_dw_pcie_from_pp(pp)), "msi_init, core detect + setup_rc next");
	return 0;
}

static void exynos9820_pcie1_notify(struct exynos9820_pcie1 *ep, u32 event)
{
	struct exynos_pcie_register_event *reg = ep->event;

	if (!reg || !(reg->events & event))
		return;
	reg->notify.event = event;
	reg->notify.user = reg->user;
	if (reg->mode == EXYNOS_PCIE_TRIGGER_CALLBACK && reg->callback)
		reg->callback(&reg->notify);
	else if (reg->completion)
		complete(reg->completion);
}

static const struct dw_pcie_host_ops exynos9820_pcie1_host_ops = {
	.init = exynos9820_pcie1_host_init,
	.msi_init = exynos9820_pcie1_msi_init,
};

/* dw_handle_msi_irq for the 32 vectors of controller 0 (not exported to modules) */
static void exynos9820_pcie1_handle_msi(struct exynos9820_pcie1 *ep)
{
	unsigned long status, pos;

	status = dw_pcie_readl_dbi(&ep->pci, PCIE_MSI_INTR0_STATUS);
	for_each_set_bit(pos, &status, MAX_MSI_IRQS_PER_CTRL)
		generic_handle_demux_domain_irq(ep->pci.pp.irq_domain, pos);
}

static void exynos9820_pcie1_irq_handler(struct irq_desc *desc)
{
	struct exynos9820_pcie1 *ep = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 val;
	int i;

	chained_irq_enter(chip, desc);

	val = elb_readl(ep, PCIE_IRQ1);
	elb_writel(ep, val, PCIE_IRQ1);
	if ((val & IRQ_LINK_DOWN_EVT1_1) && !ep->off)
		exynos9820_pcie1_notify(ep, EXYNOS_PCIE_EVENT_LINKDOWN);
	val = elb_readl(ep, PCIE_IRQ2);
	elb_writel(ep, val, PCIE_IRQ2);
	if ((val & IRQ_LINK_DOWN) && !ep->off)
		exynos9820_pcie1_notify(ep, EXYNOS_PCIE_EVENT_LINKDOWN);
	if (IS_ENABLED(CONFIG_PCI_MSI) && (val & IRQ_MSI_RISING_ASSERT)) {
		exynos9820_pcie1_handle_msi(ep);
		/* mask and unmask, so a vector raised meanwhile pends again (vendor) */
		dw_pcie_writel_dbi(&ep->pci, PCIE_MSI_INTR0_MASK, ~0);
		dw_pcie_writel_dbi(&ep->pci, PCIE_MSI_INTR0_MASK, ep->pci.pp.irq_mask[0]);
	}
	val = elb_readl(ep, PCIE_IRQ0);
	elb_writel(ep, val, PCIE_IRQ0);
	for (i = 0; i < PCI_NUM_INTX; i++)
		if (val & IRQ_INTX_ASSERT(i))
			generic_handle_domain_irq(ep->intx_domain, i);

	chained_irq_exit(chip, desc);
}

static int exynos9820_pcie1_intx_map(struct irq_domain *domain,
				     unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops exynos9820_pcie1_intx_domain_ops = {
	.map = exynos9820_pcie1_intx_map,
	.xlate = irq_domain_xlate_onecell,
};

static int exynos9820_pcie1_init_irq(struct exynos9820_pcie1 *ep,
				     struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *intc;
	int irq;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	intc = of_get_child_by_name(dev->of_node, "legacy-interrupt-controller");
	if (!intc)
		return dev_err_probe(dev, -EINVAL, "no legacy-interrupt-controller node\n");

	ep->intx_domain = irq_domain_create_linear(of_fwnode_handle(intc), PCI_NUM_INTX,
						   &exynos9820_pcie1_intx_domain_ops, ep);
	of_node_put(intc);
	if (!ep->intx_domain)
		return dev_err_probe(dev, -ENOMEM, "failed to create INTx domain\n");

	irq_set_chained_handler_and_data(irq, exynos9820_pcie1_irq_handler, ep);
	return 0;
}

static bool exynos9820_pcie1_link_up(struct dw_pcie *pci)
{
	u32 s = LTSSM_STATE(elb_readl(to_pcie1(pci), PCIE_ELBI_RDLH_LINKUP));

	return s >= 0x0d && s <= 0x15;		/* vendor exynos_pcie_link_up */
}

static int exynos9820_pcie1_start_link(struct dw_pcie *pci)
{
	struct exynos9820_pcie1 *ep = to_pcie1(pci);
	u32 v;
	int ret;

	step(ep, "start_link");

	/* force_pclk_en & cpm_delay */
	writel(0x0c, ep->pcs + PCS_FORCE_PCLK);
	writel(0x18500000, ep->pcs + PCS_CPM_DELAY);

	step(ep, "ltssm enable");
	elb_writel(ep, PCIE_ELBI_LTSSM_ENABLE, PCIE_APP_LTSSM_ENABLE);

	/*
	 * Do not hand a dead link to the core: a config access while the link
	 * is down is answered with a bus error that hangs the SoC.
	 */
	ret = readl_poll_timeout(pci->elbi_base + PCIE_ELBI_RDLH_LINKUP, v,
				 (v & 0xff) == LTSSM_LINK_UP_L0, 1000, 500000);
	if (ret) {
		dev_err(pci->dev, "link did not come up (RDLH_LINKUP=%08x, history %08x)\n",
			v, elb_readl(ep, PCIE_STATE_HISTORY_CHECK));
		elb_writel(ep, PCIE_ELBI_LTSSM_DISABLE, PCIE_APP_LTSSM_ENABLE);
		exynos9820_perst(ep, true);
		return ret;
	}
	dev_info(pci->dev, "link up (RDLH_LINKUP=%08x)\n", v);
	step(ep, "link up, config reads next");
	return 0;
}

static void exynos9820_pcie1_stop_link(struct dw_pcie *pci)
{
	struct exynos9820_pcie1 *ep = to_pcie1(pci);

	elb_writel(ep, PCIE_ELBI_LTSSM_DISABLE, PCIE_APP_LTSSM_ENABLE);
	exynos9820_perst(ep, true);
}

static const struct dw_pcie_ops exynos9820_pcie1_ops = {
	.link_up = exynos9820_pcie1_link_up,
	.start_link = exynos9820_pcie1_start_link,
	.stop_link = exynos9820_pcie1_stop_link,
};

/*
 * Config accesses to the modem while its link is powered down (lspci, sysfs
 * config) stall the bus until the watchdog resets the phone (19.10): answer
 * them like an absent device. The child bus ops are swapped after
 * dw_pcie_host_init, since setup_rc only programs the iATU while
 * bridge->child_ops is the stock one.
 */
static struct pci_ops exynos9820_pcie1_child_ops;
static struct pci_ops *exynos9820_pcie1_dw_child_ops;

static bool exynos9820_pcie1_bus_off(struct pci_bus *bus)
{
	struct dw_pcie_rp *pp = bus->sysdata;

	return READ_ONCE(to_pcie1(to_dw_pcie_from_pp(pp))->off);
}

static int exynos9820_pcie1_rd_other(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 *val)
{
	if (exynos9820_pcie1_bus_off(bus)) {
		PCI_SET_ERROR_RESPONSE(val);
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	return exynos9820_pcie1_dw_child_ops->read(bus, devfn, where, size, val);
}

static int exynos9820_pcie1_wr_other(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 val)
{
	if (exynos9820_pcie1_bus_off(bus))
		return PCIBIOS_DEVICE_NOT_FOUND;
	return exynos9820_pcie1_dw_child_ops->write(bus, devfn, where, size, val);
}

static void exynos9820_pcie1_guard_cfg(struct dw_pcie_rp *pp)
{
	struct pci_bus *child;

	if (!exynos9820_pcie1_dw_child_ops) {
		exynos9820_pcie1_dw_child_ops = pp->bridge->child_ops;
		exynos9820_pcie1_child_ops = *pp->bridge->child_ops;
		exynos9820_pcie1_child_ops.read = exynos9820_pcie1_rd_other;
		exynos9820_pcie1_child_ops.write = exynos9820_pcie1_wr_other;
	}
	list_for_each_entry(child, &pp->bridge->bus->children, node)
		child->ops = &exynos9820_pcie1_child_ops;
}

static int exynos9820_pcie1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos9820_pcie1 *ep;
	struct dw_pcie *pci;
	int ret;

	if (trace_ms)
		dev_info(dev, "step: check clocks\n");
	ret = exynos9820_check_clocks(dev);
	if (ret)
		return ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	pci = &ep->pci;
	pci->dev = dev;
	pci->ops = &exynos9820_pcie1_ops;

	ep->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(ep->phy))
		return PTR_ERR(ep->phy);
	ep->pcs = devm_platform_ioremap_resource_byname(pdev, "pcs");
	if (IS_ERR(ep->pcs))
		return PTR_ERR(ep->pcs);

	/* single registers in shared blocks, not requested */
	ep->pmu = devm_ioremap(dev, PMU_PCIE1_PHY_CONTROL, 4);
	ep->gpf0 = devm_ioremap(dev, GPIO_GPF0, 0x20);
	if (!ep->pmu || !ep->gpf0)
		return -ENOMEM;

	/* PERST# asserted until host_init; CLKREQ# to the RC */
	step(ep, "gpf0 perst/clkreq");
	bank_field(ep->gpf0, GPIO_DAT, GPF0_PERST, 1, 0);
	bank_field(ep->gpf0, GPIO_DRV, GPF0_PERST, 4, GPIO_DRV_STOCK);
	bank_field(ep->gpf0, GPIO_CON, GPF0_PERST, 4, GPIO_FUNC_OUT);
	bank_field(ep->gpf0, GPIO_PUD, GPF0_CLKREQ, 4, GPIO_PUD_UP);
	bank_field(ep->gpf0, GPIO_DRV, GPF0_CLKREQ, 4, GPIO_DRV_STOCK);
	bank_field(ep->gpf0, GPIO_CON, GPF0_CLKREQ, 4, GPIO_FUNC_PCIE_CLKREQ);

	ret = exynos9820_pcie1_init_irq(ep, pdev);
	if (ret)
		return ret;

	step(ep, "dw_pcie_host_init");
	pci->pp.ops = &exynos9820_pcie1_host_ops;
	platform_set_drvdata(pdev, ep);

	ret = dw_pcie_host_init(&pci->pp);
	if (ret) {
		dev_err(dev, "failed to initialize host: %d\n", ret);
		irq_set_chained_handler_and_data(platform_get_irq(pdev, 0), NULL, NULL);
		irq_domain_remove(ep->intx_domain);
		return ret;
	}
	exynos9820_pcie1_guard_cfg(&pci->pp);
	pcie1 = ep;
	return 0;
}

static const struct of_device_id exynos9820_pcie1_of_match[] = {
	{ .compatible = "omonullos,exynos9820-pcie-ch1", },
	{ },
};
MODULE_DEVICE_TABLE(of, exynos9820_pcie1_of_match);

/* no .remove: never rmmod, reboot to try a new build */
static struct platform_driver exynos9820_pcie1_driver = {
	.probe = exynos9820_pcie1_probe,
	.driver = {
		.name = "pcie-exynos9820-ch1",
		.of_match_table = exynos9820_pcie1_of_match,
		.suppress_bind_attrs = true,
	},
};

extern const u8 pcie1_overlay[];
extern const u32 pcie1_overlay_size;

static bool autostart;
module_param(autostart, bool, 0444);
MODULE_PARM_DESC(autostart, "bring the link up at load (S5100 bootloader running, AP2CP_WAKEUP high)");

static DEFINE_MUTEX(pcie1_lock);
static int pcie1_ovcs = -1;

/* overlay + device on first use: the driver binds synchronously and trains the link */
static int exynos9820_pcie1_create(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	int ret;

	if (pcie1_ovcs < 0) {
		ret = of_overlay_fdt_apply(pcie1_overlay, pcie1_overlay_size, &pcie1_ovcs, NULL);
		if (ret) {
			pr_err("pcie-exynos9820-ch1: device tree overlay: %d\n", ret);
			return ret;
		}
	}
	/* the overlay's node is not populated by itself under an existing bus */
	np = of_find_compatible_node(NULL, NULL, "omonullos,exynos9820-pcie-ch1");
	pdev = np ? of_find_device_by_node(np) : NULL;
	if (np && !pdev) {
		of_platform_device_create(np, NULL, NULL);
	} else if (pdev && !pcie1) {
		/*
		 * The link did not train last time (a runtime resume came before
		 * cbd booted the S5100 over SPI, 2026-10-18): probe again.
		 */
		ret = device_reprobe(&pdev->dev);
		if (ret)
			dev_err(&pdev->dev, "reprobe: %d\n", ret);
	}
	if (pdev)
		put_device(&pdev->dev);
	of_node_put(np);
	return pcie1 ? 0 : -ENODEV;
}

/* vendor exynos_pcie_host_v1_send_pme_turn_off: PME_Turn_Off, then wait for L2/L3 Ready */
static void exynos9820_pcie1_pme_turn_off(struct exynos9820_pcie1 *ep)
{
	u32 val;
	int i, retry;

	elb_writel(ep, 0x1, PCIE_APP_REQ_EXIT_L1);
	val = elb_readl(ep, PCIE_APP_REQ_EXIT_L1_MODE);
	val &= ~APP_REQ_EXIT_L1_MODE;
	elb_writel(ep, val | L1_REQ_NAK_CONTROL_MASTER, PCIE_APP_REQ_EXIT_L1_MODE);

	for (retry = 0; retry < 10; retry++) {
		elb_writel(ep, 0x1, XMIT_PME_TURNOFF);
		for (i = 0; i < 2400 && !(elb_readl(ep, PCIE_IRQ0) & IRQ_RADM_PM_TO_ACK); i++)
			udelay(10);
		elb_writel(ep, 0x0, XMIT_PME_TURNOFF);
		for (i = 0; i < 2400; i++) {
			if (LTSSM_STATE(elb_readl(ep, PCIE_ELBI_RDLH_LINKUP)) == LTSSM_L23_READY)
				return;
			udelay(10);
		}
	}
	dev_warn(ep->pci.dev, "no L2/L3 Ready from the endpoint (RDLH_LINKUP=%08x)\n",
		 elb_readl(ep, PCIE_ELBI_RDLH_LINKUP));
}

/* the link and PHY half of the vendor poweroff */
static void exynos9820_pcie1_phy_off(struct exynos9820_pcie1 *ep)
{
	writel(0x0d, ep->pcs + PCS_FORCE_PCLK);	/* force_pclk_en & cpm_delay off */
	elb_writel(ep, 0x0, PCIE_STATE_HISTORY_CHECK);
	exynos9820_perst(ep, true);
	elb_writel(ep, PCIE_ELBI_LTSSM_DISABLE, PCIE_APP_LTSSM_ENABLE);
	writel(0x23, ep->phy + PHY_PWRDN);	/* phy all power down */
}

/* vendor exynos_pcie_host_v1_poweroff, without the clock gates (left open) */
static void exynos9820_pcie1_power_down(struct exynos9820_pcie1 *ep)
{
	if (!ep->rp)
		ep->rp = pci_get_slot(ep->pci.pp.bridge->bus, PCI_DEVFN(0, 0));
	if (ep->rp)
		pci_save_state(ep->rp);

	WRITE_ONCE(ep->off, true);
	exynos9820_pcie1_pme_turn_off(ep);
	exynos9820_pcie1_phy_off(ep);
	dev_info(ep->pci.dev, "link off\n");
}

/*
 * vendor poweron after the first: PHY and link again, then the root port's
 * config. Training now and then fails once (19.10, history 06) and the modem
 * stays dead for the boot, so power the PHY down and try again.
 */
static int exynos9820_pcie1_power_up(struct exynos9820_pcie1 *ep)
{
	int ret, try;

	elb_writel(ep, 0x0, PCIE_APP_REQ_EXIT_L1);
	for (try = 1; ; try++) {
		exynos9820_pcie1_host_init(&ep->pci.pp);
		dw_pcie_setup_rc(&ep->pci.pp);
		ret = exynos9820_pcie1_start_link(&ep->pci);
		if (!ret || try == 3)
			break;
		exynos9820_pcie1_phy_off(ep);
		msleep(50);
		dev_warn(ep->pci.dev, "link training retry %d\n", try + 1);
	}
	if (ret)
		return ret;
	if (ep->rp)
		pci_restore_state(ep->rp);
	ep->off = false;
	return 0;
}

/*
 * Vendor host API for the modem driver. Only channel 1 exists here. The
 * first poweron creates the controller and trains the link; poweroff (the
 * modem's runtime suspend) takes it down, the next poweron trains it again.
 */
int exynos_check_pcie_link_status(int ch_num)
{
	return pcie1 && exynos9820_pcie1_link_up(&pcie1->pci);
}
EXPORT_SYMBOL(exynos_check_pcie_link_status);

int exynos_pcie_host_v1_poweron(int ch_num)
{
	int ret = 0;

	if (ch_num != 1)
		return -ENODEV;
	mutex_lock(&pcie1_lock);
	if (!pcie1)
		ret = exynos9820_pcie1_create();
	else if (pcie1->off)
		ret = exynos9820_pcie1_power_up(pcie1);
	if (!ret && !exynos_check_pcie_link_status(ch_num))
		ret = -ENOLINK;
	mutex_unlock(&pcie1_lock);
	return ret;
}
EXPORT_SYMBOL(exynos_pcie_host_v1_poweron);

static bool link_pm = true;
module_param(link_pm, bool, 0644);
MODULE_PARM_DESC(link_pm, "power the link down on the modem's runtime suspend (0: keep it in L0)");

int exynos_pcie_host_v1_poweroff(int ch_num)
{
	if (ch_num != 1)
		return -ENODEV;
	mutex_lock(&pcie1_lock);
	if (link_pm && pcie1 && !pcie1->off && exynos9820_pcie1_link_up(&pcie1->pci))
		exynos9820_pcie1_power_down(pcie1);
	mutex_unlock(&pcie1_lock);
	return 0;
}
EXPORT_SYMBOL(exynos_pcie_host_v1_poweroff);

int exynos_pcie_host_v1_register_event(struct exynos_pcie_register_event *reg)
{
	if (!pcie1 || !reg)
		return -ENODEV;
	pcie1->event = reg;
	return 0;
}
EXPORT_SYMBOL(exynos_pcie_host_v1_register_event);

int exynos_pcie_host_v1_deregister_event(struct exynos_pcie_register_event *reg)
{
	if (pcie1 && pcie1->event == reg)
		pcie1->event = NULL;
	return 0;
}
EXPORT_SYMBOL(exynos_pcie_host_v1_deregister_event);

int exynos_pcie_host_v1_l1ss_ctrl(int enable, int id)
{
	return 0;
}
EXPORT_SYMBOL(exynos_pcie_host_v1_l1ss_ctrl);

int exynos_pcie_host_v1_lanechange(int ch_num, int lane)
{
	return 0;
}
EXPORT_SYMBOL(exynos_pcie_host_v1_lanechange);

/* no SysMMU on this path: the S5100 sees AP physical addresses */
int pcie_iommu_map(int ch_num, unsigned long iova, phys_addr_t paddr, size_t size, int prot)
{
	return 0;
}
EXPORT_SYMBOL(pcie_iommu_map);

void exynos_pcie_host_v1_register_dump(int ch_num)
{
	struct exynos9820_pcie1 *ep = pcie1;

	if (!ep)
		return;
	dev_info(ep->pci.dev, "RDLH_LINKUP %08x IRQ0 %08x IRQ1 %08x IRQ2 %08x history %08x\n",
		 elb_readl(ep, PCIE_ELBI_RDLH_LINKUP), elb_readl(ep, PCIE_IRQ0),
		 elb_readl(ep, PCIE_IRQ1), elb_readl(ep, PCIE_IRQ2),
		 elb_readl(ep, PCIE_STATE_HISTORY_CHECK));
}
EXPORT_SYMBOL(exynos_pcie_host_v1_register_dump);

static int __init exynos9820_pcie1_init(void)
{
	int ret;

	ret = platform_driver_register(&exynos9820_pcie1_driver);
	if (ret || !autostart)
		return ret;
	mutex_lock(&pcie1_lock);
	ret = exynos9820_pcie1_create();
	mutex_unlock(&pcie1_lock);
	if (ret)
		pr_err("pcie-exynos9820-ch1: autostart: %d\n", ret);
	return 0;
}
module_init(exynos9820_pcie1_init);

MODULE_DESCRIPTION("PCIe host for the Galaxy S10 5G's 5G modem (Exynos 9820 channel 1)");
MODULE_LICENSE("GPL v2");
