// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-modem.ko: the S5100 5G modem's control pins as a GPIO chip, for the
 * vendor's modem_ctrl_s5100.c and the SPI boot device. Mainline has no pin
 * controller for the Exynos 9820's CMGP (gpm0..31, 0x15c30000) and PERIC1
 * (gph1, 0x10830000) blocks, so these nine pins are driven here directly,
 * 32-bit register accesses only. The two inputs the CP signals with
 * (CP2AP_WAKEUP gpm30, PHONE_ACTIVE gpm28) are interrupts: CMGP wakeup EINTs
 * (exynos7 layout: ECON 0x700, MASK 0x900, PEND 0xa00, 4 bytes per bank),
 * each bank on a GIC line of its own (stock: gpm28 SPI 0x71, gpm30 SPI 0x73,
 * in the overlay's "omonullos,s5100-gpio" node).
 *
 * The integer GPIO lookups of the device tree are gone from mainline; the
 * vendor driver's of_get_named_gpio() on modem2_pdata ends up in
 * zrn_s5100_gpio_lookup() (zinc/linux/of_gpio.h). Pin map from the stock
 * dtbo's mif2_pdata and its pinctrl (evt1_dual). Added for OmonullOS,
 * October 2026.
 */
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define CMGP_BASE	0x15c30000
#define GPH1_BASE	0x10830100
#define GPM(n)		(CMGP_BASE + (n) * 0x20)

/* bank registers: 4 bits per pin in CON/PUD/DRV, 1 in DAT */
#define BANK_CON	0x00
#define BANK_DAT	0x04
#define BANK_PUD	0x08
#define  FUNC_IN	0x0
#define  FUNC_OUT	0x1
#define  FUNC_EINT	0xf

/* CMGP wakeup EINT, per gpm bank n at +4n */
#define EINT_CON	0x700
#define EINT_MASK	0x900
#define EINT_PEND	0xa00
#define  EINT_LEVEL_LOW		0
#define  EINT_LEVEL_HIGH	1
#define  EINT_EDGE_FALLING	2
#define  EINT_EDGE_RISING	3
#define  EINT_EDGE_BOTH		4

struct zrn_pin {
	const char *name;	/* the stock mif2_pdata property */
	phys_addr_t bank;
	u8 pin;
	s8 eint;		/* gpm bank number with a wakeup EINT, or -1 */
};

static const struct zrn_pin zrn_pins[] = {
	{ "gpio_ap2cp_cp_pwr_on",	GPM(20),   0, -1 },
	{ "gpio_ap2cp_nreset_n",	GPH1_BASE, 4, -1 },
	{ "gpio_cp2ap_cp_ps_hold",	GPM(31),   0, -1 },
	{ "gpio_cp2ap_wake_up",		GPM(30),   0, 30 },
	{ "gpio_ap2cp_dump_noti",	GPM(25),   0, -1 },
	{ "gpio_ap2cp_pda_active",	GPH1_BASE, 3, -1 },
	{ "gpio_cp2ap_phone_active",	GPM(28),   0, 28 },
	{ "gpio_ap2cp_wake_up",		GPM(17),   0, -1 },
};

#define NPINS	ARRAY_SIZE(zrn_pins)

struct zrn_s5100_gpio {
	struct gpio_chip gc;
	void __iomem *cmgp;		/* whole CMGP block: banks and EINT */
	void __iomem *gph1;
	int parent[NPINS];		/* GIC virq of an EINT pin, else 0 */
	raw_spinlock_t lock;
};

static struct zrn_s5100_gpio *zrn_gpio;

static void __iomem *bank(struct zrn_s5100_gpio *g, unsigned int i)
{
	const struct zrn_pin *p = &zrn_pins[i];

	return p->bank == GPH1_BASE ? g->gph1 : g->cmgp + (p->bank - CMGP_BASE);
}

static void field(struct zrn_s5100_gpio *g, unsigned int i, u32 reg, u32 width, u32 val)
{
	void __iomem *r = bank(g, i) + reg;
	u32 shift = zrn_pins[i].pin * width, mask = GENMASK(width - 1, 0) << shift;
	unsigned long flags;

	raw_spin_lock_irqsave(&g->lock, flags);
	writel((readl(r) & ~mask) | (val << shift & mask), r);
	raw_spin_unlock_irqrestore(&g->lock, flags);
}

static int zrn_get(struct gpio_chip *gc, unsigned int i)
{
	struct zrn_s5100_gpio *g = gpiochip_get_data(gc);

	return readl(bank(g, i) + BANK_DAT) >> zrn_pins[i].pin & 1;
}

static int zrn_set(struct gpio_chip *gc, unsigned int i, int val)
{
	field(gpiochip_get_data(gc), i, BANK_DAT, 1, !!val);
	return 0;
}

static int zrn_dir_in(struct gpio_chip *gc, unsigned int i)
{
	struct zrn_s5100_gpio *g = gpiochip_get_data(gc);

	/* an EINT pin keeps its function: its DAT still reads the level */
	if (zrn_pins[i].eint < 0)
		field(g, i, BANK_CON, 4, FUNC_IN);
	return 0;
}

static int zrn_dir_out(struct gpio_chip *gc, unsigned int i, int val)
{
	struct zrn_s5100_gpio *g = gpiochip_get_data(gc);

	field(g, i, BANK_DAT, 1, !!val);
	field(g, i, BANK_CON, 4, FUNC_OUT);
	return 0;
}

static int zrn_get_dir(struct gpio_chip *gc, unsigned int i)
{
	struct zrn_s5100_gpio *g = gpiochip_get_data(gc);
	u32 con = readl(bank(g, i) + BANK_CON) >> (4 * zrn_pins[i].pin) & 0xf;

	return con == FUNC_OUT ? GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

/* ---- EINT */

static void __iomem *eint(struct zrn_s5100_gpio *g, unsigned int i, u32 reg)
{
	return g->cmgp + reg + 4 * zrn_pins[i].eint;
}

static void zrn_irq_ack(struct irq_data *d)
{
	struct zrn_s5100_gpio *g = gpiochip_get_data(irq_data_get_irq_chip_data(d));

	writel(1, eint(g, d->hwirq, EINT_PEND));
}

static void zrn_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct zrn_s5100_gpio *g = gpiochip_get_data(gc);

	writel(1, eint(g, d->hwirq, EINT_MASK));
	gpiochip_disable_irq(gc, d->hwirq);
}

static void zrn_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct zrn_s5100_gpio *g = gpiochip_get_data(gc);

	gpiochip_enable_irq(gc, d->hwirq);
	writel(0, eint(g, d->hwirq, EINT_MASK));
}

static int zrn_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct zrn_s5100_gpio *g = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	u32 con;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_LEVEL_LOW:	con = EINT_LEVEL_LOW; break;
	case IRQ_TYPE_LEVEL_HIGH:	con = EINT_LEVEL_HIGH; break;
	case IRQ_TYPE_EDGE_FALLING:	con = EINT_EDGE_FALLING; break;
	case IRQ_TYPE_EDGE_RISING:	con = EINT_EDGE_RISING; break;
	case IRQ_TYPE_EDGE_BOTH:	con = EINT_EDGE_BOTH; break;
	default:
		return -EINVAL;
	}
	if (type & IRQ_TYPE_EDGE_BOTH)
		irq_set_handler_locked(d, handle_edge_irq);
	else
		irq_set_handler_locked(d, handle_level_irq);
	writel(con, eint(g, d->hwirq, EINT_CON));
	field(g, d->hwirq, BANK_CON, 4, FUNC_EINT);
	return 0;
}

static int zrn_irq_reqres(struct irq_data *d)
{
	if (zrn_pins[d->hwirq].eint < 0)
		return -EINVAL;
	return gpiochip_reqres_irq(irq_data_get_irq_chip_data(d), d->hwirq);
}

static void zrn_irq_relres(struct irq_data *d)
{
	gpiochip_relres_irq(irq_data_get_irq_chip_data(d), d->hwirq);
}

static const struct irq_chip zrn_irq_chip = {
	.name = "zrn-s5100-eint",
	.irq_ack = zrn_irq_ack,
	.irq_mask = zrn_irq_mask,
	.irq_unmask = zrn_irq_unmask,
	.irq_set_type = zrn_irq_set_type,
	.irq_request_resources = zrn_irq_reqres,
	.irq_release_resources = zrn_irq_relres,
	/* no suspend yet: the vendor driver's enable/disable_irq_wake pairs
	 * just succeed */
	.flags = IRQCHIP_IMMUTABLE | IRQCHIP_SKIP_SET_WAKE,
};

static void zrn_parent_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct zrn_s5100_gpio *g = gpiochip_get_data(gc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int i, irq = irq_desc_get_irq(desc);

	chained_irq_enter(chip, desc);
	for (i = 0; i < NPINS; i++)
		if (g->parent[i] == irq)
			generic_handle_domain_irq(gc->irq.domain, i);
	chained_irq_exit(chip, desc);
}

static int zrn_s5100_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zrn_s5100_gpio *g;
	struct gpio_irq_chip *girq;
	unsigned int i, n = 0;
	int *parents, ret;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	parents = devm_kcalloc(dev, NPINS, sizeof(*parents), GFP_KERNEL);
	if (!g || !parents)
		return -ENOMEM;
	raw_spin_lock_init(&g->lock);
	/* shared pin controllers, not requested */
	g->cmgp = devm_ioremap(dev, CMGP_BASE, 0x1000);
	g->gph1 = devm_ioremap(dev, GPH1_BASE, 0x20);
	if (!g->cmgp || !g->gph1)
		return -ENOMEM;

	for (i = 0; i < NPINS; i++) {
		if (zrn_pins[i].eint < 0)
			continue;
		ret = platform_get_irq(pdev, n);
		if (ret < 0)
			return ret;
		/* masked and quiet until a driver asks */
		writel(1, eint(g, i, EINT_MASK));
		writel(1, eint(g, i, EINT_PEND));
		g->parent[i] = parents[n++] = ret;
	}

	g->gc.label = "zrn-s5100";
	g->gc.parent = dev;
	g->gc.owner = THIS_MODULE;
	g->gc.base = -1;
	g->gc.ngpio = NPINS;
	g->gc.can_sleep = false;
	g->gc.get = zrn_get;
	g->gc.set = zrn_set;
	g->gc.direction_input = zrn_dir_in;
	g->gc.direction_output = zrn_dir_out;
	g->gc.get_direction = zrn_get_dir;

	girq = &g->gc.irq;
	gpio_irq_chip_set_chip(girq, &zrn_irq_chip);
	girq->parent_handler = zrn_parent_handler;
	girq->num_parents = n;
	girq->parents = parents;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_level_irq;

	ret = devm_gpiochip_add_data(dev, &g->gc, g);
	if (ret)
		return ret;
	zrn_gpio = g;
	dev_info(dev, "S5100 control pins at GPIO %d..%d\n", g->gc.base, g->gc.base + NPINS - 1);
	return 0;
}

/* of_get_named_gpio() on modem2_pdata: the legacy GPIO number of a pin, by its property */
int zrn_s5100_gpio_lookup(const struct device_node *np, const char *name)
{
	unsigned int i;

	if (!zrn_gpio || !of_device_is_compatible(np, "sec_modem,modem2_pdata"))
		return -ENOENT;
	for (i = 0; i < NPINS; i++)
		if (!strcmp(zrn_pins[i].name, name))
			return zrn_gpio->gc.base + i;
	return -ENOENT;
}

static const struct of_device_id zrn_s5100_gpio_match[] = {
	{ .compatible = "omonullos,s5100-gpio" },
	{ },
};

static struct platform_driver zrn_s5100_gpio_drv = {
	.probe = zrn_s5100_gpio_probe,
	.driver = {
		.name = "zrn-s5100-gpio",
		.of_match_table = zrn_s5100_gpio_match,
		.suppress_bind_attrs = true,
	},
};
struct platform_driver *zrn_s5100_gpio_driver = &zrn_s5100_gpio_drv;
