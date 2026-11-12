// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/drb for Samsung's RIL: the misc device of the G977N kernel's
 * dev_ril_bridge (drivers/misc/dev_ril_bridge, Samsung Electronics, GPL),
 * cut down to what the RIL sees. The RIL writes SIPC frames to it (main_cmd
 * 0x27, CP channel info for the display/Wi-Fi drivers) and waits in select()
 * for frames from kernel drivers. Without the device the RIL created a plain
 * file /dev/drb, select() always returned readable, read() returned 0, and
 * its "SINR" thread kept a CPU core at 100 % (2026-10-18). No driver here
 * sends to the RIL, so reads wait (as stock, 100 ms) and writes are dropped.
 * Added for OmonullOS, October 2026.
 */
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/wait.h>

static DECLARE_WAIT_QUEUE_HEAD(drb_wq);

static __poll_t drb_poll(struct file *filp, struct poll_table_struct *wait)
{
	poll_wait(filp, &drb_wq, wait);
	return 0;
}

static ssize_t drb_read(struct file *filp, char __user *buf, size_t count, loff_t *pos)
{
	wait_event_interruptible_timeout(drb_wq, false, msecs_to_jiffies(100));
	return 0;
}

static ssize_t drb_write(struct file *filp, const char __user *data, size_t count, loff_t *pos)
{
	/* stock: a 7-byte sipc_fmt_hdr and at most 256 bytes */
	if (count <= 7 || count > 256)
		return -EFAULT;
	return count;
}

static const struct file_operations drb_fops = {
	.owner = THIS_MODULE,
	.open = nonseekable_open,
	.poll = drb_poll,
	.read = drb_read,
	.write = drb_write,
};

static struct miscdevice drb_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "drb",
	.fops = &drb_fops,
	.mode = 0660,
};

module_misc_device(drb_dev);

MODULE_DESCRIPTION("/dev/drb (Samsung dev_ril_bridge) for the RIL");
MODULE_LICENSE("GPL");
