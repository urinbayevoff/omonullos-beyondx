/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Samsung Galaxy S10 5G (beyondx, SM-G977N), for uniLoader.
 *
 * Added for OmonullOS, October 2026, from uniLoader's board-beyond1lte.c
 * (Copyright (c) 2022, David Wronek; 2024, Ivaylo Ivanov; 2026, faveoled):
 * the same SoC, panel size and DECON. The framebuffer address is the S10's
 * (uniLoader's beyond1lte, Mu-Silicium's beyond1lte memory map), and this
 * phone's S-Boot passes s3cfb.bootloaderfb=0xca000000 to the kernel.
 * kernel/uniloader-failsafe.py adds the fallback writes to beyondx_init.
 * Modified for OmonullOS, November 2026: the boot menu (bootmenu-bx.c) as
 * late_init when ZBOOT_MENU is defined (uniloader-board.py --menu).
 */

#include <board.h>
#include <util.h>
#include <drivers/framework.h>
#include <lib/simplefb.h>

#ifdef ZBOOT_MENU
#include "bootmenu-bx.c"
#endif

#define DECON_F_BASE		0x19030000
#define HW_SW_TRIG_CONTROL	0x70

// Early initialization
int beyondx_init(void)
{
	/* Allow framebuffer to be written to */
	*(int*) (DECON_F_BASE + HW_SW_TRIG_CONTROL) = 0x1281;
	return 0;
}

static struct video_info beyondx_fb = {
	.format = FB_FORMAT_ARGB8888,
	.width = 1440,
	.height = 3040,
	.stride = 4,
	.address = (void *)0xca000000
};

static const struct device beyondx_devices[] = {
	{ "simplefb", &beyondx_fb, "fb" },
};

struct board_data board_ops = {
	.name = "samsung-beyondx",
	.ops = {
		.early_init = beyondx_init,
#ifdef ZBOOT_MENU
		.late_init = bx_boot_menu,
#endif
	},
	.devices = beyondx_devices,
	.num_devices = ARRAY_SIZE(beyondx_devices),
	.quirks = 0
};
