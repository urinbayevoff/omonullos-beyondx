/*
 * s5100boot: power the Galaxy S10 5G's 5G modem (S5100) and send it its
 * bootloader over SPI from user space, as Samsung's modem_ctrl_s5100.c
 * (s5100_on) and boot_device_spi.c (IOCTL_MODEM_XMIT_BOOT) do; then wait
 * for CP2AP_WAKEUP (gpm30), as check_cp_status.
 *
 *   s5100boot [radio2 device]     power on, send BOOT from radio2's TOC
 *   s5100boot off                 reset low, power off
 *   s5100boot wake 0|1            AP2CP_WAKEUP (gpm17): 1 before the PCIe
 *                                 link comes up, as s5100_runtime_resume
 *
 * Registers through /dev/mem with real 32-bit accesses (Python's mmap
 * writes device registers a byte at a time, which the SPI FIFO takes as
 * several frames). Pins and sequence from the stock dtbo (mif2_pdata) and
 * driver; SPI_CAM0 (0x10850000, gpc1-0..3) at 1.625 MHz: OSCCLK 26 MHz,
 * DIV_CLK_PERIC1_SPI_CAM0 /4, and the SPI block's own /4. 8-bit, mode 0,
 * auto chip select (stock "samsung,spi-chip-select-mode = 1").
 * Added for OmonullOS, October 2026.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int memfd;

static volatile uint32_t *map(uint32_t base)
{
	void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, base);
	if (p == MAP_FAILED) { perror("mmap"); exit(1); }
	return p;
}

static void msleep(int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

static volatile uint32_t *gph1, *gpm, *gpc1, *spi, *cmu, *cmu1;

/* bank: CON at +0, DAT at +4 */
static void out(volatile uint32_t *bank, int pin, int val)
{
	bank[1] = (bank[1] & ~(1u << pin)) | ((uint32_t)val << pin);
	bank[0] = (bank[0] & ~(0xfu << 4 * pin)) | (1u << 4 * pin);
}
#define GPM(n) (gpm + ((n) * 0x20) / 4)	/* gpmN bank, one pin each */
static int cp2ap_wakeup(void) { return GPM(30)[1] & 1; }

static void power(int on)
{
	if (!on) {
		out(gph1, 4, 0);		/* AP2CP_NRESET_N */
		out(GPM(20), 0, 0);		/* CP_PWR_ON */
		return;
	}
	out(GPM(17), 0, 0);			/* AP2CP_WAKE_UP */
	out(gph1, 3, 1);			/* AP2CP_PDA_ACTIVE */
	out(GPM(25), 0, 0);			/* AP2CP_DUMP_NOTI */
	out(gph1, 4, 0);
	out(GPM(20), 0, 1);
	msleep(50);
	out(gph1, 4, 1);
}

static uint8_t *boot_image(const char *dev, uint32_t *len)
{
	uint8_t toc[0x200];
	FILE *f = fopen(dev, "rb");
	if (!f || fread(toc, 1, sizeof(toc), f) != sizeof(toc)) { perror(dev); exit(1); }
	for (int i = 0; i < 0x200; i += 32) {
		uint32_t off, size;
		if (strncmp((char *)toc + i, "BOOT", 12))
			continue;
		memcpy(&off, toc + i + 12, 4);
		memcpy(&size, toc + i + 20, 4);
		uint8_t *img = malloc(size);
		if (fseek(f, off, SEEK_SET) || fread(img, 1, size, f) != size) { perror("read"); exit(1); }
		fclose(f);
		*len = size;
		return img;
	}
	fprintf(stderr, "no BOOT in %s's TOC\n", dev);
	exit(1);
}

int main(int argc, char **argv)
{
	memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd < 0) { perror("/dev/mem"); return 1; }
	gph1 = map(0x10830000) + 0x100 / 4;
	gpc1 = map(0x10830000) + 0x080 / 4;
	gpm = map(0x15c30000);
	spi = map(0x10850000);
	cmu = map(0x10800000);			/* CMU_PERIC1 */
	cmu1 = map(0x10801000);

	if (argc > 1 && !strcmp(argv[1], "off")) {
		power(0);
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "wake")) {
		out(GPM(17), 0, atoi(argv[2]) != 0);
		printf("AP2CP_WAKEUP %d, CP2AP_WAKEUP %d\n", GPM(17)[1] & 1, cp2ap_wakeup());
		return 0;
	}
	uint32_t n;
	uint8_t *img = boot_image(argc > 1 ? argv[1] : "/dev/disk/by-partlabel/radio2", &n);
	printf("BOOT %u bytes\n", n);

	/* pins gpc1-0..3: SPI, no pull */
	gpc1[0] = (gpc1[0] & ~0xffffu) | 0x2222;
	gpc1[2] = gpc1[2] & ~0xffffu;
	/* clock: MUX_CLKCMU_PERIC1_SPI_CAM0_USER -> OSCCLK, DIV /4 */
	cmu[0x1a0 / 4] &= ~(1u << 4);
	cmu1[0x810 / 4] = (cmu1[0x810 / 4] & ~0xfu) | 3;
	/* controller: out of USI reset, SW reset, byte mode, no DMA/irq */
	spi[0xc4 / 4] = 0;			/* USI_CON */
	spi[0x10 / 4] = 0;			/* INT_EN */
	spi[0x28 / 4] = 0;			/* SWAP_CFG */
	spi[0x0c / 4] = 1;			/* CS inactive */
	spi[0] = 1 << 5;			/* CH_CFG: SW_RST */
	msleep(1);
	spi[0] = 0;
	spi[0x24 / 4] = 0x1f; spi[0x24 / 4] = 0;	/* PENDING_CLR */
	spi[0x08 / 4] = 0;			/* MODE_CFG: byte */
	spi[0x2c / 4] = 1;			/* FB_CLK: feedback delay 1 */

	power(0);
	msleep(500);
	power(1);
	msleep(30);				/* "Ignore CP2AP_WAKEUP Low" */

	spi[0x0c / 4] = (1 << 1) | (2 << 4);	/* auto CS, NSC_CNT_2 */
	spi[0] = 1;				/* TX on */
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (uint32_t i = 0; i < n; i += 32) {
		uint32_t k, end = i + 32 < n ? i + 32 : n;
		for (k = i; k < end; k++)
			spi[0x18 / 4] = img[k];
		for (k = 0; !(spi[0x14 / 4] >> 25 & 1); k++)
			if (k > 10000000) { fprintf(stderr, "TX stuck at %u, status %08x\n", i, spi[0x14 / 4]); return 1; }
		if (spi[0x14 / 4] & 0x3c) { fprintf(stderr, "FIFO error at %u, status %08x\n", i, spi[0x14 / 4]); return 1; }
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	spi[0] = 0;
	spi[0x0c / 4] = 1;
	double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("sent in %.3f s (%.2f Mbit/s)\n", dt, n * 8 / dt / 1e6);

	for (int k = 0; k < 200; k++) {		/* check_cp_status: 200 x 20 ms */
		if (cp2ap_wakeup()) { printf("CP2AP_WAKEUP high after %d ms\n", k * 20); return 0; }
		msleep(20);
	}
	printf("CP2AP_WAKEUP stayed low; PS_HOLD %u PHONE_ACTIVE %u\n", GPM(31)[1] & 1, GPM(28)[1] & 1);
	return 2;
}
