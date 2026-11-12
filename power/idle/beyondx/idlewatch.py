#!/usr/bin/env python3
# Watch one core's C2 from cpu0 (run under `taskset -c 0`): FlexPMU debug
# lines (APM SRAM 0x0204bc00, Samsung exynos-flexpmu-dbg.c), PMU/BID regs
# and the cpuidle counters, printed to stdout (stream it over ssh: disk and
# kmsg may not survive a stuck core). Read only.
import mmap, os, struct, sys, time

core = int(sys.argv[1]) if len(sys.argv) > 1 else 3
n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)

def mapw(base, size=0x1000):
	return memoryview(mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)).cast("I")

sram = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x0204b000)
pmu0, pmu1, bid = mapw(0x15860000), mapw(0x15861000), mapw(0x15870000)
int_en = [0x1044, 0x10c4, 0x1144, 0x11c4, 0x1344, 0x13c4, 0x1544, 0x15c4][max(core, 0)]
status = int_en - 0x40

def line(i):
	return sram[0xc00 + 16 * i + 8:0xc00 + 16 * i + 16].hex()

def cpuidle(f):
	try:
		return open("/sys/devices/system/cpu/cpu%d/cpuidle/state1/%s" % (core, f)).read().strip()
	except OSError:
		return "-"

INT_EN = [0x1044, 0x10c4, 0x1144, 0x11c4, 0x1344, 0x13c4, 0x1544, 0x15c4]

def use(c):
	try:
		return open("/sys/devices/system/cpu/cpu%d/cpuidle/state1/usage" % c).read().strip()
	except OSError:
		return "-"

t0 = time.monotonic()
if core < 0:	# every core on one line: STATUS, INFORM, INT_EN bit 3, usage
	for i in range(n):
		st = "".join(str(pmu1[(INT_EN[c] - 0x40 - 0x1000) // 4] & 1) for c in range(8))
		inf = "".join(str(pmu0[(0x860 + 4 * c) // 4]) for c in range(8))
		ie = "".join(str(pmu1[(INT_EN[c] - 0x1000) // 4] >> 3 & 1) for c in range(8))
		print("%5.2f st=%s inf=%s ie=%s bidEN=%02x p2=%02x p1=%04x pend1=%x | fpmu=%s seq=%s cur=%s use=%s"
		      % (time.monotonic() - t0, st, inf, ie, bid[0x200 // 4], bid[0x208 // 4], bid[0x108 // 4],
			 bid[0x100 // 4], line(0), line(1), line(3), ",".join(use(c) for c in range(1, 8))), flush=True)
		time.sleep(0.2)
	sys.exit()
for i in range(n):
	print("%5.2f use=%s st=%s inform=%x int_en=%x bidEN=%x p2=%x p1=%x | cpu=%s seq=%s cur=%s/%s irq=%s/%s inf23=%s pmd=%s"
	      % (time.monotonic() - t0, cpuidle("usage"), pmu1[(status - 0x1000) // 4] & 0xf,
		 pmu0[(0x860 + 4 * core) // 4], pmu1[(int_en - 0x1000) // 4], bid[0x200 // 4],
		 bid[0x208 // 4], bid[0x108 // 4], line(0), line(1), line(2), line(3),
		 line(11), line(12), line(28), line(26)), flush=True)
	time.sleep(0.25)
