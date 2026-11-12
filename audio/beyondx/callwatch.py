#!/usr/bin/env python3
import os, re, subprocess, sys, time
# callwatch.py OUTFILE SECONDS (root, read only): from now until the next call ends, the whole DSP log
# (dsplog pages, per-frame noise dropped), ROUTE/SPUM/WDMA1/WDMA2/WDMA7 registers and the tx level.
# Lines may be out of order after a #BACKFILL: sort by the timestamp.
# Added for OmonullOS, November 2026.
P = '/sys/module/snd_zrn_abox_bx/parameters/'
out = open(sys.argv[1], 'w')
def rd(n): return open(P + n).read()
end = time.time() + float(sys.argv[2])
seen = set()
NOISE = r'wd3IntCnt|in-full|source_update|sink_update|buffer_empty|mute count|PcmCountISR'
def take():
    for l in rd('dsplog').splitlines():
        m = re.match(r'\[\s*[0-9.]+\]', l)
        if m and l not in seen and not re.search(NOISE, l):
            seen.add(l); out.write(l + '\n')
# the DSP log before the call too: the VSS's start and the call streams' OPEN/HW_PARAMS/TRIGGER
# reach the PCMIN task in the first 100 ms, before `call` reads 1
while rd('call').strip() != '1':
    if time.time() > end: out.write('timeout\n'); sys.exit()
    take(); out.flush()
    time.sleep(0.05)
out.write('#CALL=1\n')
def backfill(nbytes):
    # the pages behind the newest (dsplog_skip: our module's read offset), so the
    # lines a busy VSS pushed out of the newest page between two reads are kept
    try:
        for skip in range(3000, nbytes, 3000):
            open(P + 'dsplog_skip', 'w').write(str(skip))
            take()
    finally:
        open(P + 'dsplog_skip', 'w').write('0')
    out.write('#BACKFILL %d\n' % nbytes)
time.sleep(0.3); backfill(240000)
t0 = time.time(); last_regs = 0
REGS = '18c50044 18c50048 18c50300 18c53100 18c53104 18c53108 18c5310c 18c53110 18c53114 18c53130 18c53200 18c53208 18c53230 18c53400 18c53408 18c53430 18c53700 18c53708 18c53730'.split()
while True:
    on = rd('call').strip() == '1'
    take()
    if time.time() - last_regs > 0.5:
        last_regs = time.time()
        r = subprocess.run(['zpoke'] + REGS[:1], capture_output=True, text=True)
        regs = ' '.join(subprocess.run(['zpoke', a], capture_output=True, text=True).stdout.split('=')[-1].strip() for a in REGS)
        tx = re.search(r' tx=(-?\d+)', rd('call_diag'))
        out.write('#REGS +%.1f %s tx=%s\n' % (time.time() - t0, regs, tx.group(1) if tx else '?'))
    out.flush()
    if not on: break
    time.sleep(0.08)
time.sleep(0.3); backfill(240000)
out.write('#END +%.1f\n' % (time.time() - t0))
