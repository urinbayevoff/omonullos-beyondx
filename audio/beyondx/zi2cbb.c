/*
 * zi2cbb: bit-banged I2C on gph0-6 (SCL) / gph0-7 (SDA) of PERIC1, the
 * speaker amplifiers' bus (stock hsi2c_38 = hsi2c@109A0000), to see the
 * CS35L40s answer before there is a DT node and bus driver for them.
 *
 *   zi2cbb scan                 addresses that ACK
 *   zi2cbb rd ADDR REG [N]      N 32-bit registers (CS35L4x: 32-bit address
 *                               and data, big-endian)
 *   zi2cbb reset 0|1            gph1-1/gph1-2 (amp nRESET) low or high
 *
 * Open drain by hand: DAT stays 0, a line is driven low by making it an
 * output and released by making it an input (the bus has pull-ups: both
 * read high with the pins' own pull-downs on). The pins are left as inputs
 * with no pull; their I2C function (3) is not restored.
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

#define GPIO_PERIC1	0x10830000UL
#define GPH0		0xe0
#define GPH1		0x100
#define CON		0x0
#define DAT		0x4
#define PUD		0x8
#define SCL		6
#define SDA		7

static volatile uint32_t *g;

static uint32_t rd(unsigned off) { return g[off / 4]; }
static void wr(unsigned off, uint32_t v) { g[off / 4] = v; }

static void delay(void)
{
	struct timespec t = { 0, 5000 };
	nanosleep(&t, NULL);
}

/* 1 = release (input), 0 = drive low (output, DAT 0) */
static void line(int pin, int high)
{
	uint32_t c = rd(GPH0 + CON) & ~(0xfu << (pin * 4));
	if (!high)
		c |= 1u << (pin * 4);
	wr(GPH0 + CON, c);
	delay();
}

static int get(int pin) { return (rd(GPH0 + DAT) >> pin) & 1; }

static void start(void) { line(SDA, 1); line(SCL, 1); line(SDA, 0); line(SCL, 0); }
static void stop(void) { line(SDA, 0); line(SCL, 1); line(SDA, 1); }

static int clock_high(void)
{
	line(SCL, 1);
	for (int i = 0; i < 1000 && !get(SCL); i++)	/* clock stretching */
		delay();
	return get(SCL);
}

static int put_byte(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		line(SDA, (b >> i) & 1);
		clock_high();
		line(SCL, 0);
	}
	line(SDA, 1);
	clock_high();
	int ack = !get(SDA);
	line(SCL, 0);
	return ack;
}

static uint8_t get_byte(int ack)
{
	uint8_t b = 0;
	line(SDA, 1);
	for (int i = 0; i < 8; i++) {
		clock_high();
		b = (b << 1) | get(SDA);
		line(SCL, 0);
	}
	line(SDA, !ack);
	clock_high();
	line(SCL, 0);
	line(SDA, 1);
	return b;
}

static void setup(void)
{
	uint32_t m = (0xfu << (SCL * 4)) | (0xfu << (SDA * 4));
	wr(GPH0 + PUD, rd(GPH0 + PUD) & ~m);
	wr(GPH0 + DAT, rd(GPH0 + DAT) & ~((1u << SCL) | (1u << SDA)));
	wr(GPH0 + CON, rd(GPH0 + CON) & ~m);
	delay();
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: zi2cbb scan | rd ADDR REG [N] | reset 0|1\n");
		return 2;
	}
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	g = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_PERIC1);
	if (g == MAP_FAILED) { perror("mmap"); return 1; }

	if (!strcmp(argv[1], "reset") && argc > 2) {
		uint32_t m = (0xfu << 4) | (0xfu << 8), bits = (1u << 1) | (1u << 2);
		uint32_t d = rd(GPH1 + DAT) & ~bits;
		if (atoi(argv[2]))
			d |= bits;
		wr(GPH1 + DAT, d);
		wr(GPH1 + CON, (rd(GPH1 + CON) & ~m) | (1u << 4) | (1u << 8));
		printf("gph1 con %08x dat %08x\n", rd(GPH1 + CON), rd(GPH1 + DAT));
		return 0;
	}

	setup();
	printf("idle: scl %d sda %d\n", get(SCL), get(SDA));
	if (!get(SCL) || !get(SDA)) {
		fprintf(stderr, "bus not idle\n");
		return 1;
	}

	if (!strcmp(argv[1], "scan")) {
		for (int a = 0x08; a < 0x78; a++) {
			start();
			if (put_byte(a << 1))
				printf("0x%02x ack\n", a);
			stop();
		}
		return 0;
	}

	if (!strcmp(argv[1], "rd") && argc > 3) {
		int a = strtol(argv[2], NULL, 16), n = argc > 4 ? atoi(argv[4]) : 1;
		uint32_t reg = strtoul(argv[3], NULL, 16);
		for (int k = 0; k < n; k++, reg += 4) {
			int ok = 1;
			start();
			ok &= put_byte(a << 1);
			for (int i = 3; i >= 0; i--)
				ok &= put_byte(reg >> (i * 8));
			start();
			ok &= put_byte((a << 1) | 1);
			uint32_t v = 0;
			for (int i = 0; i < 4; i++)
				v = (v << 8) | get_byte(i < 3);
			stop();
			printf("%02x:%08x = %08x%s\n", a, reg, v, ok ? "" : " (nak)");
		}
		return 0;
	}
	fprintf(stderr, "bad command\n");
	return 2;
}
