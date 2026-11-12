#!/usr/bin/env python3
# dsplogdump.py OUTFILE [MAXBYTES] (root): the whole DSP text log ring, oldest first,
# read page by page through dsplog_skip (put back to 0 at the end). Reads only
# the log (DRAM); dsplog_skip is our module's own read offset, no hardware.
# Added for OmonullOS, November 2026.
import re, sys
P = '/sys/module/snd_zrn_abox_bx/parameters/'
STEP = 3000                      # pages are ~4 KB: overlap them
mx = int(sys.argv[2]) if len(sys.argv) > 2 else 4 << 20
def page(skip):
    open(P + 'dsplog_skip', 'w').write(str(skip))
    return open(P + 'dsplog').read()
pages = []
try:
    first = page(0)
    pages.append(first)
    skip = STEP
    while skip < mx:
        p = page(skip)
        if skip > STEP and p == first:   # wrapped round the ring
            break
        pages.append(p)
        skip += STEP
finally:
    open(P + 'dsplog_skip', 'w').write('0')
seen = set(); lines = []
for p in reversed(pages):            # oldest page first
    for l in p.splitlines():
        m = re.match(r'\[\s*([0-9]+\.[0-9]+)\]', l)
        if m and l not in seen:
            seen.add(l); lines.append((float(m.group(1)), l))
lines.sort(key=lambda t: t[0])
with open(sys.argv[1], 'w') as f:
    for _, l in lines:
        f.write(l + '\n')
print('%d pages, %d lines, %.3f .. %.3f' % (len(pages), len(lines), lines[0][0] if lines else 0, lines[-1][0] if lines else 0))
