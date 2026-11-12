#!/usr/bin/env python3
"""exynos5-hsi2c: no late interrupt on a message that timed out.

After "tx timeout" the transfer returns, but i2c->msg still points at the
caller's message; a TX_ALMOSTEMPTY interrupt arriving later read its buffer
and the kernel panicked in exynos5_i2c_irq (15d60000, about one boot in 20,
zlastlog 20261020T002743). The transfer now masks and clears the interrupts
and drops i2c->msg under the lock; the handler ignores interrupts without one.
"""
from pathlib import Path
import sys

path = Path(sys.argv[1]) / 'drivers/i2c/busses/i2c-exynos5.c'
s = path.read_text()
if 'exynos5_i2c_xfer_msg: no late interrupt' in s:
    sys.exit(0)

def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit('patch-i2c-exynos5-late-irq: no unique match for:\n' + old)
    s = s.replace(old, new)

rep('''	int_status = readl(i2c->regs + HSI2C_INT_STATUS);
	writel(int_status, i2c->regs + HSI2C_INT_STATUS);
''', '''	int_status = readl(i2c->regs + HSI2C_INT_STATUS);
	writel(int_status, i2c->regs + HSI2C_INT_STATUS);

	/* late, after the transfer gave up on its message */
	if (!i2c->msg) {
		writel(0, i2c->regs + HSI2C_INT_ENABLE);
		exynos5_i2c_clr_pend_irq(i2c);
		spin_unlock(&i2c->lock);
		return IRQ_HANDLED;
	}
''')
rep('''	if (time_left == 0)
		ret = -ETIMEDOUT;
	else
		ret = i2c->state;
''', '''	if (time_left == 0)
		ret = -ETIMEDOUT;
	else
		ret = i2c->state;

	/* exynos5_i2c_xfer_msg: no late interrupt may touch msgs once we return */
	spin_lock_irqsave(&i2c->lock, flags);
	writel(0, i2c->regs + HSI2C_INT_ENABLE);
	exynos5_i2c_clr_pend_irq(i2c);
	i2c->msg = NULL;
	spin_unlock_irqrestore(&i2c->lock, flags);
''')
rep('''			      struct i2c_msg *msgs, int stop)
{
	unsigned long time_left;
	int ret;
''', '''			      struct i2c_msg *msgs, int stop)
{
	unsigned long time_left, flags;
	int ret;
''')
path.write_text(s)
