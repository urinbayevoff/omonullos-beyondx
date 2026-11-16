// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-bbd-bx: the Galaxy S10 5G's sensor hub (Broadcom BCM4775 B0, called
 * "BCM4773" in Samsung's device tree: GNSS receiver and hub for the light,
 * proximity, motion and magnetic sensors) on a mainline kernel.
 *
 * Board glue for Broadcom's SPI transport and "bbd" device nodes
 * (sensors/driver/, as on the S9), in place of zrn_bbd_board.c. What differs
 * from the S9, all from the stock device tree (dtbo 08, BCM4773@0 on spi_17):
 *
 *  - the bus: spi@10970000, USI16 of PERIC1, mode register SYSREG_PERIC1
 *    0x1048 (stock label usi_16, "spi"), clocks CMU_PERIC1 USI16 (gates
 *    on at handover, 200 MHz from CLKCMU_PERIC1_IP, read 2026-11-15);
 *  - pins: gph0-0 clock, gph0-1/gph0-2 data, gph0-3 chip select on PERIC1;
 *    MCU request gpg4-1, MCU response gpg4-2, enable gpg0-2 on PERIC0;
 *    host request gpa1-7, SHUB_INT gpa4-0, AP_SLEEP gpa0-5 (ALIVE, gpiolib);
 *  - the supply: vdd_mcu_1p8 is S2MPS19 LDO21M, regulator-always-on in the
 *    stock tree and on at handover. Nothing here touches the PMIC;
 *    mcu_power only reports it on.
 *
 * gph0 is shared with the speaker amplifiers' bit-banged I2C on gph0-6/7
 * (snd-zrn-abox-bx), which drives those pins open-drain with DAT held 0 and
 * changes their direction under its own lock. So this file never writes
 * gph0's data register: the chip select is the controller's own (gph0-3 in
 * function 2, stock spi17-cs-func, driven through CS_REG), and gph0's
 * configuration register is written only when the bus is set up or parked.
 *
 * PERIC0's pin controller has no driver in this kernel; gpg0 and gpg4 are
 * offered here as a small GPIO chip ("zrn-bx-peric0") so the transport keeps
 * using descriptors.
 */
#include <linux/delay.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/irq.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/spi/spi.h>
#include <linux/io.h>
#include "bbd.h"

struct gpio_desc *zrn_gpios[ZRN_GPIO_COUNT];

int zrn_named_gpio(const char *name)
{
	if (!strcmp(name, "ssp-host-req"))
		return ZRN_GPIO_HOST_REQ;
	if (!strcmp(name, "ssp-mcu-req"))
		return ZRN_GPIO_MCU_REQ;
	if (!strcmp(name, "ssp-mcu-resp"))
		return ZRN_GPIO_MCU_RESP;
	return -ENOENT;
}

/* ---- gpg0 / gpg4 of PERIC0 as a GPIO chip ----------------------------------- */

#define PERIC0_PINS	0x10430000
#define GPG0		0x80
#define GPG4		0xe0
#define PIN_CON		0x00
#define PIN_DAT		0x04
#define PIN_PUD		0x08
#define PIN_DRV		0x0c

static void __iomem *peric0;
static DEFINE_RAW_SPINLOCK(peric0_lock);

/* lines 0-7: gpg0-0..7, lines 8-15: gpg4-0..7 */
static void __iomem *pg_bank(unsigned int off)
{
	return peric0 + (off < 8 ? GPG0 : GPG4);
}

static int pg_get_direction(struct gpio_chip *gc, unsigned int off)
{
	u32 f = (readl(pg_bank(off) + PIN_CON) >> ((off & 7) * 4)) & 0xf;

	return f == 1 ? GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static void pg_set_con(unsigned int off, u32 func)
{
	void __iomem *b = pg_bank(off);
	unsigned int sh = (off & 7) * 4;
	unsigned long flags;

	raw_spin_lock_irqsave(&peric0_lock, flags);
	writel((readl(b + PIN_CON) & ~(0xfU << sh)) | (func << sh), b + PIN_CON);
	raw_spin_unlock_irqrestore(&peric0_lock, flags);
}

static int pg_set(struct gpio_chip *gc, unsigned int off, int value)
{
	void __iomem *b = pg_bank(off);
	u32 bit = BIT(off & 7);
	unsigned long flags;
	u32 v;

	raw_spin_lock_irqsave(&peric0_lock, flags);
	v = readl(b + PIN_DAT);
	writel(value ? v | bit : v & ~bit, b + PIN_DAT);
	raw_spin_unlock_irqrestore(&peric0_lock, flags);
	return 0;
}

static int pg_get(struct gpio_chip *gc, unsigned int off)
{
	return !!(readl(pg_bank(off) + PIN_DAT) & BIT(off & 7));
}

static int pg_direction_input(struct gpio_chip *gc, unsigned int off)
{
	pg_set_con(off, 0);
	return 0;
}

static int pg_direction_output(struct gpio_chip *gc, unsigned int off, int value)
{
	pg_set(gc, off, value);
	pg_set_con(off, 1);
	return 0;
}

static struct gpio_chip pg_chip = {
	.label = "zrn-bx-peric0",
	.owner = THIS_MODULE,
	.base = -1,
	.ngpio = 16,
	.get_direction = pg_get_direction,
	.direction_input = pg_direction_input,
	.direction_output = pg_direction_output,
	.get = pg_get,
	.set = pg_set,
	.can_sleep = false,
};
static bool pg_added;

/* stock pin settings (dtbo 08, fragment@1): no pulls on the handshake pair
 * (MCU_RESP never reads high against the SoC's pull-down: the S9's lesson),
 * pull-down on the enable pin, drive 3 on all three */
static void pg_pins_setup(void)
{
	unsigned long flags;
	u32 v;

	raw_spin_lock_irqsave(&peric0_lock, flags);
	v = readl(peric0 + GPG4 + PIN_PUD) & ~(0xffU << 4);		/* gpg4-1, gpg4-2: none */
	writel(v, peric0 + GPG4 + PIN_PUD);
	v = readl(peric0 + GPG4 + PIN_DRV) & ~(0xffU << 4);
	writel(v | (0x33U << 4), peric0 + GPG4 + PIN_DRV);
	v = readl(peric0 + GPG0 + PIN_PUD) & ~(0xfU << 8);		/* gpg0-2: down */
	writel(v | (1U << 8), peric0 + GPG0 + PIN_PUD);
	v = readl(peric0 + GPG0 + PIN_DRV) & ~(0xfU << 8);
	writel(v | (3U << 8), peric0 + GPG0 + PIN_DRV);
	raw_spin_unlock_irqrestore(&peric0_lock, flags);
}

static struct gpiod_lookup_table hub_gpios = {
	.dev_id = "zrn-bbd",
	.table = {
		GPIO_LOOKUP("gpa1", 7, "host-req", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("zrn-bx-peric0", 8 + 1, "mcu-req", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("zrn-bx-peric0", 8 + 2, "mcu-resp", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("zrn-bx-peric0", 2, "gps-pwr", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpa0", 5, "ap-sleep", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpa4", 0, "shub-int", GPIO_ACTIVE_HIGH),
		{ }
	},
};

void zrn_mcu_req(int high)
{
	gpiod_set_value(zrn_gpios[ZRN_GPIO_MCU_REQ], high);
}

/* ---- the SPI controller --------------------------------------------------- */

/*
 * USI16 of PERIC1 (spi@10970000), driven by polling its FIFOs as on the S9.
 * The clock: CLKCMU_PERIC1_IP (200 MHz) through the USI16 user mux and an
 * undivided DIV; SPI clock = 200 MHz / (2 * (prescaler + 1)). Stock runs
 * the chip at up to 25 MHz (prescaler 3); 7 (12.5 MHz) to start with.
 */
#define SPI_BASE	0x10970000
#define SPI_CH_CFG	0x00
#define SPI_CLK_CFG	0x04
#define SPI_MODE_CFG	0x08
#define SPI_CS_REG	0x0c
#define SPI_STATUS	0x14
#define SPI_TX_DATA	0x18
#define SPI_RX_DATA	0x1c
#define SPI_FB_CLK	0x2c
#define USI_CON		0xc4
#define USI_OPTION	0xc8
#define USI16_MODE	0x10821048	/* SYSREG_PERIC1: 2 selects SPI */
#define CMU_PERIC1	0x10800000
#define GATE_USI16_RSTN	0x2080
#define GATE_USI16_IPCLK 0x2108
#define GATE_USI16_PCLK	0x210c
#define PERIC1_PINS	0x10830000
#define GPH0		0xe0

static void __iomem *spi_regs, *usi_mode, *gph0;
static DEFINE_RAW_SPINLOCK(gph0_con_lock);
static int prescaler = 7;
module_param(prescaler, int, 0444);
MODULE_PARM_DESC(prescaler, "SPI clock = 200 MHz / (2 * (prescaler + 1)); stock's 25 MHz is 3");
static bool force_gates;
module_param(force_gates, bool, 0444);
MODULE_PARM_DESC(force_gates, "hold USI16's three clock gates open by hand (they are on, auto-gated, at handover)");

/* gph0-0..3 to function 2 (the controller), or to inputs (their pull-downs,
 * gph0 PUD 0x..1111 at handover, hold them low); gph0-4..7 left alone */
static void gph0_bus(bool on)
{
	unsigned long flags;
	u32 v;

	raw_spin_lock_irqsave(&gph0_con_lock, flags);
	v = readl(gph0 + PIN_CON) & ~0xffffU;
	writel(on ? v | 0x2222 : v, gph0 + PIN_CON);
	raw_spin_unlock_irqrestore(&gph0_con_lock, flags);
}

/* Configures the controller; safe to call again (the reset path does, since
 * the pins are taken away from it while the chip is held in standby). */
static int hw_spi_setup(void)
{
	u32 v;

	if (!spi_regs)
		spi_regs = ioremap(SPI_BASE, 0x100);
	if (!usi_mode)
		usi_mode = ioremap(USI16_MODE, 4);
	if (!spi_regs || !usi_mode)
		return -ENOMEM;
	if (force_gates) {
		void __iomem *cmu = ioremap(CMU_PERIC1, 0x3000);

		if (!cmu)
			return -ENOMEM;
		writel(0x00300000, cmu + GATE_USI16_RSTN);
		writel(0x00300000, cmu + GATE_USI16_IPCLK);
		writel(0x00300000, cmu + GATE_USI16_PCLK);
		iounmap(cmu);
	}
	writel(2, usi_mode);
	v = readl(spi_regs + USI_CON);
	writel(v & ~1U, spi_regs + USI_CON);		/* out of reset */
	udelay(10);
	v = readl(spi_regs + USI_OPTION);
	writel((v & ~4U) | 2, spi_regs + USI_OPTION);	/* keep its clock running */
	writel(BIT(5), spi_regs + SPI_CH_CFG);		/* software reset */
	udelay(10);
	writel(0, spi_regs + SPI_CH_CFG);
	writel(BIT(3) | BIT(2), spi_regs + SPI_CH_CFG);	/* mode 3 */
	writel(BIT(8) | (prescaler & 0xff), spi_regs + SPI_CLK_CFG);
	writel(0, spi_regs + SPI_MODE_CFG);		/* bytes, no DMA */
	writel(0, spi_regs + SPI_FB_CLK);
	writel(1, spi_regs + SPI_CS_REG);		/* manual select, released */
	gph0_bus(true);
	pr_info("zrn-bbd: SPI controller: mode %x ch %08x clk %08x cs %x usi %08x/%08x\n", readl(usi_mode),
		readl(spi_regs + SPI_CH_CFG), readl(spi_regs + SPI_CLK_CFG), readl(spi_regs + SPI_CS_REG),
		readl(spi_regs + USI_CON), readl(spi_regs + USI_OPTION));
	return 0;
}

static int hw_spi_xfer(const u8 *tx, u8 *rx, unsigned int len)
{
	const u32 cfg = BIT(3) | BIT(2);

	/* a system sleep powers PERIC1 down: USI16 comes back in mode 0 with the
	 * SPI registers cleared, and every transfer then times out */
	if (readl(usi_mode) != 2) {
		pr_info("zrn-bbd: USI16 lost its setup (system sleep?), configuring again\n");
		hw_spi_setup();
	}
	writel(cfg | 3, spi_regs + SPI_CH_CFG);		/* both directions on */
	while (len) {
		unsigned int n = min(len, 32U), i, spins = 0;

		for (i = 0; i < n; i++)
			writel(tx ? tx[i] : 0, spi_regs + SPI_TX_DATA);
		while (((readl(spi_regs + SPI_STATUS) >> 15) & 0x1ff) < n) {
			if (++spins > 20000) {
				writel(cfg | BIT(5), spi_regs + SPI_CH_CFG);	/* empty the FIFOs */
				udelay(10);
				writel(cfg, spi_regs + SPI_CH_CFG);
				pr_err_ratelimited("zrn-bbd: SPI stuck (status %08x)\n", readl(spi_regs + SPI_STATUS));
				return -ETIMEDOUT;
			}
		}
		for (i = 0; i < n; i++) {
			u8 in = readl(spi_regs + SPI_RX_DATA);

			if (rx)
				rx[i] = in;
		}
		if (tx)
			tx += n;
		if (rx)
			rx += n;
		len -= n;
	}
	writel(cfg, spi_regs + SPI_CH_CFG);
	return 0;
}

static int hub_spi_transfer_one(struct spi_controller *ctlr, struct spi_device *spi,
		struct spi_transfer *t)
{
	return hw_spi_xfer(t->tx_buf, t->rx_buf, t->len);
}

/* called with true to release the chip (the line is active low) */
static void hub_spi_set_cs(struct spi_device *spi, bool release)
{
	writel(release ? 1 : 0, spi_regs + SPI_CS_REG);
}

static struct platform_device *hub_pdev;
static struct spi_controller *hub_ctlr;
static struct spi_device *hub_spi;
static struct class *sec_class;
static struct device *sec_hub, *sec_gps;
static struct kobject *pwr_kobj;

/* ---- the power files lhd expects ------------------------------------------ */

/* LDO21M is always on (stock: regulator-always-on); lhd only switches it on */
static ssize_t mcu_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "1\n");
}

static ssize_t mcu_power_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	int on;

	if (kstrtoint(buf, 0, &on))
		return -EINVAL;
	pr_info("zrn-bbd: mcu_power %d asked (LDO21M stays on)\n", on);
	return size;
}
static DEVICE_ATTR_RW(mcu_power);

/*
 * Resetting the chip, as on the S9 (sensors/HUB-COLDBOOT-REA.md) without the
 * supply cut: park every line we drive towards the chip low, hold it in
 * standby, then bring it up in Samsung's order: AP_SLEEP high, pins back,
 * enable pin high, 100 ms. If MCU_RESP is still high the hold is repeated.
 */
static unsigned int standby_ms = 1000;
module_param(standby_ms, uint, 0644);
MODULE_PARM_DESC(standby_ms, "how long the reset path holds the chip in standby with its lines parked");

static bool mcu_resp_high(void)
{
	return gpiod_get_value(zrn_gpios[ZRN_GPIO_MCU_RESP]) > 0;
}

static void hub_pins_park(void)
{
	zrn_mcu_req(0);
	gpiod_set_value(zrn_gpios[ZRN_GPIO_AP_SLEEP], 0);
	if (spi_regs)
		writel(1, spi_regs + SPI_CS_REG);
	gph0_bus(false);
}

static int hub_pins_restore(void)
{
	gpiod_set_value(zrn_gpios[ZRN_GPIO_AP_SLEEP], 1);
	return hw_spi_setup();
}

static int hub_cycle(unsigned int hold_ms)
{
	int ret;

	hub_pins_park();
	gpiod_set_value(zrn_gpios[ZRN_GPIO_GPS_PWR], 0);
	msleep(hold_ms);
	ret = hub_pins_restore();
	if (ret)
		return ret;
	gpiod_set_value(zrn_gpios[ZRN_GPIO_GPS_PWR], 1);
	msleep(100);
	return 0;
}

static int hub_power_sequence(unsigned int hold_ms)
{
	unsigned int hold = hold_ms;
	bool stale = mcu_resp_high();
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = hub_cycle(hold);
		pr_info("zrn-bbd: hub standby %u ms: MCU_RESP %d (was %d), SHUB_INT %d\n", hold,
			mcu_resp_high(), stale, gpiod_get_value(zrn_gpios[ZRN_GPIO_SHUB_INT]));
		if (ret || !mcu_resp_high())
			return ret;
		hold = max(hold * 3, standby_ms);
	}
	pr_warn("zrn-bbd: MCU_RESP still high with MCU_REQ low; the chip did not reset\n");
	return 0;
}

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	int reset;

	if (kstrtoint(buf, 0, &reset) || reset != 1)
		return -EINVAL;
	reset = hub_power_sequence(standby_ms);
	return reset ? reset : size;
}
static DEVICE_ATTR_WO(reset);

/* the lines, for bring-up: levels and the pin registers involved */
static ssize_t pins_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "host_req %d mcu_req %d mcu_resp %d gps_pwr %d ap_sleep %d shub_int %d\n"
		"gph0 con %08x dat %08x pud %08x | gpg0 con %08x dat %08x | gpg4 con %08x dat %08x pud %08x\n"
		"spi ch %08x clk %08x cs %08x status %08x usi %08x/%08x mode %x\n",
		gpiod_get_value(zrn_gpios[ZRN_GPIO_HOST_REQ]), gpiod_get_value(zrn_gpios[ZRN_GPIO_MCU_REQ]),
		gpiod_get_value(zrn_gpios[ZRN_GPIO_MCU_RESP]), gpiod_get_value(zrn_gpios[ZRN_GPIO_GPS_PWR]),
		gpiod_get_value(zrn_gpios[ZRN_GPIO_AP_SLEEP]), gpiod_get_value(zrn_gpios[ZRN_GPIO_SHUB_INT]),
		readl(gph0 + PIN_CON), readl(gph0 + PIN_DAT), readl(gph0 + PIN_PUD),
		readl(peric0 + GPG0 + PIN_CON), readl(peric0 + GPG0 + PIN_DAT),
		readl(peric0 + GPG4 + PIN_CON), readl(peric0 + GPG4 + PIN_DAT), readl(peric0 + GPG4 + PIN_PUD),
		readl(spi_regs + SPI_CH_CFG), readl(spi_regs + SPI_CLK_CFG), readl(spi_regs + SPI_CS_REG),
		readl(spi_regs + SPI_STATUS), readl(spi_regs + USI_CON), readl(spi_regs + USI_OPTION),
		readl(usi_mode));
}
static DEVICE_ATTR_RO(pins);

static ssize_t value_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", gpiod_get_value(zrn_gpios[ZRN_GPIO_GPS_PWR]));
}

static ssize_t value_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t size)
{
	int on;

	if (kstrtoint(buf, 0, &on))
		return -EINVAL;
	gpiod_set_value(zrn_gpios[ZRN_GPIO_GPS_PWR], !!on);
	pr_info("zrn-bbd: hub enable pin %d\n", !!on);
	return size;
}
static struct kobj_attribute value_attr = __ATTR(value, 0664, value_show, value_store);

/* ---- sensor packets, to and from user space (as on the S9) ---------------- */

static DEFINE_KFIFO(ssp_fifo, unsigned char, 16384);
static DEFINE_SPINLOCK(ssp_lock);
static DECLARE_WAIT_QUEUE_HEAD(ssp_wait);
static bool hub_ready;

/* each packet from the hub is queued as a two-byte length and its bytes */
static int ssp_on_packet(void *data, const char *buf, size_t size)
{
	unsigned char len[2] = { size & 0xff, size >> 8 };
	unsigned long flags;

	spin_lock_irqsave(&ssp_lock, flags);
	if (size < 0x10000 && kfifo_avail(&ssp_fifo) >= size + 2) {
		kfifo_in(&ssp_fifo, len, 2);
		kfifo_in(&ssp_fifo, buf, size);
	}
	spin_unlock_irqrestore(&ssp_lock, flags);
	wake_up_interruptible(&ssp_wait);
	return 0;
}

static int ssp_on_control(void *data, const char *str)
{
	pr_info("zrn-bbd: control: %s\n", str);
	return 0;
}

static int ssp_on_mcu_ready(void *data, bool ready)
{
	pr_info("zrn-bbd: hub %s\n", ready ? "ready" : "not ready");
	hub_ready = ready;
	wake_up_interruptible(&ssp_wait);
	return 0;
}

static int ssp_on_mcu_reset(void *data, bool no_response)
{
	pr_info("zrn-bbd: hub reset%s\n", no_response ? " (it had stopped answering)" : "");
	return 0;
}

static bbd_callbacks ssp_callbacks = {
	.on_packet = ssp_on_packet,
	.on_control = ssp_on_control,
	.on_mcu_ready = ssp_on_mcu_ready,
	.on_mcu_reset = ssp_on_mcu_reset,
};

static ssize_t ssp_read(struct file *file, char __user *buf, size_t size, loff_t *pos)
{
	unsigned int copied;
	int ret;

	if (kfifo_is_empty(&ssp_fifo)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (wait_event_interruptible(ssp_wait, !kfifo_is_empty(&ssp_fifo)))
			return -ERESTARTSYS;
	}
	ret = kfifo_to_user(&ssp_fifo, buf, size, &copied);
	return ret ? ret : copied;
}

static ssize_t ssp_write(struct file *file, const char __user *buf, size_t size, loff_t *pos)
{
	unsigned char pkt[1024];

	if (size > sizeof(pkt))
		return -EMSGSIZE;
	if (copy_from_user(pkt, buf, size))
		return -EFAULT;
	return bbd_send_packet(pkt, size);
}

static __poll_t ssp_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &ssp_wait, wait);
	return kfifo_is_empty(&ssp_fifo) ? 0 : EPOLLIN | EPOLLRDNORM;
}

static const struct file_operations ssp_fops = {
	.owner = THIS_MODULE,
	.read = ssp_read,
	.write = ssp_write,
	.poll = ssp_poll,
};

static struct miscdevice ssp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "zrn_ssp",
	.fops = &ssp_fops,
	.mode = 0660,
};
static bool misc_added;

static ssize_t ready_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", hub_ready);
}
static DEVICE_ATTR_RO(ready);

/* ---- what Samsung's ssp_suspend / ssp_resume do (as on the S9) ------------ */

static bool ap_sleep_pulse;
module_param(ap_sleep_pulse, bool, 0644);
MODULE_PARM_DESC(ap_sleep_pulse, "pulse AP_SLEEP low at each suspend and count the resumes for 0xd3 (as stock)");
static unsigned int ap_sleep_pulse_us = 100;
module_param(ap_sleep_pulse_us, uint, 0644);
MODULE_PARM_DESC(ap_sleep_pulse_us, "how long AP_SLEEP is held low");
static bool pulsed;
static unsigned long resume_seq, ap_suspends;

void zrn_ssp_suspend(void)
{
	struct irq_data *d = hub_spi && hub_spi->irq > 0 ? irq_get_irq_data(hub_spi->irq) : NULL;
	unsigned int us = ap_sleep_pulse_us;
	const char *why = NULL;
	int level;

	if (!ap_sleep_pulse)
		why = "off";
	else if (!hub_ready)
		why = "hub not ready";
	else if (!d || !irqd_is_wakeup_set(d))
		why = "host_req is not a wake IRQ";
	if (why) {
		pr_info("zrn-bbd: no AP_SLEEP pulse (%s)\n", why);
		return;
	}
	gpiod_set_value(zrn_gpios[ZRN_GPIO_AP_SLEEP], 0);
	usleep_range(us, us + 50);
	level = gpiod_get_value(zrn_gpios[ZRN_GPIO_AP_SLEEP]);
	gpiod_set_value(zrn_gpios[ZRN_GPIO_AP_SLEEP], 1);
	pr_info("zrn-bbd: AP_SLEEP pulse, read %d\n", level);
	pulsed = true;
	ap_suspends++;
}

void zrn_ssp_resume(void)
{
	gpiod_set_value(zrn_gpios[ZRN_GPIO_AP_SLEEP], 1);
	if (!pulsed)
		return;
	pulsed = false;
	resume_seq++;
	if (!IS_ERR_OR_NULL(sec_hub))
		sysfs_notify(&sec_hub->kobj, NULL, "resume_seq");
}

static ssize_t resume_seq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", resume_seq);
}
static DEVICE_ATTR_RO(resume_seq);

static ssize_t ap_suspends_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", ap_suspends);
}
static DEVICE_ATTR_RO(ap_suspends);

/* ---- putting it together ---------------------------------------------------- */

static void board_exit(void)
{
	if (misc_added)
		misc_deregister(&ssp_misc);
	if (hub_spi)
		spi_unregister_device(hub_spi);
	zrn_bcm_spi_exit();
	if (hub_ctlr)
		spi_unregister_controller(hub_ctlr);
	if (pwr_kobj) {
		sysfs_remove_file(pwr_kobj, &value_attr.attr);
		kobject_put(pwr_kobj);
	}
	if (!IS_ERR_OR_NULL(sec_gps))
		device_unregister(sec_gps);
	if (!IS_ERR_OR_NULL(sec_hub))
		device_unregister(sec_hub);
	if (!IS_ERR_OR_NULL(sec_class))
		class_destroy(sec_class);
	if (!IS_ERR_OR_NULL(hub_pdev)) {
		int i;

		for (i = 0; i < ZRN_GPIO_COUNT; i++)
			if (!IS_ERR_OR_NULL(zrn_gpios[i]))
				gpiod_put(zrn_gpios[i]);
		platform_device_unregister(hub_pdev);
	}
	gpiod_remove_lookup_table(&hub_gpios);
	if (pg_added)
		gpiochip_remove(&pg_chip);
	if (spi_regs) {
		writel(1, spi_regs + SPI_CS_REG);
		writel(BIT(5), spi_regs + SPI_CH_CFG);
		writel(0, spi_regs + SPI_CH_CFG);
		iounmap(spi_regs);
	}
	if (usi_mode)
		iounmap(usi_mode);
	if (gph0)
		iounmap(gph0);
	if (peric0)
		iounmap(peric0);
}

static int __init board_init(void)
{
	static const char * const names[ZRN_GPIO_COUNT] = { "host-req", "mcu-req", "mcu-resp", "gps-pwr",
		"ap-sleep", "shub-int" };
	/* "AP sleep" is held high (the AP is awake) from the start, as Samsung's
	 * sensor driver does at probe: with it low the chip does not come up.
	 * The enable pin starts low: the power sequence raises it. */
	static const enum gpiod_flags flags[ZRN_GPIO_COUNT] = { GPIOD_IN, GPIOD_OUT_LOW, GPIOD_IN, GPIOD_OUT_LOW,
		GPIOD_OUT_HIGH, GPIOD_IN };
	struct spi_board_info info = {
		.modalias = "ssp-spi",
		.max_speed_hz = 25000000,
		.mode = SPI_MODE_3,
	};
	int i, ret;

	peric0 = ioremap(PERIC0_PINS, 0x1000);
	gph0 = ioremap(PERIC1_PINS + GPH0, 0x20);
	if (!peric0 || !gph0) {
		ret = -ENOMEM;
		goto err;
	}
	pg_pins_setup();
	ret = gpiochip_add_data(&pg_chip, NULL);
	if (ret)
		goto err;
	pg_added = true;

	gpiod_add_lookup_table(&hub_gpios);
	hub_pdev = platform_device_register_simple("zrn-bbd", -1, NULL, 0);
	if (IS_ERR(hub_pdev)) {
		ret = PTR_ERR(hub_pdev);
		goto err;
	}
	for (i = 0; i <= ZRN_GPIO_SHUB_INT; i++) {
		zrn_gpios[i] = gpiod_get(&hub_pdev->dev, names[i], flags[i]);
		if (IS_ERR(zrn_gpios[i])) {
			ret = PTR_ERR(zrn_gpios[i]);
			pr_err("zrn-bbd: no %s GPIO: %d\n", names[i], ret);
			goto err;
		}
	}

	sec_class = class_create(THIS_MODULE, "sec");
	if (IS_ERR(sec_class)) {
		ret = PTR_ERR(sec_class);
		goto err;
	}
	sec_hub = device_create(sec_class, NULL, MKDEV(0, 0), NULL, "sensorhub");
	sec_gps = device_create(sec_class, NULL, MKDEV(0, 0), NULL, "gps");
	ret = -ENOMEM;
	if (IS_ERR(sec_hub) || IS_ERR(sec_gps))
		goto err;
	ret = device_create_file(sec_hub, &dev_attr_mcu_power);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_ready);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_resume_seq);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_ap_suspends);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_reset);
	if (!ret)
		ret = device_create_file(sec_hub, &dev_attr_pins);
	if (ret)
		goto err;
	pwr_kobj = kobject_create_and_add("GPS_PWR_EN", &sec_gps->kobj);
	ret = pwr_kobj ? sysfs_create_file(pwr_kobj, &value_attr.attr) : -ENOMEM;
	if (ret)
		goto err;

	ret = zrn_bcm_spi_init();
	if (ret)
		goto err;

	ret = hw_spi_setup();
	if (ret)
		goto err;
	hub_ctlr = spi_alloc_host(&hub_pdev->dev, 0);
	if (!hub_ctlr) {
		ret = -ENOMEM;
		goto err;
	}
	hub_ctlr->bus_num = 97;
	hub_ctlr->num_chipselect = 1;
	hub_ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	hub_ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	hub_ctlr->transfer_one = hub_spi_transfer_one;
	hub_ctlr->set_cs = hub_spi_set_cs;
	ret = spi_register_controller(hub_ctlr);
	if (ret) {
		spi_controller_put(hub_ctlr);
		hub_ctlr = NULL;
		goto err;
	}
	ret = -ENODEV;
	hub_spi = spi_new_device(hub_ctlr, &info);
	if (!hub_spi) {
		pr_err("zrn-bbd: could not declare the hub on the SPI bus\n");
		goto err;
	}
	/* after the probe: it clears the whole bbd state, callbacks included */
	bbd_register(NULL, &ssp_callbacks);
	ret = hub_power_sequence(mcu_resp_high() ? standby_ms : 100);
	if (ret)
		goto err;
	ret = misc_register(&ssp_misc);
	if (ret)
		goto err;
	misc_added = true;
	pr_info("zrn-bbd: transport up (S10 5G, SPI17); the hub wakes when lhd loads its patch\n");
	return 0;

err:
	board_exit();
	return ret;
}

module_init(board_init);
module_exit(board_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S10 5G sensor hub (BCM4775) transport: board glue for Broadcom's bbd driver");
