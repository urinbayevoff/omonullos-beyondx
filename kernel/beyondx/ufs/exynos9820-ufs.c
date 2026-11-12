// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * UFS host controller driver for the Samsung Exynos 9820 (Galaxy S10 5G).
 *
 * Added for OmonullOS, October 2026: the S9+ port's ufs-exynos9810.c
 * (third_party/s9plus/src/ufs) moved to the 9820. The host glue is that
 * driver's, which runs on mainline 7.1; what differs on the 9820 is taken
 * from Samsung's SM-G977N kernel (drivers/scsi/ufs/ufs-exynos.c):
 *
 *   - the PHY calibration is ufs-cal-9820.c, copied unchanged next to this
 *     file; its parameter block carries connected and active lanes, the max
 *     gear, the chip's EVT and the board, which are filled in where Samsung's
 *     driver fills them in (after link startup and around power mode changes)
 *   - hw-rev 6, which also takes the HWACG control (the 9810 stopped at 5)
 *   - a 32-bit DMA mask, as Samsung's probe sets it
 *   - no UFS VCC switching: its GPIO has no pinctrl driver here, the rail
 *     stays as S-Boot left it, and only RST_n resets the device (Samsung's
 *     driver also takes over from S-Boot with RST_n alone)
 *   - IO coherency is in SYSREG_FSYS1 (the UFS host sits in BLK_FSYS1)
 *
 * There is no CMU driver: GATE_UFS_EMBD and UFS_EMBD are fixed clocks at the
 * rate S-Boot leaves (166.5625 MHz, read from the CMU on the phone; Samsung's
 * table says 177.667, which only Android sets). The probe compares it with
 * the 1 us tick S-Boot programmed into the host and refuses to go on if they
 * disagree, since every UniPro timing the calibration writes is derived from
 * it (on bx20 it did refuse 177.667 against a tick of 166).
 *
 * The block is only reached once kernel/beyondx/zufs.sh has found BLK_FSYS1
 * clocked, so this is a module and never probes on its own at boot.
 *
 * Copyright (C) 2013-2018 Samsung Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/arm-smccc.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_host.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <ufs/ufshcd.h>
#include <ufs/unipro.h>
#include "ufshcd-pltfrm.h"
#include "ufs-cal-9820.h"

/* Vendor-specific HCI registers (second reg window) */
#define HCI_TXPRDT_ENTRY_SIZE		0x00
#define HCI_RXPRDT_ENTRY_SIZE		0x04
#define HCI_1US_TO_CNT_VAL		0x0c
#define  CNT_VAL_1US_MASK		0x3ff
#define HCI_UTRL_NEXUS_TYPE		0x40
#define HCI_UTMRL_NEXUS_TYPE		0x44
#define HCI_SW_RST			0x50
#define  UFS_LINK_SW_RST		BIT(0)
#define  UFS_UNIPRO_SW_RST		BIT(1)
#define  UFS_SW_RST_MASK		(UFS_UNIPRO_SW_RST | UFS_LINK_SW_RST)
#define HCI_DATA_REORDER		0x60
#define HCI_AXIDMA_RWDATA_BURST_LEN	0x6c
#define  BURST_LEN(x)			((x) << 27 | (x))
#define  WLU_EN				BIT(31)
#define HCI_GPIO_OUT			0x70
#define HCI_ERROR_EN_PA_LAYER		0x78
#define HCI_ERROR_EN_DL_LAYER		0x7c
#define HCI_ERROR_EN_N_LAYER		0x80
#define HCI_ERROR_EN_T_LAYER		0x84
#define HCI_ERROR_EN_DME_LAYER		0x88
#define HCI_UFSHCI_V2P1_CTRL		0x8c
#define  IA_TICK_SEL			BIT(16)
#define HCI_REQ_HOLD_EN			0xac
#define HCI_CLKSTOP_CTRL		0xb0
#define  REFCLKOUT_STOP			BIT(4)
#define  MPHY_APBCLK_STOP		BIT(3)
#define  REFCLK_STOP			BIT(2)
#define  UNIPRO_MCLK_STOP		BIT(1)
#define  UNIPRO_PCLK_STOP		BIT(0)
#define  CLK_STOP_ALL			(REFCLKOUT_STOP | REFCLK_STOP | \
					 UNIPRO_MCLK_STOP | UNIPRO_PCLK_STOP)
#define HCI_FORCE_HCS			0xb4
#define  REFCLKOUT_STOP_EN		BIT(11)
#define  MPHY_APBCLK_STOP_EN		BIT(10)
#define  UFSP_DRCG_EN			BIT(8)
#define  REFCLK_STOP_EN			BIT(7)
#define  UNIPRO_PCLK_STOP_EN		BIT(6)
#define  UNIPRO_MCLK_STOP_EN		BIT(5)
#define  HCI_CORECLK_STOP_EN		BIT(4)
#define  CLK_STOP_CTRL_EN_ALL		(UFSP_DRCG_EN | MPHY_APBCLK_STOP_EN | \
					 REFCLKOUT_STOP_EN | REFCLK_STOP_EN | \
					 UNIPRO_PCLK_STOP_EN | UNIPRO_MCLK_STOP_EN)
#define HCI_FSM_MONITOR			0xc0
#define HCI_UFS_ACG_DISABLE		0xfc
#define  HCI_UFS_ACG_DISABLE_EN		BIT(0)
#define HCI_IOP_ACG_DISABLE		0x100
#define  HCI_IOP_ACG_DISABLE_EN		BIT(0)
#define HCI_MPHY_REFCLK_SEL		0x108
#define  MPHY_REFCLK_SEL		BIT(0)

#define UFS_VER_0004			4
#define UFS_VER_0005			5
#define UFS_VER_0006			6

#define PRDT_PREFETCH_EN		BIT(31)
#define PRDT_SET_SIZE(x)		((x) & 0x1f)

#define DFES_ERR_EN			BIT(31)
#define DFES_DEF_DL_ERRS		(UIC_DATA_LINK_LAYER_ERROR_RX_BUF_OF | \
					 UIC_DATA_LINK_LAYER_ERROR_PA_INIT)
#define DFES_DEF_N_ERRS			(UIC_NETWORK_UNSUPPORTED_HEADER_TYPE | \
					 UIC_NETWORK_BAD_DEVICEID_ENC | \
					 UIC_NETWORK_LHDR_TRAP_PACKET_DROPPING)
#define DFES_DEF_T_ERRS			(UIC_TRANSPORT_UNSUPPORTED_HEADER_TYPE | \
					 UIC_TRANSPORT_UNKNOWN_CPORTID | \
					 UIC_TRANSPORT_NO_CONNECTION_RX | \
					 UIC_TRANSPORT_BAD_TC)

/* PHY isolation bypass, bit 0 of UFS_PHY_CONTROL in the PMU */
#define PMU_UFS_PHY_CTRL_EN		BIT(0)

struct exynos9820_ufs {
	struct device *dev;
	struct ufs_hba *hba;

	void __iomem *reg_hci;		/* vendor-specific HCI */
	void __iomem *reg_unipro;
	void __iomem *reg_pma;		/* PHY */

	struct regmap *pmu;
	u32 pmu_phy_ctrl;
	u32 hw_rev;

	struct regmap *sysreg;		/* FSYS1 system register: IO coherency */
	u32 sysreg_offset;
	u32 sysreg_mask;
	u32 sysreg_bits;

	struct clk *clk_hci;
	struct clk *clk_unipro;
	u32 mclk_rate;

	u32 num_rx_lanes;
	u32 num_tx_lanes;

	struct uic_pwr_mode req_pmd;	/* what the board asks for */
	struct uic_pwr_mode act_pmd;	/* what was negotiated */

	struct ufs_cal_param *cal;
};

static inline struct exynos9820_ufs *to_exynos_ufs(struct ufs_hba *hba)
{
	return ufshcd_get_variant(hba);
}

static inline void hci_writel(struct exynos9820_ufs *ufs, u32 val, u32 reg)
{
	writel(val, ufs->reg_hci + reg);
}

static inline u32 hci_readl(struct exynos9820_ufs *ufs, u32 reg)
{
	return readl(ufs->reg_hci + reg);
}

/*
 * Hooks the calibration code calls back into. The handle it hands around is
 * this driver's own state, set up as cal_param->host.
 */
void ufs_lld_dme_set(void *h, u32 addr, u32 val)
{
	struct exynos9820_ufs *ufs = h;

	ufshcd_dme_set_attr(ufs->hba, addr, ATTR_SET_NOR, val, DME_LOCAL);
}

void ufs_lld_dme_get(void *h, u32 addr, u32 *val)
{
	struct exynos9820_ufs *ufs = h;

	ufshcd_dme_get_attr(ufs->hba, addr, val, DME_LOCAL);
}

void ufs_lld_dme_peer_set(void *h, u32 addr, u32 val)
{
	struct exynos9820_ufs *ufs = h;

	ufshcd_dme_set_attr(ufs->hba, addr, ATTR_SET_NOR, val, DME_PEER);
}

void ufs_lld_pma_write(void *h, u32 val, u32 addr)
{
	struct exynos9820_ufs *ufs = h;
	u32 clkstop = hci_readl(ufs, HCI_CLKSTOP_CTRL);

	/* as Samsung's phy_pma_writel(): ungate the PHY's APB for each access */
	hci_writel(ufs, clkstop & ~MPHY_APBCLK_STOP, HCI_CLKSTOP_CTRL);
	writel(val, ufs->reg_pma + addr);
	hci_writel(ufs, clkstop | MPHY_APBCLK_STOP, HCI_CLKSTOP_CTRL);
}

u32 ufs_lld_pma_read(void *h, u32 addr)
{
	struct exynos9820_ufs *ufs = h;
	u32 clkstop = hci_readl(ufs, HCI_CLKSTOP_CTRL);
	u32 val;

	hci_writel(ufs, clkstop & ~MPHY_APBCLK_STOP, HCI_CLKSTOP_CTRL);
	val = readl(ufs->reg_pma + addr);
	hci_writel(ufs, clkstop | MPHY_APBCLK_STOP, HCI_CLKSTOP_CTRL);
	return val;
}

void ufs_lld_unipro_write(void *h, u32 val, u32 addr)
{
	struct exynos9820_ufs *ufs = h;

	writel(val, ufs->reg_unipro + addr);
}

void ufs_lld_udelay(u32 val)
{
	udelay(val);
}

void ufs_lld_usleep_delay(u32 min, u32 max)
{
	usleep_range(min, max);
}

unsigned long ufs_lld_get_time_count(unsigned long offset)
{
	return jiffies;
}

unsigned long ufs_lld_calc_timeout(const unsigned int ms)
{
	return msecs_to_jiffies(ms);
}

static int exynos_ufs_cal(struct exynos9820_ufs *ufs,
			  ufs_cal_errno (*fn)(struct ufs_cal_param *),
			  const char *what)
{
	ufs_cal_errno err;

	ufs->cal->mclk_rate = ufs->mclk_rate;
	ufs->cal->available_lane = ufs->num_rx_lanes;
	ufs->cal->pmd = &ufs->act_pmd;

	err = fn(ufs->cal);
	if (err != UFS_CAL_NO_ERROR) {
		dev_err(ufs->dev, "%s calibration failed: %d\n", what, err);
		return -EIO;
	}

	return 0;
}

/* Samsung's exynos_ufs_update_active_lanes() */
static void exynos_ufs_update_active_lanes(struct exynos9820_ufs *ufs)
{
	u32 tx = 0, rx = 0;

	ufshcd_dme_get(ufs->hba, UIC_ARG_MIB(PA_ACTIVETXDATALANES), &tx);
	ufshcd_dme_get(ufs->hba, UIC_ARG_MIB(PA_ACTIVERXDATALANES), &rx);
	ufs->cal->active_tx_lane = tx;
	ufs->cal->active_rx_lane = rx;
	dev_info(ufs->dev, "active lanes tx %u rx %u\n", tx, rx);
}

/* Samsung's exynos_ufs_update_max_gear() */
static void exynos_ufs_update_max_gear(struct exynos9820_ufs *ufs)
{
	u32 gear = 0;

	ufshcd_dme_get(ufs->hba, UIC_ARG_MIB(PA_MAXRXHSGEAR), &gear);
	ufs->cal->max_gear = min_t(u8, gear, ufs->req_pmd.gear);
	dev_info(ufs->dev, "max gear %u (device %u)\n", ufs->cal->max_gear, gear);
}

static void exynos_ufs_ctrl_phy_pwr(struct exynos9820_ufs *ufs, bool en)
{
	regmap_update_bits(ufs->pmu, ufs->pmu_phy_ctrl,
			   PMU_UFS_PHY_CTRL_EN, en ? PMU_UFS_PHY_CTRL_EN : 0);
}

static void exynos_ufs_ctrl_auto_hci_clk(struct exynos9820_ufs *ufs, bool en)
{
	u32 reg = hci_readl(ufs, HCI_FORCE_HCS);

	if (en)
		hci_writel(ufs, reg | HCI_CORECLK_STOP_EN, HCI_FORCE_HCS);
	else
		hci_writel(ufs, reg & ~HCI_CORECLK_STOP_EN, HCI_FORCE_HCS);
}

static void exynos_ufs_ctrl_clk(struct exynos9820_ufs *ufs, bool en)
{
	u32 reg = hci_readl(ufs, HCI_FORCE_HCS);

	if (en)
		hci_writel(ufs, reg | CLK_STOP_CTRL_EN_ALL, HCI_FORCE_HCS);
	else
		hci_writel(ufs, reg & ~CLK_STOP_CTRL_EN_ALL, HCI_FORCE_HCS);
}

static void exynos_ufs_gate_clk(struct exynos9820_ufs *ufs, bool en)
{
	u32 reg = hci_readl(ufs, HCI_CLKSTOP_CTRL);

	if (en)
		hci_writel(ufs, reg | CLK_STOP_ALL, HCI_CLKSTOP_CTRL);
	else
		hci_writel(ufs, reg & ~CLK_STOP_ALL, HCI_CLKSTOP_CTRL);
}

static void exynos_ufs_set_hwacg_control(struct exynos9820_ufs *ufs, bool en)
{
	u32 reg;

	if (ufs->hw_rev != UFS_VER_0004 && ufs->hw_rev != UFS_VER_0005 &&
	    ufs->hw_rev != UFS_VER_0006)
		return;

	reg = hci_readl(ufs, HCI_UFS_ACG_DISABLE);

	if (en)
		hci_writel(ufs, reg & ~HCI_UFS_ACG_DISABLE_EN, HCI_UFS_ACG_DISABLE);
	else
		hci_writel(ufs, reg | HCI_UFS_ACG_DISABLE_EN, HCI_UFS_ACG_DISABLE);
}

static void exynos_ufs_select_refclk(struct exynos9820_ufs *ufs, bool en)
{
	u32 reg;

	/*
	 * Only revision 4 has the alternative reference-clock path; on the
	 * S9 (revision 5) writing it anyway left the link dead. The 9820 is
	 * revision 6, where Samsung's driver returns here as well.
	 */
	if (ufs->hw_rev != UFS_VER_0004)
		return;

	reg = hci_readl(ufs, HCI_MPHY_REFCLK_SEL);

	if (en)
		hci_writel(ufs, reg | MPHY_REFCLK_SEL, HCI_MPHY_REFCLK_SEL);
	else
		hci_writel(ufs, reg & ~MPHY_REFCLK_SEL, HCI_MPHY_REFCLK_SEL);
}

static void exynos_ufs_fit_aggr_timeout(struct exynos9820_ufs *ufs)
{
	u32 reg;

	reg = hci_readl(ufs, HCI_UFSHCI_V2P1_CTRL) | IA_TICK_SEL;
	hci_writel(ufs, reg, HCI_UFSHCI_V2P1_CTRL);
	hci_writel(ufs, (ufs->mclk_rate / 1000000) & CNT_VAL_1US_MASK,
		   HCI_1US_TO_CNT_VAL);
}

static void exynos_ufs_config_intr(struct exynos9820_ufs *ufs, u32 errs, u32 reg)
{
	hci_writel(ufs, DFES_ERR_EN | errs, reg);
}

static void exynos_ufs_init_host(struct exynos9820_ufs *ufs)
{
	u32 reg;

	exynos_ufs_ctrl_auto_hci_clk(ufs, false);
	exynos_ufs_fit_aggr_timeout(ufs);

	hci_writel(ufs, 0xa, HCI_DATA_REORDER);
	hci_writel(ufs, PRDT_PREFETCH_EN | PRDT_SET_SIZE(12),
		   HCI_TXPRDT_ENTRY_SIZE);
	hci_writel(ufs, PRDT_SET_SIZE(12), HCI_RXPRDT_ENTRY_SIZE);
	hci_writel(ufs, 0xffffffff, HCI_UTRL_NEXUS_TYPE);
	hci_writel(ufs, 0xffffffff, HCI_UTMRL_NEXUS_TYPE);
	hci_writel(ufs, WLU_EN | BURST_LEN(3), HCI_AXIDMA_RWDATA_BURST_LEN);

	reg = hci_readl(ufs, HCI_IOP_ACG_DISABLE);
	hci_writel(ufs, reg & ~HCI_IOP_ACG_DISABLE_EN, HCI_IOP_ACG_DISABLE);
}

static void exynos_ufs_dev_hw_reset(struct exynos9820_ufs *ufs)
{
	/*
	 * Samsung pulses RST_n for 5 us. The S9 port found that a device whose
	 * link was already trained needs the pulse held well past tRSTW and
	 * the full initialisation window after it, and S-Boot has trained this
	 * one; the longer timing costs 30 ms once.
	 */
	hci_writel(ufs, 0, HCI_GPIO_OUT);
	usleep_range(10000, 11000);
	hci_writel(ufs, 1, HCI_GPIO_OUT);
	usleep_range(20000, 21000);
}

static int exynos_ufs_host_reset(struct exynos9820_ufs *ufs)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(10);

	exynos_ufs_ctrl_auto_hci_clk(ufs, false);
	hci_writel(ufs, UFS_SW_RST_MASK, HCI_SW_RST);

	do {
		if (!(hci_readl(ufs, HCI_SW_RST) & UFS_SW_RST_MASK)) {
			exynos_ufs_init_host(ufs);
			exynos_ufs_dev_hw_reset(ufs);
			return 0;
		}
		cpu_relax();
	} while (time_before(jiffies, timeout));

	dev_err(ufs->dev, "timeout waiting for host software reset\n");
	return -ETIMEDOUT;
}

static void exynos_ufs_config_sysreg(struct exynos9820_ufs *ufs)
{
	if (!ufs->sysreg)
		return;

	regmap_update_bits(ufs->sysreg, ufs->sysreg_offset,
			   ufs->sysreg_mask, ufs->sysreg_bits);
}

static int exynos_ufs_get_clks(struct exynos9820_ufs *ufs)
{
	struct ufs_clk_info *clki;
	u32 tick;

	list_for_each_entry(clki, &ufs->hba->clk_list_head, list) {
		if (IS_ERR_OR_NULL(clki->clk))
			continue;
		if (!strcmp(clki->name, "GATE_UFS_EMBD"))
			ufs->clk_hci = clki->clk;
		else if (!strcmp(clki->name, "UFS_EMBD"))
			ufs->clk_unipro = clki->clk;
	}

	if (!ufs->clk_hci || !ufs->clk_unipro) {
		dev_err(ufs->dev, "GATE_UFS_EMBD/UFS_EMBD clocks missing\n");
		return -EINVAL;
	}

	ufs->mclk_rate = clk_get_rate(ufs->clk_unipro);
	if (!ufs->mclk_rate) {
		dev_err(ufs->dev, "UFS_EMBD has no rate\n");
		return -EINVAL;
	}

	/*
	 * S-Boot set the host's 1 us tick from the real clock (Samsung's
	 * fit_aggr_timeout: rate / 1 MHz). A fixed clock that disagrees would
	 * put every UniPro timing the calibration writes off by as much.
	 */
	tick = hci_readl(ufs, HCI_1US_TO_CNT_VAL) & CNT_VAL_1US_MASK;
	dev_info(ufs->dev, "unipro mclk %u Hz, S-Boot's 1 us tick %u\n",
		 ufs->mclk_rate, tick);
	if (tick && abs((int)tick - (int)(ufs->mclk_rate / 1000000)) > 1) {
		dev_err(ufs->dev, "UFS_EMBD is not %u Hz, S-Boot runs it at ~%u MHz\n",
			ufs->mclk_rate, tick);
		return -EINVAL;
	}

	return 0;
}

static int exynos_ufs_parse_dt(struct exynos9820_ufs *ufs)
{
	struct device_node *np = ufs->dev->of_node;
	struct of_phandle_args args;
	u8 val;
	int ret;

	if (of_property_read_u32(np, "hw-rev", &ufs->hw_rev))
		ufs->hw_rev = UFS_VER_0004;	/* Samsung's default */

	/* the calibration's table selectors, as Samsung's populate_dt */
	if (of_property_read_u8(np, "samsung,evt-ver", &ufs->cal->evt_ver))
		ufs->cal->evt_ver = 0;
	if (of_property_read_u8(np, "samsung,brd-for-cal", &ufs->cal->board))
		ufs->cal->board = 0;
	ufs->cal->tbl = HOST_EMBD;

	ufs->pmu = syscon_regmap_lookup_by_phandle_args(np, "samsung,pmu-syscon",
							1, &ufs->pmu_phy_ctrl);
	if (IS_ERR(ufs->pmu))
		return dev_err_probe(ufs->dev, PTR_ERR(ufs->pmu),
				     "no samsung,pmu-syscon\n");

	/*
	 * IO coherency: a bit in the FSYS1 system register has to agree with
	 * the dma-coherent property, or transfers land in stale memory.
	 */
	ret = of_parse_phandle_with_fixed_args(np, "samsung,sysreg", 3, 0, &args);
	if (!ret) {
		ufs->sysreg = syscon_node_to_regmap(args.np);
		of_node_put(args.np);
		if (IS_ERR(ufs->sysreg))
			return dev_err_probe(ufs->dev, PTR_ERR(ufs->sysreg),
					     "samsung,sysreg is not a syscon\n");
		ufs->sysreg_offset = args.args[0];
		ufs->sysreg_mask = args.args[1];
		ufs->sysreg_bits = args.args[2];
	}

	ufs->req_pmd.mode = FAST_MODE;
	ufs->req_pmd.hs_series = PA_HS_MODE_B;
	ufs->req_pmd.lane =
		of_property_read_u8(np, "samsung,pmd-attr-lane", &val) ? 1 : val;
	ufs->req_pmd.gear =
		of_property_read_u8(np, "samsung,pmd-attr-gear", &val) ? 1 : val;

	dev_info(ufs->dev, "hw-rev %u evt %u board %u, asks for HS-G%u x%u\n",
		 ufs->hw_rev, ufs->cal->evt_ver, ufs->cal->board,
		 ufs->req_pmd.gear, ufs->req_pmd.lane);
	return 0;
}

/*
 * Samsung's Flash Memory Protector sits in the data path and decides how
 * wide a PRDT entry is. Its registers belong to the secure world, so it is
 * set up through the SMC calls Samsung's fmp.c and smu.c make (the same IDs
 * on the 9810 and the 9820). Samsung asks for descriptor type 3 (128-byte
 * PRDT entries, which carry its keys); with no FMP driver the S9 port
 * measured type 0 to be the one that transfers, so that is the default.
 * Type 3 would also need CONFIG_SCSI_UFS_VARIABLE_SG_ENTRY_SIZE, which this
 * kernel does not have, so it is refused.
 */
#define SMC_CMD_FMP_SECURITY	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,\
					   ARM_SMCCC_SMC_64,\
					   ARM_SMCCC_OWNER_SIP, 0x1810)
#define SMC_CMD_SMU		ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,\
					   ARM_SMCCC_SMC_64,\
					   ARM_SMCCC_OWNER_SIP, 0x1850)
#define SMU_EMBEDDED			0
#define SMU_INIT			0

/* -1 leaves the protector alone; 0 selects the standard descriptor type. */
static int ufs_fmp_desctype;
module_param_named(fmp, ufs_fmp_desctype, int, 0444);
MODULE_PARM_DESC(fmp, "FMP descriptor type: 0 = standard, -1 = untouched");

static void exynos_ufs_fmp_init(struct ufs_hba *hba)
{
	struct arm_smccc_res res;

	if (ufs_fmp_desctype < 0)
		return;
	if (ufs_fmp_desctype != 0) {
		dev_err(hba->dev, "fmp: descriptor type %d not supported\n",
			ufs_fmp_desctype);
		return;
	}

	arm_smccc_smc(SMC_CMD_FMP_SECURITY, 0, SMU_EMBEDDED,
		      ufs_fmp_desctype, 0, 0, 0, 0, &res);
	dev_info(hba->dev, "fmp: desctype %d -> %ld\n",
		 ufs_fmp_desctype, res.a0);
	if (res.a0)
		return;

	arm_smccc_smc(SMC_CMD_SMU, SMU_INIT, SMU_EMBEDDED, 0, 0, 0, 0, 0,
		      &res);
	dev_info(hba->dev, "fmp: smu init -> %ld, sg entry %zu\n", res.a0,
		 ufshcd_sg_entry_size(hba));
}

/*
 * The S9 port measured that a segment larger than 4K wedges the doorbell
 * (the FMP data unit stays in the data path even on descriptor type 0).
 */
static uint ufs_max_seg_size = 4096;
module_param_named(max_seg_size, ufs_max_seg_size, uint, 0444);

static bool ufs_clk_gating;
module_param_named(clkgate, ufs_clk_gating, bool, 0444);

/* Samsung's probe: DMA_BIT_MASK(32); swiotlb bounces what lies above 4G */
static uint ufs_dma_bits = 32;
module_param_named(dma_bits, ufs_dma_bits, uint, 0444);
MODULE_PARM_DESC(dma_bits, "DMA address width to request, 32 or 64");

static int exynos_ufs_set_dma_mask(struct ufs_hba *hba)
{
	int err = dma_set_mask_and_coherent(hba->dev,
					    DMA_BIT_MASK(ufs_dma_bits));

	dev_info(hba->dev, "dma mask %u bit err=%d\n", ufs_dma_bits, err);
	return err;
}

static int exynos_ufs_init(struct ufs_hba *hba)
{
	struct platform_device *pdev = to_platform_device(hba->dev);
	struct exynos9820_ufs *ufs;
	int ret;

	ufs = devm_kzalloc(hba->dev, sizeof(*ufs), GFP_KERNEL);
	if (!ufs)
		return -ENOMEM;

	ufs->cal = devm_kzalloc(hba->dev, sizeof(*ufs->cal), GFP_KERNEL);
	if (!ufs->cal)
		return -ENOMEM;

	ufs->dev = hba->dev;
	ufs->hba = hba;
	ufs->cal->host = ufs;

	ufs->reg_hci = devm_platform_ioremap_resource_byname(pdev, "vs_hci");
	if (IS_ERR(ufs->reg_hci))
		return PTR_ERR(ufs->reg_hci);

	ufs->reg_unipro = devm_platform_ioremap_resource_byname(pdev, "unipro");
	if (IS_ERR(ufs->reg_unipro))
		return PTR_ERR(ufs->reg_unipro);

	ufs->reg_pma = devm_platform_ioremap_resource_byname(pdev, "pma");
	if (IS_ERR(ufs->reg_pma))
		return PTR_ERR(ufs->reg_pma);

	ret = exynos_ufs_parse_dt(ufs);
	if (ret)
		return ret;

	ret = exynos_ufs_get_clks(ufs);
	if (ret)
		return ret;

	dev_info(ufs->dev, "inherited clkstop=%08x force_hcs=%08x gpio=%08x refclk_sel=%08x\n",
		 hci_readl(ufs, HCI_CLKSTOP_CTRL), hci_readl(ufs, HCI_FORCE_HCS),
		 hci_readl(ufs, HCI_GPIO_OUT), hci_readl(ufs, HCI_MPHY_REFCLK_SEL));

	if (ufs_cal_init(ufs->cal, 0) != UFS_CAL_NO_ERROR)
		return -EINVAL;

	/* only now: the hooks below take a non-NULL variant as set up */
	ufshcd_set_variant(hba, ufs);

	if (ufs_clk_gating)
		hba->caps |= UFSHCD_CAP_CLK_GATING |
			     UFSHCD_CAP_HIBERN8_WITH_CLK_GATING;
	hba->quirks |= UFSHCD_QUIRK_PRDT_BYTE_GRAN |
		       UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR |
		       UFSHCI_QUIRK_SKIP_RESET_INTR_AGGR |
		       UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR |
		       UFSHCD_QUIRK_SKIP_DEF_UNIPRO_TIMEOUT_SETTING;

	/* take the PHY out of isolation and set coherency up */
	exynos_ufs_ctrl_phy_pwr(ufs, true);
	exynos_ufs_config_sysreg(ufs);
	exynos_ufs_fmp_init(hba);

	/* both still take effect here: scsi_add_host runs later */
	if (ufs_max_seg_size) {
		hba->host->max_segment_size = ufs_max_seg_size;
		dma_set_max_seg_size(hba->dev, ufs_max_seg_size);
	}

	return 0;
}

static void exynos_ufs_dump_state(struct exynos9820_ufs *ufs, const char *why);

static void exynos_ufs_exit(struct ufs_hba *hba)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);

	/* also the failed-probe path; nothing to undo before .init finished */
	if (!ufs)
		return;
	exynos_ufs_dump_state(ufs, "exit");
	exynos_ufs_ctrl_phy_pwr(ufs, false);
}

static int exynos_ufs_hce_enable_notify(struct ufs_hba *hba,
					enum ufs_notify_change_status status)
{
	if (status == PRE_CHANGE)
		return exynos_ufs_host_reset(to_exynos_ufs(hba));

	return 0;
}

static int exynos_ufs_link_startup_notify(struct ufs_hba *hba,
					  enum ufs_notify_change_status status)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);
	int ret;

	switch (status) {
	case PRE_CHANGE:
		exynos_ufs_config_intr(ufs, DFES_DEF_DL_ERRS, HCI_ERROR_EN_DL_LAYER);
		exynos_ufs_config_intr(ufs, DFES_DEF_N_ERRS, HCI_ERROR_EN_N_LAYER);
		exynos_ufs_config_intr(ufs, DFES_DEF_T_ERRS, HCI_ERROR_EN_T_LAYER);

		exynos_ufs_ctrl_clk(ufs, true);
		exynos_ufs_select_refclk(ufs, true);
		exynos_ufs_gate_clk(ufs, false);
		exynos_ufs_set_hwacg_control(ufs, false);

		if (!ufs->num_rx_lanes || !ufs->num_tx_lanes) {
			ufshcd_dme_get(hba, UIC_ARG_MIB(PA_AVAILRXDATALANES),
				       &ufs->num_rx_lanes);
			ufshcd_dme_get(hba, UIC_ARG_MIB(PA_AVAILTXDATALANES),
				       &ufs->num_tx_lanes);
			dev_info(ufs->dev, "available lanes rx %u tx %u\n",
				 ufs->num_rx_lanes, ufs->num_tx_lanes);
		}

		ret = exynos_ufs_cal(ufs, ufs_cal_pre_link, "pre-link");
		exynos_ufs_dump_state(ufs, "pre-link");
		return ret;
	case POST_CHANGE:
		exynos_ufs_update_active_lanes(ufs);
		exynos_ufs_update_max_gear(ufs);
		return exynos_ufs_cal(ufs, ufs_cal_post_link, "post-link");
	default:
		return 0;
	}
}

static int exynos_ufs_negotiate_pwr_mode(struct ufs_hba *hba,
				const struct ufs_pa_layer_attr *desired,
				struct ufs_pa_layer_attr *final)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);
	struct uic_pwr_mode *req = &ufs->req_pmd;

	/* Samsung's exynos_ufs_init_pmc_req() */
	ufs->num_rx_lanes = desired->lane_rx;
	ufs->num_tx_lanes = desired->lane_tx;
	ufs->cal->connected_rx_lane = desired->lane_rx;
	ufs->cal->connected_tx_lane = desired->lane_tx;

	final->gear_rx = min_t(u8, desired->gear_rx, req->gear);
	final->gear_tx = min_t(u8, desired->gear_tx, req->gear);
	final->lane_rx = min_t(u8, desired->lane_rx, req->lane);
	final->lane_tx = min_t(u8, desired->lane_tx, req->lane);
	final->pwr_rx = req->mode;
	final->pwr_tx = req->mode;
	final->hs_rate = req->hs_series;

	ufs->act_pmd.gear = final->gear_rx;
	ufs->act_pmd.lane = final->lane_rx;
	ufs->act_pmd.mode = req->mode;
	ufs->act_pmd.hs_series = req->hs_series;

	return 0;
}

static int exynos_ufs_pwr_change_notify(struct ufs_hba *hba,
					enum ufs_notify_change_status status,
					struct ufs_pa_layer_attr *final)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);
	int ret;

	switch (status) {
	case PRE_CHANGE:
		return exynos_ufs_cal(ufs, ufs_cal_pre_pmc, "pre-pmc");
	case POST_CHANGE:
		exynos_ufs_update_active_lanes(ufs);
		ret = exynos_ufs_cal(ufs, ufs_cal_post_pmc, "post-pmc");
		dev_info(ufs->dev, "power mode (%d): M(%d) G(%d) L(%d) HS-series(%d)\n",
			 ret, ufs->act_pmd.mode, ufs->act_pmd.gear,
			 ufs->act_pmd.lane, ufs->act_pmd.hs_series);
		return ret;
	default:
		return 0;
	}
}

static int exynos_ufs_setup_clocks(struct ufs_hba *hba, bool on,
				   enum ufs_notify_change_status status)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);

	/*
	 * ufshcd_hba_init() runs this before .init, when there is no variant
	 * and nothing mapped to gate yet.
	 */
	if (!ufs)
		return 0;

	if (on && status == PRE_CHANGE) {
		exynos_ufs_ctrl_auto_hci_clk(ufs, false);
		exynos_ufs_gate_clk(ufs, false);
		exynos_ufs_set_hwacg_control(ufs, false);
	} else if (!on && status == POST_CHANGE) {
		exynos_ufs_gate_clk(ufs, true);
		exynos_ufs_ctrl_auto_hci_clk(ufs, true);
		exynos_ufs_set_hwacg_control(ufs, true);
	}

	return 0;
}

static void exynos_ufs_setup_xfer_req(struct ufs_hba *hba, int tag,
				      bool is_scsi_cmd)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);
	u32 type = hci_readl(ufs, HCI_UTRL_NEXUS_TYPE);

	if (is_scsi_cmd)
		type |= BIT(tag);
	else
		type &= ~BIT(tag);

	hci_writel(ufs, type, HCI_UTRL_NEXUS_TYPE);
}

static void exynos_ufs_setup_task_mgmt(struct ufs_hba *hba, int tag, u8 tm_func)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);
	u32 type = hci_readl(ufs, HCI_UTMRL_NEXUS_TYPE);

	switch (tm_func) {
	case UFS_ABORT_TASK:
	case UFS_QUERY_TASK:
		type |= BIT(tag);
		break;
	case UFS_ABORT_TASK_SET:
	case UFS_CLEAR_TASK_SET:
	case UFS_LOGICAL_RESET:
	case UFS_QUERY_TASK_SET:
		type &= ~BIT(tag);
		break;
	}

	hci_writel(ufs, type, HCI_UTMRL_NEXUS_TYPE);
}

static void exynos_ufs_hibern8_notify(struct ufs_hba *hba, enum uic_cmd_dme cmd,
				      enum ufs_notify_change_status status)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);

	if (status == PRE_CHANGE && cmd == UIC_CMD_DME_HIBER_EXIT)
		exynos_ufs_cal(ufs, ufs_cal_pre_h8_exit, "pre-h8-exit");
	else if (status == POST_CHANGE && cmd == UIC_CMD_DME_HIBER_ENTER)
		exynos_ufs_cal(ufs, ufs_cal_post_h8_enter, "post-h8-enter");
}

static int exynos_ufs_suspend(struct ufs_hba *hba, enum ufs_pm_op pm_op,
			      enum ufs_notify_change_status status)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);

	if (status == PRE_CHANGE)
		return 0;

	/*
	 * As the S9 port's patch-ufs-suspend.py: a link kept in Hibern8 is
	 * left alone, ufshcd leaves it again with DME_HIBER_EXIT on resume.
	 * Only a link-off transition, which reinitialises host and device,
	 * resets the device and isolates the PHY.
	 */
	if (ufshcd_is_link_off(hba)) {
		hci_writel(ufs, 0, HCI_GPIO_OUT);
		exynos_ufs_ctrl_phy_pwr(ufs, false);
	}

	return 0;
}

static int exynos_ufs_resume(struct ufs_hba *hba, enum ufs_pm_op pm_op)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);

	exynos_ufs_ctrl_phy_pwr(ufs, true);
	exynos_ufs_config_sysreg(ufs);
	exynos_ufs_ctrl_auto_hci_clk(ufs, false);

	return 0;
}

static int exynos_ufs_device_reset(struct ufs_hba *hba)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);

	exynos_ufs_dev_hw_reset(ufs);
	dev_info(ufs->dev, "device reset done, gpio=%08x\n",
		 hci_readl(ufs, HCI_GPIO_OUT));

	return 0;
}

static void exynos_ufs_dump_state(struct exynos9820_ufs *ufs, const char *why)
{
	struct ufs_hba *hba = ufs->hba;
	/* everything but the plain register reads may sleep */
	bool sleepable = !strcmp(why, "pre-link");
	u32 phy_control = 0;

	if (sleepable)
		regmap_read(ufs->pmu, ufs->pmu_phy_ctrl, &phy_control);
	dev_info(ufs->dev, "%s: HCE=%08x HCS=%08x IS=%08x IE=%08x\n",
		 why, ufshcd_readl(hba, REG_CONTROLLER_ENABLE),
		 ufshcd_readl(hba, REG_CONTROLLER_STATUS),
		 ufshcd_readl(hba, REG_INTERRUPT_STATUS),
		 ufshcd_readl(hba, REG_INTERRUPT_ENABLE));
	dev_info(ufs->dev, "UIC cmd=%08x args=%08x/%08x/%08x errors PA=%08x DL=%08x DME=%08x\n",
		 ufshcd_readl(hba, REG_UIC_COMMAND),
		 ufshcd_readl(hba, REG_UIC_COMMAND_ARG_1),
		 ufshcd_readl(hba, REG_UIC_COMMAND_ARG_2),
		 ufshcd_readl(hba, REG_UIC_COMMAND_ARG_3),
		 ufshcd_readl(hba, REG_UIC_ERROR_CODE_PHY_ADAPTER_LAYER),
		 ufshcd_readl(hba, REG_UIC_ERROR_CODE_DATA_LINK_LAYER),
		 ufshcd_readl(hba, REG_UIC_ERROR_CODE_DME));
	dev_info(ufs->dev, "%s: clkstop=%08x force=%08x gpio=%08x phy=%08x fsm=%08x acg=%08x hold=%08x\n",
		 why, hci_readl(ufs, HCI_CLKSTOP_CTRL), hci_readl(ufs, HCI_FORCE_HCS),
		 hci_readl(ufs, HCI_GPIO_OUT), phy_control,
		 hci_readl(ufs, HCI_FSM_MONITOR), hci_readl(ufs, HCI_UFS_ACG_DISABLE),
		 hci_readl(ufs, HCI_REQ_HOLD_EN));
	/*
	 * Only from .link_startup_notify, which is process context: the event
	 * callbacks run from the interrupt handler with host_lock held, where
	 * a DME_GET waits with interrupts off (it wedged the S9).
	 */
	if (sleepable) {
		u32 lanes = 0;
		int err = ufshcd_dme_get(hba, UIC_ARG_MIB(PA_AVAILRXDATALANES),
					 &lanes);

		dev_info(ufs->dev, "%s: live dme_get err=%d avail_rx=%u\n",
			 why, err, lanes);
	}
}

/*
 * The first fatal error is the one worth reading; the recovery that follows
 * floods the ramoops record. Stopping here keeps the record on the cause,
 * and the watchdog (nobody pets it after a panic) brings the phone back to
 * TWRP.
 */
static bool ufs_panic_on_fatal = true;
module_param_named(panic_on_fatal, ufs_panic_on_fatal, bool, 0644);
MODULE_PARM_DESC(panic_on_fatal, "stop at the first UFS fatal error");

static void exynos_ufs_event_notify(struct ufs_hba *hba,
				    enum ufs_event_type event, void *data)
{
	struct exynos9820_ufs *ufs = to_exynos_ufs(hba);
	u32 val;

	if (!ufs)
		return;
	val = *(u32 *)data;

	switch (event) {
	case UFS_EVT_LINK_STARTUP_FAIL:
		dev_warn(ufs->dev, "event: link startup failed %d\n", (int)val);
		exynos_ufs_dump_state(ufs, "link-fail");
		return;
	case UFS_EVT_FATAL_ERR:
		dev_err(ufs->dev, "event: fatal 0x%08x%s\n", val,
			val & 0x20000 ? " SYSTEM_BUS_FATAL_ERROR" : "");
		exynos_ufs_dump_state(ufs, "fatal");
		if (ufs_panic_on_fatal)
			panic("UFS fatal error 0x%08x", val);
		return;
	case UFS_EVT_HOST_RESET:
	case UFS_EVT_ABORT:
		dev_err(ufs->dev, "event: %s 0x%08x\n",
			event == UFS_EVT_ABORT ? "abort" : "host-reset", val);
		return;
	default:
		return;
	}
}

static const struct ufs_hba_variant_ops exynos9820_ufs_ops = {
	.name			= "exynos9820_ufs",
	.init			= exynos_ufs_init,
	.exit			= exynos_ufs_exit,
	.hce_enable_notify	= exynos_ufs_hce_enable_notify,
	.link_startup_notify	= exynos_ufs_link_startup_notify,
	.negotiate_pwr_mode	= exynos_ufs_negotiate_pwr_mode,
	.pwr_change_notify	= exynos_ufs_pwr_change_notify,
	.setup_clocks		= exynos_ufs_setup_clocks,
	.setup_xfer_req		= exynos_ufs_setup_xfer_req,
	.setup_task_mgmt	= exynos_ufs_setup_task_mgmt,
	.hibern8_notify		= exynos_ufs_hibern8_notify,
	.suspend		= exynos_ufs_suspend,
	.resume			= exynos_ufs_resume,
	.set_dma_mask		= exynos_ufs_set_dma_mask,
	.device_reset		= exynos_ufs_device_reset,
	.event_notify		= exynos_ufs_event_notify,
};

static int exynos9820_ufs_probe(struct platform_device *pdev)
{
	int ret = ufshcd_pltfrm_init(pdev, &exynos9820_ufs_ops);

	if (ret)
		dev_err(&pdev->dev, "ufshcd_pltfrm_init failed: %d\n", ret);
	return ret;
}

static void exynos9820_ufs_remove(struct platform_device *pdev)
{
	ufshcd_pltfrm_remove(pdev);
}

static const struct of_device_id exynos9820_ufs_match[] = {
	{ .compatible = "samsung,exynos9820-ufs" },
	{},
};
MODULE_DEVICE_TABLE(of, exynos9820_ufs_match);

static const struct dev_pm_ops exynos9820_ufs_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ufshcd_system_suspend, ufshcd_system_resume)
	SET_RUNTIME_PM_OPS(ufshcd_runtime_suspend, ufshcd_runtime_resume, NULL)
	.prepare	= ufshcd_suspend_prepare,
	.complete	= ufshcd_resume_complete,
};

static struct platform_driver exynos9820_ufs_driver = {
	.driver = {
		.name = "exynos9820-ufs",
		.of_match_table = exynos9820_ufs_match,
		.pm = &exynos9820_ufs_pm_ops,
	},
	.probe = exynos9820_ufs_probe,
	.remove = exynos9820_ufs_remove,
};
module_platform_driver(exynos9820_ufs_driver);

MODULE_DESCRIPTION("Exynos 9820 UFS host controller driver");
MODULE_LICENSE("GPL");
