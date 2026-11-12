#!/usr/bin/env python3
"""vssfind.py DUMPDIR -- find the codec's tone in vssdump.py's / dramdump.py's dumps.

Scans vss1.bin/vss2.bin (DSP 0xa0000000..), sram1/2.bin (ABOX SRAM
0x18d29000..), asram1/2.bin (ABOX SRAM 0x18d00000..) and dram1/2.bin (DSP
0x80000000..) in 1 KiB blocks for a steady sine: 16-bit, 32-bit or float
samples, mono or interleaved stereo, period 4/8/16/32/48 samples (the tone
reaches the VSS as period 4 = 2 kHz at 8 kHz; period 8 = 1 kHz at 8 kHz).
Also looks for exact copies of the WDMA2 input's sample runs (sram1.bin). Prints each hit's address, sample shape and peak, and merges runs
of neighbouring blocks into one buffer. Plain Python, reads files only.
Added for OmonullOS, November 2026.
"""
import array
import os
import sys

BLOCK = 1024
BASES = {'vss': 0xA0000000, 'sram': 0x18D29000, 'asram': 0x18D00000, 'dram': 0x80000000}


def corr(x, lag):
	n = len(x) - lag
	if n <= 16:
		return 0.0
	a = x[:n]; b = x[lag:lag + n]
	ab = sum(p * q for p, q in zip(a, b))
	aa = sum(p * p for p in a); bb = sum(q * q for q in b)
	return ab / ((aa * bb) ** 0.5) if aa and bb else 0.0


def shapes(block):
	s16 = array.array('h', block)
	s32 = array.array('i', block)
	yield '16bit', list(s16), 1500, 32767
	yield '32bit', [v >> 16 for v in s32], 1500, 32767
	f = array.array('f', block)
	if all(-4.0 < v < 4.0 for v in f[::16]):
		yield 'float', [int(v * 32768) if -4.0 < v < 4.0 else 0 for v in f], 150, 131072


def find(path, base):
	data = open(path, 'rb').read()
	hits = []
	for off in range(0, len(data) - BLOCK + 1, BLOCK):
		blk = data[off:off + BLOCK]
		if not any(blk):
			continue
		hit = None
		for name, x, lo, hi in shapes(blk):	# 16 bit first: a 16-bit sine also looks like a 32-bit one
			peak = max(abs(v) for v in x)
			if not lo <= peak <= hi:
				continue
			for ch in (1, 2):
				xs = x[::ch]
				for per, rate in ((4, 8000), (8, 8000), (16, 16000), (32, 32000), (48, 48000)):
					# a sine: high correlation one period on, negative half a period on
					if corr(xs, per) > 0.9 and corr(xs, per // 2) < -0.8:
						hit = (base + off, name, ch, per, peak)
						break
				if hit:
					break
			if hit:
				hits.append(hit)
				break
	return hits


def merge(hits):
	runs = []
	for h in hits:
		if runs and h[0] == runs[-1][1] + BLOCK and h[1:4] == runs[-1][2][1:4]:
			runs[-1][1] = h[0]
			runs[-1][3] = max(runs[-1][3], h[4])
		else:
			runs.append([h[0], h[0], h, h[4]])
	return runs


def main():
	d = sys.argv[1] if len(sys.argv) > 1 else '/home/alarm/vssdump'
	for f in sorted(os.listdir(d)):
		if not f.endswith('.bin'):
			continue
		base = BASES[f.rstrip('0123456789.bin')]
		runs = merge(find(os.path.join(d, f), base))
		print('== %s: %d buffer(s) with the tone' % (f, len(runs)))
		for start, last, h, peak in runs:
			print('  %08x..%08x  %s %s per %s peak %d' % (start, last + BLOCK, h[1], 'stereo' if h[2] == 2 else 'mono', h[3], peak))
	exact(d)


def exact(d):
	"""exact copies of 8-sample runs of the WDMA2 input (sram1.bin 0x10.., 16 bit stereo):
	interleaved as it is, or one channel (mono)"""
	src = os.path.join(d, 'sram1.bin')
	if not os.path.exists(src):
		return
	s = array.array('h', open(src, 'rb').read()[0x10:0x290])
	if not any(abs(v) > 200 for v in s):
		print('== exact: the WDMA2 input is silent, skipped')
		return
	pats = {}
	for i in range(0, 64, 8):
		pats['stereo@%d' % i] = s[i:i + 16].tobytes()
		pats['left@%d' % i] = array.array('h', s[i:i + 32:2]).tobytes()
		pats['right@%d' % i] = array.array('h', s[i + 1:i + 33:2]).tobytes()
	for f in sorted(os.listdir(d)):
		if not f.endswith('.bin'):
			continue
		data = open(os.path.join(d, f), 'rb').read()
		base = BASES[f.rstrip('0123456789.bin')]
		found = {}
		for name, p in pats.items():
			o = data.find(p)
			while o >= 0 and len(found) < 64:
				found.setdefault(base + o - o % 0x100, set()).add(name.split('@')[0])
				o = data.find(p, o + 2)
		print('== exact %s: %d place(s)' % (f, len(found)))
		for a in sorted(found)[:32]:
			print('  ~%08x  %s' % (a, ' '.join(sorted(found[a]))))


if __name__ == '__main__':
	main()
