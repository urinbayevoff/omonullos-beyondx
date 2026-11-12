/*
 * zpoke ADDR [VALUE]: read or write one 32-bit register through /dev/mem
 * with a single 32-bit load/store (Python's mmap splits stores into bytes).
 * Added for OmonullOS, October 2026.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: zpoke ADDR [VALUE]\n"); return 2; }
	unsigned long a = strtoul(argv[1], NULL, 16);
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	volatile uint32_t *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, a & ~0xfffUL);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }
	p += (a & 0xfff) / 4;
	if (argc > 2)
		*p = strtoul(argv[2], NULL, 16);
	printf("%08lx = %08x\n", a, *p);
	return 0;
}
