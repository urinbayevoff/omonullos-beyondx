// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-modem.ko: /dev/modem_boot_spi, through which the 5G cbd (-ts5100 -bs)
 * sends the S5100 its bootloader, as the vendor's boot_device_spi.c
 * (IOCTL_MODEM_XMIT_BOOT, then check_cp_status: CP2AP_WAKEUP high within
 * 200 x 20 ms). Mainline has no driver for this SoC's USI SPI, so the
 * transfer is done on SPI_CAM0 (0x10850000, pins gpc1-0..3) directly, as
 * modem/s5100/s5100boot.c from user space: OSCCLK 26 MHz, DIV /4 and the
 * block's own /4 (1.625 MHz, stock "spi-max-frequency" 1.62 MHz), 8 bit,
 * mode 0, automatic chip select, feedback delay 1. Polled, 32-byte FIFO
 * steps. Added for OmonullOS, October 2026.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "modem_prj.h"
#include "modem_utils.h"

#define SPI_BASE	0x10850000
#define  SPI_CH_CFG		0x00
#define   CH_TX_ON		BIT(0)
#define   CH_SW_RST		BIT(5)
#define  SPI_MODE_CFG		0x08
#define  SPI_CS_REG		0x0c
#define   CS_INACTIVE		BIT(0)
#define   CS_AUTO		BIT(1)
#define   CS_NSC_CNT_2		(2 << 4)
#define  SPI_INT_EN		0x10
#define  SPI_STATUS		0x14
#define   ST_TX_DONE		BIT(25)
#define   ST_FIFO_ERR		0x3c	/* TX/RX underrun, overrun */
#define  SPI_TX_DATA		0x18
#define  SPI_PENDING_CLR	0x24
#define  SPI_SWAP_CFG		0x28
#define  SPI_FB_CLK		0x2c
#define  SPI_USI_CON		0xc4
#define GPC1_BASE	0x10830080		/* PERIC1 pin controller, gpc1 */
#define CMU_PERIC1	0x10800000
#define  MUX_SPI_CAM0_USER	0x1a0		/* bit 4: 0 = OSCCLK */
#define  DIV_SPI_CAM0		0x1810		/* /(n + 1) */

#define FIFO_STEP	32

static struct {
	struct miscdevice misc;
	struct mutex lock;
	void __iomem *spi;
} sb;

static int spi_setup_hw(void)
{
	void __iomem *gpc1, *cmu;

	gpc1 = ioremap(GPC1_BASE, 0x20);
	cmu = ioremap(CMU_PERIC1, 0x2000);
	if (!gpc1 || !cmu) {
		if (gpc1)
			iounmap(gpc1);
		return -ENOMEM;
	}
	/* gpc1-0..3: SPI, no pull */
	writel((readl(gpc1) & ~0xffffu) | 0x2222, gpc1);
	writel(readl(gpc1 + 0x08) & ~0xffffu, gpc1 + 0x08);
	writel(readl(cmu + MUX_SPI_CAM0_USER) & ~BIT(4), cmu + MUX_SPI_CAM0_USER);
	writel((readl(cmu + DIV_SPI_CAM0) & ~0xfu) | 3, cmu + DIV_SPI_CAM0);
	iounmap(cmu);
	iounmap(gpc1);

	/* out of USI reset, software reset, byte mode, no DMA or interrupts */
	writel(0, sb.spi + SPI_USI_CON);
	writel(0, sb.spi + SPI_INT_EN);
	writel(0, sb.spi + SPI_SWAP_CFG);
	writel(CS_INACTIVE, sb.spi + SPI_CS_REG);
	writel(CH_SW_RST, sb.spi + SPI_CH_CFG);
	usleep_range(1000, 2000);
	writel(0, sb.spi + SPI_CH_CFG);
	writel(0x1f, sb.spi + SPI_PENDING_CLR);
	writel(0, sb.spi + SPI_PENDING_CLR);
	writel(0, sb.spi + SPI_MODE_CFG);
	writel(1, sb.spi + SPI_FB_CLK);
	return 0;
}

static int spi_send(const u8 *buf, u32 len)
{
	u32 i, k, end, st;
	int ret = 0;

	ret = spi_setup_hw();
	if (ret)
		return ret;
	writel(CS_AUTO | CS_NSC_CNT_2, sb.spi + SPI_CS_REG);
	writel(CH_TX_ON, sb.spi + SPI_CH_CFG);
	for (i = 0; i < len; i += FIFO_STEP) {
		end = min(i + FIFO_STEP, len);
		for (k = i; k < end; k++)
			writel(buf[k], sb.spi + SPI_TX_DATA);
		ret = readl_poll_timeout_atomic(sb.spi + SPI_STATUS, st, st & ST_TX_DONE, 1, 20000);
		if (ret || (st & ST_FIFO_ERR)) {
			mif_err("SPI stuck at %u of %u, status %08x\n", i, len, st);
			ret = ret ?: -EIO;
			break;
		}
		cond_resched();
	}
	writel(0, sb.spi + SPI_CH_CFG);
	writel(CS_INACTIVE, sb.spi + SPI_CS_REG);
	return ret;
}

/* boot_device_spi.c check_cp_status */
static int check_cp_status(int gpio, unsigned int count)
{
	unsigned int cnt;

	for (cnt = 0; cnt < count; cnt++) {
		if (gpio_get_value(gpio)) {
			mif_info("CP2AP_WAKEUP == 1 cnt: %d\n", cnt);
			if (!cnt)
				msleep(10);
			return 0;
		}
		msleep(20);
	}
	mif_err("ERR! CP2AP_WAKEUP == 0 (cnt %d)\n", cnt);
	return -EFAULT;
}

static long spi_boot_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct modem_firmware img;
	struct device_node *np;
	int gpio, ret;
	u8 *buf;

	if (cmd != IOCTL_MODEM_XMIT_BOOT)
		return -ENOIOCTLCMD;
	if (copy_from_user(&img, (const void __user *)arg, sizeof(img)))
		return -EFAULT;
	mif_info("IOCTL_MODEM_XMIT_BOOT (size %d)\n", img.size);
	if (!img.size || img.size > SZ_1M)
		return img.size ? -EINVAL : 0;

	np = of_find_compatible_node(NULL, NULL, "sec_modem,modem2_pdata");
	gpio = np ? of_get_named_gpio(np, "gpio_cp2ap_wake_up", 0) : -ENOENT;
	of_node_put(np);
	if (!gpio_is_valid(gpio)) {
		mif_err("ERR! gpio_cp_status is null\n");
		return 0;	/* as stock: nothing sent */
	}

	buf = vmalloc(img.size);
	if (!buf)
		return -ENOMEM;
	if (copy_from_user(buf, u64_to_user_ptr(img.binary), img.size)) {
		vfree(buf);
		return -EFAULT;
	}

	mutex_lock(&sb.lock);
	mif_info("Ignore CP2AP_WAKEUP Low\n");
	msleep(30);
	mif_info("Send CP bootloader with SPI : 0x%x bytes\n", img.size);
	ret = spi_send(buf, img.size);
	if (!ret) {
		ret = check_cp_status(gpio, 200);
		if (ret < 0)
			mif_err("ERR! check_cp_status fail (err %d)\n", ret);
	}
	mutex_unlock(&sb.lock);
	vfree(buf);
	return ret;
}

static const struct file_operations spi_boot_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = spi_boot_ioctl,
	.compat_ioctl = spi_boot_ioctl,
};

int zrn_spi_boot_init(void)
{
	mutex_init(&sb.lock);
	sb.spi = ioremap(SPI_BASE, 0x100);
	if (!sb.spi)
		return -ENOMEM;
	sb.misc.minor = MISC_DYNAMIC_MINOR;
	sb.misc.name = "modem_boot_spi";
	sb.misc.fops = &spi_boot_fops;
	return misc_register(&sb.misc);
}

void zrn_spi_boot_exit(void)
{
	misc_deregister(&sb.misc);
	iounmap(sb.spi);
}
