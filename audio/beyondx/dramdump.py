#!/usr/bin/env python3
"""dramdump.py [OUTDIR] -- the ABOX DRAM and SRAM in a call (run as root).

Read only, like vssdump.py. The VSS's speech enhancement (SeTx) and encoder
(VocTx) work buffers are not in the CP's VSS memory nor in the 4 KiB of SRAM
vssdump.py saves, so this copies the calliope's own memory instead:
  dramN.bin  DSP 0x80000000.. (28 MiB, our dma_alloc_coherent DRAM)
  asramN.bin ABOX SRAM 0x18d00000.. (0x59000, the DSP's SRAM)
  sramN.bin  0x18d29000.. (4 KiB, the WDMA2 call buffer, as vssdump.py)
The DRAM's physical address is not exported by the module, so it is found
read only the way the DSP finds it: SysMMU PT_BASE (one register read) ->
level 1 / level 2 tables -> each 4 KiB page. Checked against
/lib/firmware/calliope_dram.bin. Waits for the next call (driver param
call=1), dumps 6 s in and again 0.5 s later. Writes nothing to the device.
Search the result with vssfind.py OUTDIR. --check: the translation only. Added for OmonullOS, November 2026.
"""
import mmap
import os
import struct
import sys
import time

P = '/sys/module/snd_zrn_abox_bx/parameters/'
SYSMMU_AUD, MMU_PT_BASE = 0x18E00000, 0x00C
IOVA_DRAM, DRAM_SIZE = 0x80000000, 0x1C00000
ASRAM_PA, ASRAM_SIZE = 0x18D00000, 0x59000
SRAM_PA, SRAM_SIZE = 0x18D29000, 0x1000
FW = '/lib/firmware/calliope_dram.bin'


def rd(name):
	with open(P + name) as f:
		return f.read().strip()


def grab(fd, pa, size):
	base = pa & ~0xFFF
	m = mmap.mmap(fd, size + (pa - base), mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
	try:
		return bytes(m[pa - base:pa - base + size])
	finally:
		m.close()


def u32(fd, pa):
	return struct.unpack('<I', grab(fd, pa, 4))[0]


def page_map(fd):
	"""[(iova, pa)] for every 4 KiB page of the DRAM window, from the SysMMU tables."""
	pt = u32(fd, SYSMMU_AUD + MMU_PT_BASE)
	l1_pa = pt << 12
	if not pt:
		sys.exit('SysMMU PT_BASE is 0')
	l1 = grab(fd, l1_pa, 0x4000)
	pages = []
	l2_cache = {}
	for iova in range(IOVA_DRAM, IOVA_DRAM + DRAM_SIZE, 0x1000):
		e = struct.unpack_from('<I', l1, (iova >> 20) * 4)[0]
		if e & 3 == 2:		# 1 MiB section: pa >> 4 in bits 31:16 (36-bit pa)
			pa = ((e & 0xFFFF0000) << 4) + (iova & 0xFFFFF)
		elif e & 3 == 1:	# link to a level 2 table (1 KiB)
			t = (e & 0xFFFFFFC0) << 4	# 1 KiB table: pa >> 4 in bits 31:6
			if t not in l2_cache:
				l2_cache[t] = grab(fd, t, 0x400)
			e2 = struct.unpack_from('<I', l2_cache[t], ((iova >> 12) & 0xFF) * 4)[0]
			if e2 & 3 != 2:
				sys.exit('iova %08x: no level 2 page (%08x)' % (iova, e2))
			pa = (e2 & 0xFFFFFF00) << 4	# 4 KiB page: pa >> 4 in bits 31:8
		else:
			sys.exit('iova %08x: no level 1 entry (%08x)' % (iova, e))
		pages.append((iova, pa))
	return l1_pa, pages


def runs(pages):
	"""merge physically contiguous pages into (pa, size) runs"""
	out = []
	for _, pa in pages:
		if out and out[-1][0] + out[-1][1] == pa:
			out[-1][1] += 0x1000
		else:
			out.append([pa, 0x1000])
	return out


def main():
	check = '--check' in sys.argv	# translate and compare only, no call
	args = [a for a in sys.argv[1:] if a != '--check']
	out = args[0] if args else '/home/alarm/dramdump'
	os.makedirs(out, exist_ok=True)
	fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
	l1_pa, pages = page_map(fd)
	rr = runs(pages)
	print('l1 %#x, DRAM in %d physical run(s): %s' % (l1_pa, len(rr),
	      ' '.join('%#x+%#x' % (a, s) for a, s in rr[:4])), flush=True)
	fw = open(FW, 'rb').read(0x1000)
	head = b''.join(grab(fd, a, min(s, 0x1000)) for a, s in rr[:1])[:0x1000]
	same = sum(1 for a, b in zip(fw, head) if a == b)
	print('first 4 KiB vs %s: %d/4096 bytes equal' % (FW, same), flush=True)
	if same < 2048:
		print('fw  %s' % fw[:32].hex())
		print('mem %s' % head[:32].hex())
		sys.exit('the DRAM does not look like the calliope firmware: wrong translation?')
	if check:
		print('check OK')
		return
	print('waiting for a call (call=1), up to 30 min...', flush=True)
	end = time.time() + 1800
	while rd('call') != '1':
		if time.time() > end:
			sys.exit('no call')
		time.sleep(0.2)
	time.sleep(6)
	if rd('call') != '1':
		sys.exit('the call ended before 6 s')
	for n in (1, 2):
		t = time.time()
		sram = grab(fd, SRAM_PA, SRAM_SIZE)
		asram = grab(fd, ASRAM_PA, ASRAM_SIZE)
		dram = b''.join(grab(fd, a, s) for a, s in rr)
		took = time.time() - t
		for name, data in (('sram%d.bin' % n, sram), ('asram%d.bin' % n, asram), ('dram%d.bin' % n, dram)):
			with open(os.path.join(out, name), 'wb') as f:
				f.write(data)
		with open(os.path.join(out, 'diag%d.txt' % n), 'w') as f:
			f.write(rd('call_diag') + '\n')
		print('dump %d done (%.2f s)' % (n, took), flush=True)
		time.sleep(0.5)
	os.close(fd)
	uid = os.stat('/home/alarm').st_uid
	for name in os.listdir(out):
		os.chown(os.path.join(out, name), uid, uid)
	os.chown(out, uid, uid)
	print('saved in', out)


if __name__ == '__main__':
	main()
