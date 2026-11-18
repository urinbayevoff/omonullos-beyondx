/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Galaxy S10 5G (beyondx): a boot menu in uniLoader, before the kernel.
 *
 * Added for OmonullOS, November 2026 (dual boot, docs/s10-5g/DUAL-BOOT.md
 * step 2). board-beyondx.c includes this file when ZBOOT_MENU is defined
 * (kernel/beyondx/uniloader-board.py --menu) and runs bx_boot_menu() as its
 * late_init, after the framebuffer is up and the failsafe watchdog is armed.
 *
 *   OmonullOS            uniLoader carries on and boots the kernel it holds
 *   TWRP (recovery)      INFORM3 = recovery, PMU SWRESET: S-Boot loads
 *                        RECOVERY, as `reboot recovery` does (zreboot-bx.c)
 *
 * Volume up/down move, power selects. Without a key press OmonullOS boots
 * after 5 s; after a key press the menu waits, but boots OmonullOS after
 * 60 s without one. A key already held when the menu appears (the power key
 * from switching the phone on) counts only after it is let go.
 *
 * Read only, apart from the framebuffer: the side keys' DAT registers on the
 * ALIVE pin controller (exynos9820-beyondx-keys.dtsi: vol up gpa0-3, vol
 * down gpa0-4, power gpa2-4, active low), the generic timer, and the WDT
 * count that the failsafe armed (fed while the menu waits). The pins'
 * function is not changed: if one is neither input (0x0) nor EINT (0xf),
 * as S-Boot leaves them, or the timer does not run, there is no menu and
 * OmonullOS boots as before. The only writes besides those are the reboot
 * into TWRP, the same four PMU writes zreboot-bx.c makes.
 */

/*
 * uniLoader runs with the MMU off: every access is to device memory, where
 * an unaligned one faults, and nothing in it uses the FP/SIMD registers (it
 * is not known that S-Boot leaves them enabled). GCC 14 -O2 vectorises the
 * fill loops and copies the countdown string with unaligned q-register
 * loads, so this file (and the board code after it) is built without either.
 */
#pragma GCC target("general-regs-only,strict-align")

#include <lib/video/font.h>

#define BX_ALIVE		0x15850000UL
#define BX_GPA0_CON		(BX_ALIVE + 0x00)
#define BX_GPA0_DAT		(BX_ALIVE + 0x04)
#define BX_GPA2_CON		(BX_ALIVE + 0x40)
#define BX_GPA2_DAT		(BX_ALIVE + 0x44)

#define BX_WDT_WTCNT		(0x10050000UL + 0x08)
#define BX_WDT_COUNT		23805		/* 30 s, as uniloader-failsafe.py */

#define BX_PMU			0x15860000UL
#define BX_RST_STAT		(BX_PMU + 0x0404)
#define BX_INFORM2		(BX_PMU + 0x0808)
#define BX_INFORM3		(BX_PMU + 0x080c)
#define BX_SWRESET		(BX_PMU + 0x3a00)
#define BX_REASON_RECOVERY	0x12345674
#define BX_SEC_REBOOT_NORMAL	0x4e

#define KEY_UP			1u
#define KEY_DOWN		2u
#define KEY_PWR			4u

/* layout on the 1440x3040 panel; colours are 0xAARRGGBB */
#define FB_W			1440
#define FB_H			3040
#define MARGIN			96
#define TILE_W			(FB_W - 2 * MARGIN)
#define TILE_H			260
#define TILE_Y0			900
#define TILE_GAP		40
#define COUNT_Y			(TILE_Y0 + 2 * (TILE_H + TILE_GAP) + 80)
#define C_BG			0xff000000u
#define C_TILE			0xff262626u
#define C_ACCENT		0xff0050efu	/* Metro cobalt */
#define C_TEXT			0xffffffffu
#define C_DIM			0xff8a8a8au

static const char *const bx_items[] = { "OmonullOS", "TWRP (recovery)" };
#define N_ITEMS			2

static inline unsigned int bx_rd(unsigned long a)
{
	return *(volatile unsigned int *)a;
}

static inline void bx_wr(unsigned long a, unsigned int v)
{
	*(volatile unsigned int *)a = v;
}

static inline unsigned long bx_ticks(void)
{
	unsigned long v;

	asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v) :: "memory");
	return v;
}

static unsigned long bx_freq(void)
{
	unsigned long v;

	asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
	if (v < 1000000UL || v > 100000000UL)
		v = 26000000UL;		/* OSCCLK */
	return v;
}

/* --- drawing: 32-bit aligned stores only (MMU off, device memory) --- */

static volatile unsigned int *const bx_fb = (volatile unsigned int *)0xca000000UL;

static void bx_fill(int x, int y, int w, int h, unsigned int c)
{
	for (int j = y; j < y + h; j++) {
		volatile unsigned int *p = bx_fb + (unsigned long)j * FB_W + x;

		for (int i = 0; i < w; i++)
			p[i] = c;
	}
}

/* foreground pixels only: the background is filled first */
static void bx_text(int x, int y, int scale, const char *s, unsigned int c)
{
	for (; *s; s++, x += FONTW * scale) {
		const unsigned char *g = letters[font_index(*s)];

		for (int row = 0; row < FONTH; row++)
			for (int col = 0; col < FONTW; col++)
				if (g[row] & (0x80 >> col))
					bx_fill(x + col * scale, y + row * scale,
						scale, scale, c);
	}
}

static void bx_draw_tile(int i, int selected)
{
	int y = TILE_Y0 + i * (TILE_H + TILE_GAP);
	int scale = 7;

	bx_fill(MARGIN, y, TILE_W, TILE_H, selected ? C_ACCENT : C_TILE);
	bx_text(MARGIN + 60, y + (TILE_H - FONTH * scale) / 2, scale,
		bx_items[i], C_TEXT);
}

static void bx_draw_count(int secs)
{
	char line[] = "5 soniyadan keyin: OmonullOS";

	bx_fill(0, COUNT_Y, FB_W, FONTH * 5, C_BG);
	if (secs <= 0)
		return;
	line[0] = (char)('0' + (secs > 9 ? 9 : secs));
	bx_text(MARGIN, COUNT_Y, 5, line, C_DIM);
}

static void bx_draw_menu(int sel)
{
	bx_fill(0, 0, FB_W, FB_H, C_BG);
	bx_text(MARGIN, 420, 10, "OmonullOS", C_TEXT);
	bx_text(MARGIN, 640, 5, "yuklash menyusi", C_DIM);
	for (int i = 0; i < N_ITEMS; i++)
		bx_draw_tile(i, i == sel);
	bx_text(MARGIN, 2560, 4, "Ovoz + / -  tanlash", C_DIM);
	bx_text(MARGIN, 2660, 4, "Quvvat      kirish", C_DIM);
}

/* --- keys --- */

static int bx_pin_ok(unsigned int con, int pin)
{
	unsigned int f = (con >> (4 * pin)) & 0xf;

	return f == 0x0 || f == 0xf;
}

static unsigned int bx_keys(void)
{
	unsigned int a = bx_rd(BX_GPA0_DAT), b = bx_rd(BX_GPA2_DAT), k = 0;

	if (!(a & (1u << 3)))
		k |= KEY_UP;
	if (!(a & (1u << 4)))
		k |= KEY_DOWN;
	if (!(b & (1u << 4)))
		k |= KEY_PWR;
	return k;
}

static void bx_reboot_recovery(void)
{
	unsigned int v = bx_rd(BX_INFORM2);

	bx_wr(BX_INFORM2, (v & ~(0xffu << 16)) | (BX_SEC_REBOOT_NORMAL << 16));
	bx_wr(BX_INFORM3, BX_REASON_RECOVERY);
	bx_wr(BX_RST_STAT, 0);
	bx_wr(BX_SWRESET, 1u << 1);
	/* not fed any more: if SWRESET did not take, the watchdog resets
	 * into INFORM3, which is recovery as well */
	for (;;)
		;
}

int bx_boot_menu(void)
{
	unsigned long f, ms, t0, now, deadline, last_key;
	unsigned int raw, prev, stable, last;
	int sel = 0, counting = 1, shown = 5, same = 0;

	if (!bx_pin_ok(bx_rd(BX_GPA0_CON), 3) || !bx_pin_ok(bx_rd(BX_GPA0_CON), 4) ||
	    !bx_pin_ok(bx_rd(BX_GPA2_CON), 4))
		return 0;

	f = bx_freq();
	ms = f / 1000;
	t0 = bx_ticks();
	for (volatile int i = 0; i < 1000000; i++)
		;
	if (bx_ticks() == t0)
		return 0;	/* no timer: no way to time out, so no menu */

	bx_draw_menu(sel);
	bx_draw_count(shown);

	prev = stable = last = bx_keys();	/* held now: ignored until let go */
	now = bx_ticks();
	deadline = now + 5 * f;
	last_key = now;

	for (;;) {
		unsigned int pressed;

		bx_wr(BX_WDT_WTCNT, BX_WDT_COUNT);

		raw = bx_keys();
		same = raw == prev ? same + 1 : 0;
		prev = raw;
		if (same >= 3)		/* 30 ms the same: debounced */
			stable = raw;
		pressed = stable & ~last;
		last = stable;

		now = bx_ticks();
		if (pressed) {
			last_key = now;
			if (counting) {
				counting = 0;
				bx_draw_count(0);
			}
			if (pressed & KEY_PWR)
				break;
			if (pressed & (KEY_UP | KEY_DOWN)) {
				int old = sel;

				sel = (pressed & KEY_UP) ? (sel + N_ITEMS - 1) % N_ITEMS
							 : (sel + 1) % N_ITEMS;
				bx_draw_tile(old, 0);
				bx_draw_tile(sel, 1);
			}
		}

		if (counting) {
			int left;

			if (now >= deadline)
				break;		/* sel is still 0 */
			left = (int)((deadline - now + f - 1) / f);
			if (left != shown) {
				shown = left;
				bx_draw_count(shown);
			}
		} else if (now - last_key > 60 * f) {
			sel = 0;
			break;
		}

		t0 = bx_ticks();
		while (bx_ticks() - t0 < 10 * ms)
			;
	}

	if (sel == 1) {
		bx_fill(0, 0, FB_W, FB_H, C_BG);
		bx_text(MARGIN, 1400, 6, "TWRP ...", C_TEXT);
		bx_reboot_recovery();
	}

	bx_fill(0, 0, FB_W, FB_H, C_BG);
	return 0;
}
