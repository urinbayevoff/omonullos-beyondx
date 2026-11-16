// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe host controller for the Wi-Fi module of the Galaxy S10 5G (beyondx,
 * Exynos 9820): channel 0, the Gen2 root complex in BLK_FSYS1, wired to the
 * BCM4375. Channel 1 (FSYS0, Gen3 x2) goes to the S5100 5G modem and is not
 * described here.
 *
 * Added for OmonullOS, October 2026, from the S9+ port's pcie-exynos9810.c
 * (third_party/s9plus/src/pci, SheroAbi/galaxy-s9plus-mainline-linux). The
 * 9820's channel 0 is the same IP with the same PHY: Samsung's
 * pci-exynos9820_cal.c channel 0 tables are byte for byte those of
 * pci-exynos9810_cal.c, and pcie-exynos-host-v0.c drives both SoCs. What is
 * different here (stock tree pcie0@13ED0000, Samsung G977N source):
 *
 *  - addresses: ELBI 0x13ed0000, PMA 0x13ef0000, PCS 0x13ee0000, DBI
 *    0x13f00000, config window 0x14ffe000, 16 MB MEM window at 0x14000000,
 *    FSYS1 sysreg 0x13c10000, GIC SPI 253 (as on the 9810);
 *  - the link: the stock tree says max-link-speed 1 (Gen1) for this port;
 *  - the clocks: BLK_FSYS1 also holds the UFS, so its power domain is up
 *    and every PCIe gate there reads CG_VAL=1 with the Q-channel off
 *    (read on bx28: 0x13c02000/30..48 = 0x00200000, 0x13c03034..48 =
 *    0x003f0002, CMU top GATE_CLKCMU_FSYS1_PCIE 0x1a242078 = 0x00200000).
 *    Nothing is written to the CMU; the gates are checked and probe stops
 *    if one is closed, since touching a gated block hangs the SoC;
 *  - the pins: mainline has no pin controller data for the 9820's FSYS1 and
 *    PERIC0 banks, so the four pins are set through their bank registers
 *    the way the stock pinctrl "default" state sets them: gpf2-0 CLKREQ#
 *    (function 2, pull-up), gpf2-2 PERST# (output), gpg0-5 WL_REG_ON
 *    (output); gpa0-7 (WLAN_HOST_WAKE) is left alone, brcmfmac does not
 *    use it on PCIe. Nothing else in these banks is owned by a driver.
 *
 * Bring-up order mirrors the vendor driver, as the S9+ driver does:
 *   WL_REG_ON high, 200 ms -> PMU PCIE_PHY_CONTROL on -> sysreg snoop bits
 *   sharable -> PHY power-down clear -> soft core reset -> PERST# deassert
 *   -> L1/QCH ELBI setup -> PHY CAL + reset pulses -> (core: setup_rc,
 *   iATU) -> LTSSM enable.
 *
 * The interrupt aggregator (IA) Samsung programs for the CDR reset on L1.2
 * exits is not used, like on the S9+. Legacy INTx only: Samsung's dhd
 * excludes the 4375 from MSI (dhdpcie_chip_support_msi) and the stock tree
 * runs the port with use-msi = "false".
 */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "pcie-designware.h"

/* ELBI registers (0x13ed0000) */
#define PCIE_IRQ_PULSE			0x000	/* latched pulse sources, W1C */
#define  IRQ_INTX_ASSERT(n)		BIT(2 * (n))
#define  IRQ_INTX_ASSERT_ALL		(IRQ_INTX_ASSERT(0) | IRQ_INTX_ASSERT(1) | \
					 IRQ_INTX_ASSERT(2) | IRQ_INTX_ASSERT(3))
#define PCIE_IRQ_LEVEL			0x004
#define PCIE_IRQ_SPECIAL		0x008
#define PCIE_IRQ_EN_PULSE		0x00c
#define PCIE_IRQ_EN_LEVEL		0x010
#define PCIE_IRQ_EN_SPECIAL		0x014
#define PCIE_SW_WAKE			0x018
#define  PCIE_BUS_EN			BIT(1)
#define PCIE_APP_LTSSM_ENABLE		0x02c
#define  PCIE_ELBI_LTSSM_DISABLE	0x0
#define  PCIE_ELBI_LTSSM_ENABLE		0x1
#define PCIE_ELBI_RDLH_LINKUP		0x074
#define  PCIE_ELBI_XMLH_LINKUP		BIT(4)
#define PCIE_APP_REQ_EXIT_L1_MODE	0x0f4
#define  APP_REQ_EXIT_L1_MODE_HW	BIT(0)
#define  L1_REQ_NAK_CONTROL_MASTER	BIT(4)
#define PCIE_LINKDOWN_RST_CTRL_SEL	0x1b8
#define  PCIE_LINKDOWN_RST_MANUAL	BIT(1)
#define PCIE_SOFT_CORE_RESET		0x1d0
#define PCIE_STATE_HISTORY_CHECK	0x274
#define  HISTORY_BUFFER_ENABLE		0x3
#define PCIE_STATE_POWER_S		0x2bc
#define PCIE_STATE_POWER_M		0x2c0
#define PCIE_QCH_SEL			0x2c8
#define  CLOCK_GATING_ALL		(0xf << 8 | 0xf << 4 | 0xf)
#define PCIE_ELB_PCS_G_RST		0x288
#define PCIE_ELB_PCS_CMN_RST		0x28c
#define PCIE_ELB_MAC_RST		0x290

/* DBI: the controller's own auxiliary (26 MHz) clock frequency, for PM timers */
#define PCIE_AUX_CLK_FREQ		0xb40
#define  PCIE_AUX_CLK_FREQ_26MHZ	0x1a

/* PCS registers (0x13ee0000) */
#define PCS_PRGM_TIMEOUT_L1SS		0x0c
#define PCS_CDR_RESET			0xd0
#define  PCS_CDR_RESET_MASK		(0x3 << 6)
#define PCS_RX_ELECIDLE			0xec
#define  PCS_RX_ELECIDLE_IGNORE		BIT(3)
#define PCS_TX_LATENCY			0xf8
#define PCS_REFCLK_OUT_CTRL		0x100
#define  PCS_PWRDN_A			BIT(6)
#define PCS_REFCLK_OUT_CTRL2		0x104
#define  PCS_PWRDN_B			BIT(7)

/* FSYS1 sysreg (syscon at 0x13c10000) */
#define FSYS1_PCIE0_PHY_CTRL		0x1044	/* vendor "sysreg" resource 0x13c11044 + 0 */
#define  FSYS1_PCIE0_PCS_HIGH_SPEED	BIT(1)
#define FSYS1_PCIE0_LANE_CTRL		0x1050	/* + 0xc, vendor "PCIE_WIFI0_PCIE_PHY_CONTROL" */
#define  FSYS1_PCIE0_LANE0_ENABLE	BIT(12)
#define FSYS1_PCIE0_SNOOP_CTRL		0x700	/* PCIE_SYSREG_SHARABILITY_CTRL */
#define  FSYS1_PCIE0_SNOOP_SHARABLE	(0x3 << 8)

/* PMU */
#define PCIE_PHY_CONTROL		0x71c	/* channel 0 */
#define  PCIE_PHY_CONTROL_ON		BIT(0)

/* CMU gates that must already be open (cmucal-sfr.c, read-only here) */
#define CMU_TOP_BASE			0x1a240000
#define  CMU_TOP_GATE_FSYS1_PCIE	0x2078
#define CMU_FSYS1_BASE			0x13c00000
#define  CMU_GATE_CG_VAL		BIT(21)
#define  CMU_GATE_MANUAL		BIT(20)
#define  CMU_QCH_ENABLE			BIT(0)

/* Samsung GPIO bank: CON 4 bits, DAT 1 bit, PUD 4 bits, DRV 4 bits per pin */
#define GPIO_CON			0x00
#define GPIO_DAT			0x04
#define GPIO_PUD			0x08
#define GPIO_DRV			0x0c
#define GPIO_FSYS1_BASE			0x13c40000
#define  GPF2				0x020
#define  GPF2_CLKREQ			0
#define  GPF2_PERST			2
#define GPIO_PERIC0_BASE		0x10430000
#define  GPG0				0x080
#define  GPG0_WL_REG_ON			5
#define GPIO_FUNC_OUT			0x1
#define GPIO_FUNC_PCIE_CLKREQ		0x2
#define GPIO_PUD_UP			0x3
#define GPIO_DRV_STOCK			0x3	/* samsung,pin-drv = <3> in the stock tree */

struct exynos9820_pcie {
	struct dw_pcie		pci;
	void __iomem		*phy;		/* PMA: CMN/TRSV registers */
	void __iomem		*pcs;		/* PCS */
	struct regmap		*pmu;		/* PMU, PCIE_PHY_CONTROL */
	struct regmap		*sysreg;	/* FSYS1 sysreg */
	void __iomem		*gpf2;		/* CLKREQ#, PERST# */
	void __iomem		*gpg0;		/* WL_REG_ON */
	struct irq_domain	*intx_domain;
};

#define to_exynos9820_pcie(x)	container_of(x, struct exynos9820_pcie, pci)

static u32 elb_readl(struct exynos9820_pcie *ep, u32 reg)
{
	return readl(ep->pci.elbi_base + reg);
}

static void elb_writel(struct exynos9820_pcie *ep, u32 val, u32 reg)
{
	writel(val, ep->pci.elbi_base + reg);
}

/* 1 -> 0 -> 1 on an ELBI reset line; vendor pulses them with ~10-100 us */
static void elb_reset_pulse(struct exynos9820_pcie *ep, u32 reg, u32 last_us)
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

static void pin_out(void __iomem *bank, int pin, int value)
{
	bank_field(bank, GPIO_DAT, pin, 1, value);
	bank_field(bank, GPIO_DRV, pin, 4, GPIO_DRV_STOCK);
	bank_field(bank, GPIO_CON, pin, 4, GPIO_FUNC_OUT);
}

/* PERST# is active low: asserted = 0 on the pin */
static void exynos9820_perst(struct exynos9820_pcie *ep, bool assert)
{
	bank_field(ep->gpf2, GPIO_DAT, GPF2_PERST, 1, !assert);
}

/*
 * Every gate on the path must already be open: probe reads them and gives
 * up rather than touch a gated block (hangs the SoC, CLAUDE.md rule 6).
 */
static int exynos9820_check_clocks(struct device *dev)
{
	static const u16 gates[] = {
		0x2000, 0x2030, 0x2034, 0x2038, 0x203c, 0x2040, 0x2044, 0x2048,
	};
	static const u16 qchs[] = { 0x3034, 0x3038, 0x303c, 0x3040, 0x3044, 0x3048 };
	void __iomem *top, *fsys1;
	int i, ret = 0;
	u32 v;

	top = ioremap(CMU_TOP_BASE + CMU_TOP_GATE_FSYS1_PCIE, 4);
	fsys1 = ioremap(CMU_FSYS1_BASE, 0x4000);
	if (!top || !fsys1) {
		ret = -ENOMEM;
		goto out;
	}

	v = readl(top);
	if ((v & CMU_GATE_MANUAL) && !(v & CMU_GATE_CG_VAL)) {
		dev_err(dev, "CMU GATE_CLKCMU_FSYS1_PCIE closed (%08x)\n", v);
		ret = -ENODEV;
	}
	for (i = 0; i < ARRAY_SIZE(gates); i++) {
		v = readl(fsys1 + gates[i]);
		if (!(v & CMU_GATE_CG_VAL)) {
			dev_err(dev, "FSYS1 gate +%#x closed (%08x)\n", gates[i], v);
			ret = -ENODEV;
		}
	}
	for (i = 0; i < ARRAY_SIZE(qchs); i++) {
		v = readl(fsys1 + qchs[i]);
		if (v & CMU_QCH_ENABLE)
			dev_warn(dev, "FSYS1 Q-channel +%#x enabled (%08x)\n", qchs[i], v);
	}
out:
	if (fsys1)
		iounmap(fsys1);
	if (top)
		iounmap(top);
	return ret;
}

/*
 * Undo a PHY power-down a previous kernel may have left behind, channel 0
 * part (vendor exynos_phy_all_pwrdn_clear).
 */
static void exynos9820_phy_all_pwrdn_clear(struct exynos9820_pcie *ep)
{
	u32 val;

	val = readl(ep->pcs + PCS_REFCLK_OUT_CTRL);
	val &= ~PCS_PWRDN_A;
	writel(val, ep->pcs + PCS_REFCLK_OUT_CTRL);

	val = readl(ep->pcs + PCS_REFCLK_OUT_CTRL2);
	val &= ~PCS_PWRDN_B;
	writel(val, ep->pcs + PCS_REFCLK_OUT_CTRL2);

	writel(0xc0, ep->phy + (0x20 * 4));	/* common block */
	writel(0x7e, ep->phy + (0x57 * 4));	/* trsv */
}

/*
 * Channel 0 PHY configuration, translated from pci-exynos9820_cal.c
 * (exynos_pcie_phy_config). 26 MHz reference, one lane. The table values
 * are Samsung's, unchanged (and the same as the 9810's).
 */
static void exynos9820_phy_config(struct exynos9820_pcie *ep)
{
	static const u32 cmn_config_val[48] = {
		0x01, 0xE1, 0x05, 0x00, 0x88, 0x88, 0x88, 0x0C,
		0x61, 0x45, 0x65, 0x24, 0x33, 0x18, 0xE3, 0xFC,
		0xD8, 0x05, 0xE6, 0x80, 0x00, 0x00, 0x00, 0x00,
		0x60, 0x11, 0x00, 0xA0, 0x05, 0x04, 0x18, 0x88,
		0xC0, 0xFF, 0x9B, 0x52, 0x22, 0x30, 0x4F, 0xDC,
		0x40, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x80,
	}; /* Ch0 HS1 Gen2 */
	static const u32 trsv_config_val[80] = {
		0x31, 0x40, 0x37, 0x99, 0x85, 0x00, 0xC0, 0xFF,
		0xFF, 0x3F, 0x8C, 0xC8, 0x02, 0x01, 0x88, 0x80,
		0x06, 0x90, 0x6C, 0x66, 0x09, 0x61, 0x42, 0x44,
		0xC6, 0x50, 0x0A, 0x33, 0x58, 0xE7, 0x20, 0x22,
		0x80, 0x38, 0x05, 0x85, 0x00, 0x00, 0x00, 0x7E,
		0x00, 0x00, 0x55, 0x15, 0xAC, 0xAA, 0x3E, 0x00,
		0x00, 0x00, 0x20, 0x3F, 0x00, 0x03, 0x01, 0x00,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40,
		0x05, 0x85, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
	}; /* Ch0 Gen2 */
	u32 val;
	int i;

	/* Set PCS_HIGH_SPEED, then the lane source and lane0_enable */
	regmap_update_bits(ep->sysreg, FSYS1_PCIE0_PHY_CTRL,
			   FSYS1_PCIE0_PCS_HIGH_SPEED,
			   FSYS1_PCIE0_PCS_HIGH_SPEED);
	/* clear 0xf<<4 and 0xf<<2, set 0x3<<2, clear bit 1, set lane0 bit 12 */
	regmap_update_bits(ep->sysreg, FSYS1_PCIE0_LANE_CTRL,
			   0xf << 4 | 0xf << 2 | FSYS1_PCIE0_PCS_HIGH_SPEED,
			   0x3 << 2);
	regmap_update_bits(ep->sysreg, FSYS1_PCIE0_LANE_CTRL,
			   FSYS1_PCIE0_LANE0_ENABLE,
			   FSYS1_PCIE0_LANE0_ENABLE);

	/* pcs_g_rst */
	elb_reset_pulse(ep, PCIE_ELB_PCS_G_RST, 10);

	/* PHY common block setting */
	for (i = 0; i < ARRAY_SIZE(cmn_config_val); i++)
		writel(cmn_config_val[i], ep->phy + (i * 4));

	/* PHY transceiver/receiver block setting */
	for (i = 0; i < ARRAY_SIZE(trsv_config_val); i++)
		writel(trsv_config_val[i], ep->phy + ((0x30 + i) * 4));

	/* tx latency, pcs refclk out control, PRGM_TIMEOUT_L1SS_VAL */
	writel(0x70, ep->pcs + PCS_TX_LATENCY);
	writel(0x87, ep->pcs + PCS_REFCLK_OUT_CTRL);
	writel(0x50, ep->pcs + PCS_REFCLK_OUT_CTRL2);
	val = readl(ep->pcs + PCS_PRGM_TIMEOUT_L1SS);
	val &= ~BIT(1);
	val |= BIT(4);
	writel(val, ep->pcs + PCS_PRGM_TIMEOUT_L1SS);

	/* PCIE_MAC RST */
	elb_reset_pulse(ep, PCIE_ELB_MAC_RST, 10);

	/* PCIE_PHY PCS&PMA(CMN)_RST */
	elb_reset_pulse(ep, PCIE_ELB_PCS_CMN_RST, 100);

	/* CDR reset: 0b11 -> 0b10 -> 0b00 in bits [7:6], 20 us steps */
	val = readl(ep->pcs + PCS_CDR_RESET);
	val |= PCS_CDR_RESET_MASK;
	writel(val, ep->pcs + PCS_CDR_RESET);
	udelay(20);
	val = readl(ep->pcs + PCS_CDR_RESET);
	val &= ~PCS_CDR_RESET_MASK;
	val |= 0x2 << 6;
	writel(val, ep->pcs + PCS_CDR_RESET);
	val &= ~PCS_CDR_RESET_MASK;
	writel(val, ep->pcs + PCS_CDR_RESET);
}

static void exynos9820_rx_elecidle(struct exynos9820_pcie *ep, bool ignore)
{
	u32 val = readl(ep->pcs + PCS_RX_ELECIDLE);

	val &= ~PCS_RX_ELECIDLE_IGNORE;
	if (ignore)
		val |= PCS_RX_ELECIDLE_IGNORE;
	writel(val, ep->pcs + PCS_RX_ELECIDLE);
}

static int exynos9820_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct exynos9820_pcie *ep = to_exynos9820_pcie(pci);
	u32 val;

	/* PHY out of isolation (read 0 on bx28: S-Boot leaves it off) */
	regmap_update_bits(ep->pmu, PCIE_PHY_CONTROL,
			   PCIE_PHY_CONTROL_ON, PCIE_PHY_CONTROL_ON);

	/* stock beyondx overlay: use-cache-coherency = "true" -> sharable */
	regmap_update_bits(ep->sysreg, FSYS1_PCIE0_SNOOP_CTRL,
			   FSYS1_PCIE0_SNOOP_SHARABLE,
			   FSYS1_PCIE0_SNOOP_SHARABLE);

	exynos9820_phy_all_pwrdn_clear(ep);

	/* avoid checking rx elecidle while the DBI is being programmed */
	exynos9820_rx_elecidle(ep, true);

	/* soft core reset */
	elb_writel(ep, 0x0, PCIE_SOFT_CORE_RESET);
	udelay(20);
	elb_writel(ep, 0x1, PCIE_SOFT_CORE_RESET);

	/* PERST# deassert, then 20 ms for the endpoint's own power-up */
	exynos9820_perst(ep, false);
	usleep_range(18000, 20000);

	/* APP_REQ_EXIT_L1 in H/W mode, L1 NAK control master */
	val = elb_readl(ep, PCIE_APP_REQ_EXIT_L1_MODE);
	val |= APP_REQ_EXIT_L1_MODE_HW | L1_REQ_NAK_CONTROL_MASTER;
	elb_writel(ep, val, PCIE_APP_REQ_EXIT_L1_MODE);

	/* link-down reset through the manual path, no clock gating */
	elb_writel(ep, PCIE_LINKDOWN_RST_MANUAL, PCIE_LINKDOWN_RST_CTRL_SEL);
	val = elb_readl(ep, PCIE_QCH_SEL);
	val &= ~CLOCK_GATING_ALL;
	elb_writel(ep, val, PCIE_QCH_SEL);

	/* LTSSM history buffer: cheap and makes failures diagnosable */
	elb_writel(ep, 0x0, PCIE_STATE_HISTORY_CHECK);
	elb_writel(ep, HISTORY_BUFFER_ENABLE, PCIE_STATE_HISTORY_CHECK);
	elb_writel(ep, 0x200000, PCIE_STATE_POWER_S);
	elb_writel(ep, 0xffffffff, PCIE_STATE_POWER_M);

	exynos9820_phy_config(ep);

	/* bus number enable */
	val = elb_readl(ep, PCIE_SW_WAKE);
	val &= ~PCIE_BUS_EN;
	elb_writel(ep, val, PCIE_SW_WAKE);

	/* the controller's own 26 MHz auxiliary clock, for the PM timers */
	dw_pcie_writel_dbi(pci, PCIE_AUX_CLK_FREQ, PCIE_AUX_CLK_FREQ_26MHZ);

	/*
	 * Interrupt sources: the four INTx assertion pulses and nothing
	 * else (vendor exynos_pcie_enable_interrupts). Stale status from a
	 * previous kernel is cleared first.
	 */
	elb_writel(ep, elb_readl(ep, PCIE_IRQ_PULSE), PCIE_IRQ_PULSE);
	elb_writel(ep, elb_readl(ep, PCIE_IRQ_LEVEL), PCIE_IRQ_LEVEL);
	elb_writel(ep, elb_readl(ep, PCIE_IRQ_SPECIAL), PCIE_IRQ_SPECIAL);
	elb_writel(ep, IRQ_INTX_ASSERT_ALL, PCIE_IRQ_EN_PULSE);
	elb_writel(ep, 0x0, PCIE_IRQ_EN_LEVEL);
	elb_writel(ep, 0x0, PCIE_IRQ_EN_SPECIAL);

	return 0;
}

/*
 * No MSI controller on this port (see the header comment). Providing the
 * hook keeps the DWC core from claiming the interrupt line for its iMSI-RX
 * chained handler; the line is ours, for INTx.
 */
static int exynos9820_pcie_msi_init(struct dw_pcie_rp *pp)
{
	return 0;
}

static const struct dw_pcie_host_ops exynos9820_pcie_host_ops = {
	.init = exynos9820_pcie_host_init,
	.msi_init = exynos9820_pcie_msi_init,
};

static void exynos9820_pcie_irq_handler(struct irq_desc *desc)
{
	struct exynos9820_pcie *ep = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 val;
	int i;

	chained_irq_enter(chip, desc);

	val = elb_readl(ep, PCIE_IRQ_PULSE);
	elb_writel(ep, val, PCIE_IRQ_PULSE);
	for (i = 0; i < PCI_NUM_INTX; i++)
		if (val & IRQ_INTX_ASSERT(i))
			generic_handle_domain_irq(ep->intx_domain, i);

	/* level/special sources are not enabled; clear them like the vendor does */
	elb_writel(ep, elb_readl(ep, PCIE_IRQ_LEVEL), PCIE_IRQ_LEVEL);
	elb_writel(ep, elb_readl(ep, PCIE_IRQ_SPECIAL), PCIE_IRQ_SPECIAL);

	chained_irq_exit(chip, desc);
}

static int exynos9820_pcie_intx_map(struct irq_domain *domain,
				    unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops exynos9820_pcie_intx_domain_ops = {
	.map = exynos9820_pcie_intx_map,
	.xlate = irq_domain_xlate_onecell,
};

static int exynos9820_pcie_init_irq(struct exynos9820_pcie *ep,
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
		return dev_err_probe(dev, -EINVAL,
				     "no legacy-interrupt-controller node\n");

	ep->intx_domain = irq_domain_create_linear(of_fwnode_handle(intc),
						   PCI_NUM_INTX,
						   &exynos9820_pcie_intx_domain_ops,
						   ep);
	of_node_put(intc);
	if (!ep->intx_domain)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to create INTx domain\n");

	irq_set_chained_handler_and_data(irq, exynos9820_pcie_irq_handler, ep);

	return 0;
}

static bool exynos9820_pcie_link_up(struct dw_pcie *pci)
{
	struct exynos9820_pcie *ep = to_exynos9820_pcie(pci);

	return elb_readl(ep, PCIE_ELBI_RDLH_LINKUP) & PCIE_ELBI_XMLH_LINKUP;
}

static int exynos9820_pcie_start_link(struct dw_pcie *pci)
{
	struct exynos9820_pcie *ep = to_exynos9820_pcie(pci);
	u32 v;
	int ret;

	/* rx elecidle check back on before the link comes up */
	exynos9820_rx_elecidle(ep, false);

	/* assert LTSSM enable */
	elb_writel(ep, PCIE_ELBI_LTSSM_ENABLE, PCIE_APP_LTSSM_ENABLE);

	/*
	 * Do not hand a dead link to the core. The vendor driver refuses
	 * every config access while the link is down because this root
	 * complex answers such an access with a bus error that hangs the
	 * SoC instead of returning all ones. Failing here fails host init
	 * before enumeration: no WLAN this time, but a running phone.
	 */
	ret = readl_poll_timeout(pci->elbi_base + PCIE_ELBI_RDLH_LINKUP, v,
				 v & PCIE_ELBI_XMLH_LINKUP, 1000, 500000);
	if (ret) {
		dev_err(pci->dev, "link did not come up (RDLH_LINKUP=%08x, history %08x)\n",
			v, elb_readl(ep, PCIE_STATE_HISTORY_CHECK));
		elb_writel(ep, PCIE_ELBI_LTSSM_DISABLE, PCIE_APP_LTSSM_ENABLE);
		exynos9820_perst(ep, true);
	}
	return ret;
}

static void exynos9820_pcie_stop_link(struct dw_pcie *pci)
{
	struct exynos9820_pcie *ep = to_exynos9820_pcie(pci);

	elb_writel(ep, PCIE_ELBI_LTSSM_DISABLE, PCIE_APP_LTSSM_ENABLE);
	exynos9820_perst(ep, true);
}

static const struct dw_pcie_ops exynos9820_pcie_ops = {
	.link_up = exynos9820_pcie_link_up,
	.start_link = exynos9820_pcie_start_link,
	.stop_link = exynos9820_pcie_stop_link,
};

/* the one root complex, for relink (below) */
static struct exynos9820_pcie *relink_ep;

static int exynos9820_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos9820_pcie *ep;
	struct dw_pcie *pci;
	int ret;

	ret = exynos9820_check_clocks(dev);
	if (ret)
		return ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	pci = &ep->pci;
	pci->dev = dev;
	pci->ops = &exynos9820_pcie_ops;

	ep->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(ep->phy))
		return PTR_ERR(ep->phy);

	ep->pcs = devm_platform_ioremap_resource_byname(pdev, "pcs");
	if (IS_ERR(ep->pcs))
		return PTR_ERR(ep->pcs);

	ep->pmu = syscon_regmap_lookup_by_phandle(dev->of_node,
						 "samsung,pmu-syscon");
	if (IS_ERR(ep->pmu))
		return dev_err_probe(dev, PTR_ERR(ep->pmu),
				     "failed to get PMU syscon\n");

	ep->sysreg = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "samsung,fsys-sysreg");
	if (IS_ERR(ep->sysreg))
		return dev_err_probe(dev, PTR_ERR(ep->sysreg),
				     "failed to get FSYS1 sysreg syscon\n");

	/* bank registers only, not requested: no pin controller owns them */
	ep->gpf2 = devm_ioremap(dev, GPIO_FSYS1_BASE + GPF2, 0x20);
	ep->gpg0 = devm_ioremap(dev, GPIO_PERIC0_BASE + GPG0, 0x20);
	if (!ep->gpf2 || !ep->gpg0)
		return -ENOMEM;

	/* PERST# asserted until host_init brings the PHY up; CLKREQ# to the RC */
	pin_out(ep->gpf2, GPF2_PERST, 0);
	bank_field(ep->gpf2, GPIO_PUD, GPF2_CLKREQ, 4, GPIO_PUD_UP);
	bank_field(ep->gpf2, GPIO_DRV, GPF2_CLKREQ, 4, GPIO_DRV_STOCK);
	bank_field(ep->gpf2, GPIO_CON, GPF2_CLKREQ, 4, GPIO_FUNC_PCIE_CLKREQ);

	/*
	 * WL_REG_ON powers the module's PCIe interface. Vendor order
	 * (dhd_custom_exynos.c dhd_wlan_init_gpio): WL_REG_ON high,
	 * WIFI_TURNON_DELAY = 200 ms, then the root complex comes up. Low
	 * first, so a module left running by an earlier load starts over.
	 * 50 ms was not enough after a warm reboot (12.11: the BCM4375 kept a
	 * bad state from the sleep tests, brcmfmac oopsed in process_rx on two
	 * reboots in a row, only a power-key power cycle cleared it): hold it
	 * low for 500 ms so the module really powers down.
	 */
	pin_out(ep->gpg0, GPG0_WL_REG_ON, 0);
	msleep(500);
	bank_field(ep->gpg0, GPIO_DAT, GPG0_WL_REG_ON, 1, 1);
	msleep(200);

	/* INTx domain before enumeration: the endpoint's IRQ is mapped during the scan */
	ret = exynos9820_pcie_init_irq(ep, pdev);
	if (ret)
		return ret;

	pci->pp.ops = &exynos9820_pcie_host_ops;
	platform_set_drvdata(pdev, ep);

	ret = dw_pcie_host_init(&pci->pp);
	if (!ret)
		WRITE_ONCE(relink_ep, ep);
	if (ret) {
		/* ep is devm memory: the chained handler must not outlive it */
		dev_err(dev, "failed to initialize host: %d\n", ret);
		irq_set_chained_handler_and_data(platform_get_irq(pdev, 0),
						 NULL, NULL);
		irq_domain_remove(ep->intx_domain);
		bank_field(ep->gpg0, GPIO_DAT, GPG0_WL_REG_ON, 1, 0);
	}

	return ret;
}

/*
 * System sleep. SYS_SLEEP powers BLK_FSYS1 down: ELBI, PHY, DBI and iATU
 * come back at their reset values (12.11 4c: after the first real sleep
 * the endpoint read D3cold, FLR and bus reset gave up, brcmfmac's probe read
 * all ones). zsleep unbinds brcmfmac before each sleep, so only the root
 * complex is ours to stop and to build again, before the PCI core restores
 * the root port's config (its resume_noirq runs after its parent's). An
 * early wakeup lost nothing, and the same bring-up is harmless then.
 * Modified for OmonullOS, November 2026: system sleep callbacks.
 */
static int exynos9820_pcie_suspend_noirq(struct device *dev)
{
	struct exynos9820_pcie *ep = dev_get_drvdata(dev);

	exynos9820_pcie_stop_link(&ep->pci);
	return 0;
}

static int exynos9820_pcie_resume_noirq(struct device *dev)
{
	struct exynos9820_pcie *ep = dev_get_drvdata(dev);
	struct dw_pcie_rp *pp = &ep->pci.pp;
	u32 con = readl(ep->gpg0 + GPIO_CON) >> (4 * GPG0_WL_REG_ON) & 0xf;
	u32 dat = readl(ep->gpg0 + GPIO_DAT) >> GPG0_WL_REG_ON & 1;
	int ret;

	/* the sleep module puts the bank registers back; set the pins anyway */
	pin_out(ep->gpf2, GPF2_PERST, 0);
	bank_field(ep->gpf2, GPIO_PUD, GPF2_CLKREQ, 4, GPIO_PUD_UP);
	bank_field(ep->gpf2, GPIO_DRV, GPF2_CLKREQ, 4, GPIO_DRV_STOCK);
	bank_field(ep->gpf2, GPIO_CON, GPF2_CLKREQ, 4, GPIO_FUNC_PCIE_CLKREQ);

	/* WL_REG_ON held high through the sleep (CON_PDN 3): the module kept
	 * its power. Otherwise it was off: power it and wait as probe does. */
	if (con != 1 || !dat) {
		dev_info(dev, "WL_REG_ON was off (con %u dat %u): powering the module\n",
			 con, dat);
		pin_out(ep->gpg0, GPG0_WL_REG_ON, 0);
		msleep(50);
		bank_field(ep->gpg0, GPIO_DAT, GPG0_WL_REG_ON, 1, 1);
		msleep(200);
	}

	exynos9820_pcie_host_init(pp);
	ret = dw_pcie_setup_rc(pp);
	if (!ret)
		ret = exynos9820_pcie_start_link(&ep->pci);
	if (ret)
		dev_err(dev, "resume: link not back (%d); Wi-Fi needs a reboot\n", ret);
	else
		dev_info(dev, "resume: link up again\n");
	/* never fail the system resume for the Wi-Fi */
	return 0;
}

static const struct dev_pm_ops exynos9820_pcie_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(exynos9820_pcie_suspend_noirq,
				  exynos9820_pcie_resume_noirq)
};

static const struct of_device_id exynos9820_pcie_of_match[] = {
	{ .compatible = "samsung,exynos9820-pcie", },
	{ },
};
MODULE_DEVICE_TABLE(of, exynos9820_pcie_of_match);

/*
 * Awake recovery. 16.11 09:53 (and 06:13, 15.11 21:46): every ring index
 * the brcmfmac read was 0xffff and the firmware never answered again, the
 * module had dropped out; only a reboot brought Wi-Fi back. Stock Android
 * recovers the same way: its HANG event makes the HAL power the module
 * down and up. Here user space (zwlan-recover) removes 0000:01:00.0, writes
 * relink, and rescans the bus. Reading relink tells the link state.
 * Modified for OmonullOS, November 2026: relink.
 */

static int relink_get(char *buf, const struct kernel_param *kp)
{
	struct exynos9820_pcie *ep = READ_ONCE(relink_ep);

	if (!ep)
		return sysfs_emit(buf, "none\n");
	return sysfs_emit(buf, "%s ltssm %08x history %08x\n",
			  exynos9820_pcie_link_up(&ep->pci) ? "up" : "down",
			  elb_readl(ep, PCIE_ELBI_RDLH_LINKUP),
			  elb_readl(ep, PCIE_STATE_HISTORY_CHECK));
}

static int relink_set(const char *val, const struct kernel_param *kp)
{
	struct exynos9820_pcie *ep = READ_ONCE(relink_ep);
	struct dw_pcie_rp *pp;
	struct device *dev;
	struct pci_dev *rp;
	int ret;

	if (!ep || !ep->pci.pp.bridge)
		return -ENODEV;
	pp = &ep->pci.pp;
	dev = ep->pci.dev;
	pci_lock_rescan_remove();
	rp = pci_get_slot(pp->bridge->bus, PCI_DEVFN(0, 0));
	if (!rp) {
		ret = -ENODEV;
		goto out;
	}
	/* the endpoint must be gone first: its driver would touch it meanwhile */
	if (rp->subordinate && !list_empty(&rp->subordinate->devices)) {
		ret = -EBUSY;
		goto put;
	}
	dev_info(dev, "relink: link %s (%08x, history %08x): module off and on\n",
		 exynos9820_pcie_link_up(&ep->pci) ? "up" : "down",
		 elb_readl(ep, PCIE_ELBI_RDLH_LINKUP),
		 elb_readl(ep, PCIE_STATE_HISTORY_CHECK));
	/* the root port's config lives in the DBI, which the core reset clears */
	pci_save_state(rp);
	exynos9820_pcie_stop_link(&ep->pci);
	pin_out(ep->gpg0, GPG0_WL_REG_ON, 0);
	msleep(500);		/* as probe: long enough to really power down */
	bank_field(ep->gpg0, GPIO_DAT, GPG0_WL_REG_ON, 1, 1);
	msleep(200);
	exynos9820_pcie_host_init(pp);
	ret = dw_pcie_setup_rc(pp);
	if (!ret)
		ret = exynos9820_pcie_start_link(&ep->pci);
	pci_restore_state(rp);
	if (ret)
		dev_err(dev, "relink: link not back (%d)\n", ret);
	else
		dev_info(dev, "relink: link up again\n");
put:
	pci_dev_put(rp);
out:
	pci_unlock_rescan_remove();
	return ret;
}

static const struct kernel_param_ops relink_ops = {
	.set = relink_set,
	.get = relink_get,
};
module_param_cb(relink, &relink_ops, NULL, 0600);
MODULE_PARM_DESC(relink, "write: power the Wi-Fi module off and on, train the link again (0000:01:00.0 removed first); read: link state");

/* no .remove: never rmmod (CLAUDE.md rule 7), reboot to try a new build */
static struct platform_driver exynos9820_pcie_driver = {
	.probe = exynos9820_pcie_probe,
	.driver = {
		.name = "pcie-exynos9820",
		.of_match_table = exynos9820_pcie_of_match,
		.pm = pm_sleep_ptr(&exynos9820_pcie_pm_ops),
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(exynos9820_pcie_driver);

MODULE_DESCRIPTION("PCIe host for the Galaxy S10 5G Wi-Fi (Exynos 9820 channel 0)");
MODULE_LICENSE("GPL v2");
