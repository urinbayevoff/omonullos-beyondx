#!/usr/bin/env python3
"""vssdump.py [OUTDIR] -- where does the uplink go inside the VSS? (run as root)

Read only, like vsssync.py: waits for the next call (driver param call=1),
then 6 s into it copies the CP's VSS memory (AP 0xf7800000, 8 MiB = DSP
0xa0000000..) and the ABOX SRAM call buffers (0x18d29000, 4 KiB) to OUTDIR
twice, 0.5 s apart, plus call_diag. Arm `mic-test.sh tone` first: the codec's
steady 1 kHz tone is then easy to find in the dumps (vssfind.py). Writes
nothing to the device. Added for OmonullOS, November 2026.
"""
import mmap
import os
import sys
import time

P = '/sys/module/snd_zrn_abox_bx/parameters/'
VSS_PA, VSS_SIZE = 0xF7800000, 0x800000
SRAM_PA, SRAM_SIZE = 0x18D29000, 0x1000
CODE_MAGIC = bytes.fromhex("00482de9010050e3")


def rd(name):
	with open(P + name) as f:
		return f.read().strip()


def grab(fd, pa, size):
	m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ, offset=pa)
	try:
		return bytes(m[:size])
	finally:
		m.close()


def main():
	out = sys.argv[1] if len(sys.argv) > 1 else '/home/alarm/vssdump'
	os.makedirs(out, exist_ok=True)
	print('waiting for a call (call=1), up to 30 min...', flush=True)
	end = time.time() + 1800
	while rd('call') != '1':
		if time.time() > end:
			sys.exit('no call')
		time.sleep(0.2)
	time.sleep(6)
	if rd('call') != '1':
		sys.exit('the call ended before 6 s')
	fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
	for n in (1, 2):
		vss = grab(fd, VSS_PA, VSS_SIZE)
		if vss[:8] != CODE_MAGIC:
			sys.exit('VSS code not where expected: %s' % vss[:8].hex())
		sram = grab(fd, SRAM_PA, SRAM_SIZE)
		for name, data in (('vss%d.bin' % n, vss), ('sram%d.bin' % n, sram)):
			with open(os.path.join(out, name), 'wb') as f:
				f.write(data)
		with open(os.path.join(out, 'diag%d.txt' % n), 'w') as f:
			f.write(rd('call_diag') + '\n')
		print('dump %d done' % n, flush=True)
		time.sleep(0.5)
	os.close(fd)
	uid = os.stat('/home/alarm').st_uid
	for name in os.listdir(out):
		os.chown(os.path.join(out, name), uid, uid)
	os.chown(out, uid, uid)
	print('saved in', out)


if __name__ == '__main__':
	main()
