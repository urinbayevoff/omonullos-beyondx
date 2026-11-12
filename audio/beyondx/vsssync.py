#!/usr/bin/env python3
"""vsssync.py [SECONDS] [-v] -- why the VSS restarts WDMA2/WDMA3 every 20 ms.

Read only: maps the CP's VSS (AP 0xf7800000 = DSP 0xa0000000, see
CP_VSS_PA in snd-zrn-abox-bx.c) through /dev/mem and samples the VSS's own
sync variables as fast as Python can. Needs root (sudo). Writes nothing.

What the VSS does (modem.bin toc VSS, disassembled 10.11, VHM_Irq.c):
  ISR_VIFBoundary (every 20 ms speech frame): if armed (0xa0431afc == 1) and
    state (0xa03cb464) != 1 -> "trigger WDMA reset routine": calls calliope's
    0x800c9f0c (the "EMERGENCY VSS MAGIN CALLBACK" in the DSP log: stops and
    restarts NEIN WDMA2 and FEIN1 WDMA3), bumps 0xa0431b00, then state = 1.
    Then stamps the VIF time in 0xa0431af4 (us, 26 MHz / 26).
  ISR_WDMAInt (WDMA2 period, the uplink): stamps 0xa0431af8, d = af8 - af4:
    d in 1151..1799 or 11151..11799 us -> state 0 ("clock unsync");
    d in 1800..2499 or 11800..12499 us -> state 2; else state stays.
  So every frame whose WDMA2 interrupt lands 1.15-2.5 ms (mod 10 ms) after
  the VIF boundary causes a restart at the next one.

This prints the d values the VSS saw, how often state left 1, the reset
count rate, and the VIF and WDMA2 interrupt intervals.
"""
import collections
import mmap
import os
import struct
import sys
import time

VSS_PA, VSS_VA = 0xF7800000, 0xA0000000
VARS = {  # name: (DSP address, size)
    "state": (0xA03CB464, 4),
    "vif_ts": (0xA0431AF4, 4),
    "wd_ts": (0xA0431AF8, 4),
    "armed": (0xA0431AFC, 1),
    "resets": (0xA0431B00, 4),
    "wd2cnt": (0xA04318F8, 4),
}
CODE_MAGIC = bytes.fromhex("00482de9010050e3")  # first words of the VSS image

secs = float(next((a for a in sys.argv[1:] if not a.startswith("-")), "5"))
verbose = "-v" in sys.argv

fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)


def region(va, size):
    pa = VSS_PA + (va - VSS_VA)
    base = pa & ~(mmap.PAGESIZE - 1)
    m = mmap.mmap(fd, mmap.PAGESIZE * 2, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
    return m, pa - base


code, off = region(VSS_VA, 8)
if code[off:off + 8] != CODE_MAGIC:
    sys.exit(f"VSS code not where expected: {code[off:off + 8].hex()} (is a call/VSS up?)")

maps = {}
for name, (va, size) in VARS.items():
    m, o = region(va, size)
    maps[name] = (m, o, "<I" if size == 4 else "<B")


def snap():
    return {n: struct.unpack_from(f, m, o)[0] for n, (m, o, f) in maps.items()}


samples = []
t0 = time.monotonic()
while (t := time.monotonic() - t0) < secs:
    s = snap()
    s["t"] = t
    samples.append(s)

first, last = samples[0], samples[-1]
print(f"{len(samples)} samples in {secs:.1f} s ({secs * 1e3 / len(samples):.2f} ms apart)")
print(f"armed {first['armed']}->{last['armed']}  state {first['state']}->{last['state']}  "
      f"resets +{(last['resets'] - first['resets']) & 0xffffffff}  "
      f"wd2 ints +{(last['wd2cnt'] - first['wd2cnt']) & 0xffffffff}")

states = collections.Counter(s["state"] for s in samples)
print("state seen (samples):", dict(states))

deltas, vif_gaps, wd_gaps, events = [], [], [], []
prev = first
for s in samples[1:]:
    if s["vif_ts"] != prev["vif_ts"]:
        if prev["vif_ts"]:
            vif_gaps.append((s["vif_ts"] - prev["vif_ts"]) & 0xffffffff)
        events.append((s["t"], "VIF", s["vif_ts"], s["state"], s["resets"]))
    if s["wd_ts"] != prev["wd_ts"]:
        if prev["wd_ts"]:
            wd_gaps.append((s["wd_ts"] - prev["wd_ts"]) & 0xffffffff)
        d = (s["wd_ts"] - s["vif_ts"]) & 0xffffffff
        deltas.append(d)
        events.append((s["t"], "WDMA2", s["wd_ts"], s["state"], d))
    prev = s


def hist(name, vals, step):
    if not vals:
        print(f"{name}: none seen")
        return
    c = collections.Counter(v // step * step for v in vals)
    print(f"{name} ({len(vals)}): " + "  ".join(f"{k}:{n}" for k, n in sorted(c.items())))


hist("VIF interval us", vif_gaps, 500)
hist("WDMA2 interval us", wd_gaps, 500)
hist("WDMA2 after VIF, d us (bad: 1151-2499, 11151-12499)", deltas, 250)
bad = sum(1 for d in deltas if 1151 <= d <= 2499 or 11151 <= d <= 12499)
print(f"d in a bad window: {bad}/{len(deltas)}")
if verbose:
    for t, kind, ts, st, x in events[:120]:
        print(f"{t * 1e3:9.2f} ms {kind:5} ts={ts} state={st} {'resets' if kind == 'VIF' else 'd'}={x}")
