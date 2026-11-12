#!/usr/bin/env python3
"""rtpos.py [SECONDS] -- where WDMA3 and RDMA7 really are, ~3 ms apart.

Reads snd-zrn-abox-bx's call_diag (no root, read only) as fast as it can and
decodes the DMA status words: bit 31 running, bits 23:16 the base of the
period the DMA is in (>> 8), the low bits its byte offset in it (/ 16).
WDMA3's period is 0xf00 bytes (2 ch x 32 bit x 480), RDMA7's 0x1e00 (4 ch),
four periods each, so a free-running ring shows period bases 00 0f 1e 2d
(WDMA3) and 00 1e 3c 5a (RDMA7).

Seen 10.11 at 0.5 s steps: through a ringtone WDMA3 goes through all four;
in a call only 00 and 0f -- as if the VSS restarts it every 20 ms, which
would starve CHCONV4CH every other RDMA7 period (the "RDMA7 underrun" /
choppy downlink). Run it during a live call (zcell sets the path up) and,
to compare, while something plays without a call.
"""
import collections
import re
import sys
import time

P = "/sys/module/snd_zrn_abox_bx/parameters/call_diag"
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 3.0
rx = re.compile(r"\b(call|wdma3|rdma7)=([0-9a-f-]+)")

samples = []
t0 = time.monotonic()
while (t := time.monotonic() - t0) < secs:
    with open(P) as f:
        d = dict(rx.findall(f.read()))
    samples.append((t, d.get("call", "?"), int(d.get("wdma3", "0"), 16), int(d.get("rdma7", "0"), 16)))


def report(name, idx):
    per = collections.Counter()
    wraps, last_base, run = [], None, 0
    for s in samples:
        st = s[idx]
        if not st >> 31:
            continue
        run += 1
        base = (st >> 16) & 0xff
        per[base] += 1
        if last_base is not None and base < last_base:
            wraps.append(s[0])
        last_base = base
    gaps = [round((b - a) * 1000) for a, b in zip(wraps, wraps[1:])]
    print(f"{name}: running in {run}/{len(samples)} samples; period bases seen "
          + " ".join(f"{b:02x}:{n}" for b, n in sorted(per.items())))
    if gaps:
        gaps.sort()
        print(f"  ring wraps {len(wraps)}, ms between them: min {gaps[0]} median {gaps[len(gaps)//2]} max {gaps[-1]}")


calls = collections.Counter(s[1] for s in samples)
print(f"{len(samples)} samples in {secs:.1f} s ({secs * 1000 / max(len(samples), 1):.1f} ms apart), call= {dict(calls)}")
report("WDMA3", 2)
report("RDMA7", 3)
if "-v" in sys.argv:
    for t, c, w, r in samples[:200]:
        print(f"{t * 1000:8.1f} call={c} wdma3={w:08x} rdma7={r:08x}")
