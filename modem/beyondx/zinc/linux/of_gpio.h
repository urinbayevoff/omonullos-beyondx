/* stand-in: mainline dropped the integer GPIO lookups from the device tree.
 * The LTE modem's node has no GPIOs (stock/twrp-live.dts): its lookups find
 * none, as the stock driver did. The S5100's (modem2_pdata) are answered by
 * zrn_s5100_gpio.c. */
#pragma once
#include <linux/gpio.h>
#include <linux/of.h>
int zrn_s5100_gpio_lookup(const struct device_node *np, const char *name);
static inline int of_get_named_gpio(const struct device_node *np, const char *name, int index)
{
	return index ? -ENOENT : zrn_s5100_gpio_lookup(np, name);
}
static inline int of_get_gpio(const struct device_node *np, int index)
{
	return -ENOENT;
}
static inline int of_gpio_count(const struct device_node *np)
{
	return 0;
}
