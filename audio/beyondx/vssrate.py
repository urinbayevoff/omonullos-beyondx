#!/usr/bin/env python3
"""vssrate.py [SECONDS] -- how many uplink frames does the VSS take a second? (run as root)

Read only, like vssdump.py: waits for the next call (driver param call=1),
then once a second for SECONDS (default 60) reads one word of the CP's VSS
memory: the uplink's enqueue count (DSP 0xa0431920, UlVoiceDmaProc ->
EnquePreDlyBuffer, log.md 11.11 16:46). A 20 ms AMR frame is 2 enqueues
of 10 ms, so a clean uplink counts 100 a second; 11.11 16:46 saw ~68 in
0.5 s. Prints count, per-second rate and call_diag's tx. Writes nothing to
the device. Added for OmonullOS, November 2026.
"""
import mmap
import os
import struct
import sys
import time

P = '/sys/module/snd_zrn_abox_bx/parameters/'
VSS_PA = 0xF7800000
ENQ = 0x431920			# DSP 0xa0431920 - 0xa0000000
PAGE = 0x1000
CODE_MAGIC = bytes.fromhex("00482de9010050e3")


def rd(name):
	with open(P + name) as f:
		return f.read().strip()


def word(fd, off):
	base = off & ~(PAGE - 1)
	m = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ, offset=VSS_PA + base)
	try:
		return struct.unpack_from('<I', m, off - base)[0]
	finally:
		m.close()


def main():
	secs = int(sys.argv[1]) if len(sys.argv) > 1 else 60
	print('waiting for a call (call=1), up to 30 min...', flush=True)
	end = time.time() + 1800
	while rd('call') != '1':
		if time.time() > end:
			sys.exit('no call')
		time.sleep(0.2)
	time.sleep(3)
	fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
	m = mmap.mmap(fd, PAGE, mmap.MAP_SHARED, mmap.PROT_READ, offset=VSS_PA)
	magic = bytes(m[:8]); m.close()
	if magic != CODE_MAGIC:
		sys.exit('VSS code not where expected: %s' % magic.hex())
	rates = []
	t0, c0 = time.monotonic(), word(fd, ENQ)
	for _ in range(secs):
		time.sleep(1)
		if rd('call') != '1':
			print('the call ended')
			break
		t, c = time.monotonic(), word(fd, ENQ)
		rate = ((c - c0) & 0xffffffff) / (t - t0)
		rates.append(rate)
		tx = [x for x in rd('call_diag').split() if x.startswith('tx=')]
		print('%8.1f s  count %8d  %5.1f/s  %s' % (t, c, rate, tx[0] if tx else ''), flush=True)
		t0, c0 = t, c
	os.close(fd)
	if rates:
		rates.sort()
		print('rate: min %.1f  median %.1f  max %.1f  (%d s)' % (rates[0], rates[len(rates) // 2],
			rates[-1], len(rates)))


if __name__ == '__main__':
	main()
