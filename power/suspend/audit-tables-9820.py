#!/usr/bin/env python3
"""Read-only audit of the generated Exynos 9820 sleep tables (no phone access).

Prints, per table, the operations used and for save_sleep which register
blocks are touched with and without a power-domain condition. An
unconditional access to a block whose domain is off hangs the bus, so every
base in the "unconditional" list must be powered whenever the save runs.
"""
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
TABLES = HERE / 'sleep-tables-9820.h'
LOCAL = Path.home() / ('omonullos-src/ext/vk9820/drivers/soc/samsung/cal-if/'
                       'exynos9820/flexpmu_cal_local_exynos9820.h')
PMU = 0x15860000
ROW = re.compile(r'ZRN_SEQ\((\w+), "([^"]*)", ([^,]+), ([^,]+), (.+?), (.+?), '
                 r'([^,]+), ([^,]+), (.+?), (.+?)\),?$')


def num(text):
    return eval(text, {'__builtins__': {}}) & 0xffffffff  # table literals only


def tables():
    out, name = {}, None
    for line in TABLES.read_text().splitlines():
        m = re.match(r'static struct zrn_seq (\w+)\[\] = \{', line)
        if m:
            name = m[1]
            out[name] = []
        elif name and line.strip().startswith('ZRN_SEQ('):
            r = ROW.match(line.strip())
            if not r:
                sys.exit(f'unparsed row: {line}')
            out[name].append((r[1], r[2]) + tuple(num(r[i]) for i in range(3, 11)))
    return out


def domains():
    names = {}
    for m in re.finditer(r'"(\w+)_STATUS", 0x15860000, (0x[0-9a-fA-F]+)',
                         LOCAL.read_text()):
        names[int(m[2], 16)] = m[1]
    return names


def main():
    t, dom = tables(), domains()
    bad = 0
    for name, rows in t.items():
        ops = Counter(r[0].replace('PMUCAL_', '') for r in rows)
        print(f'{name:24} {len(rows):5}  ' + ' '.join(f'{k}={v}' for k, v in sorted(ops.items())))
        for op, reg, base, off, mask, value, *_ in rows:
            if op in ('PMUCAL_WRITE', 'PMUCAL_COND_WRITE', 'PMUCAL_WAIT',
                      'PMUCAL_WAIT_TWO') and value & ~mask:
                print(f'  value outside mask: {name} {reg} {value:#x}/{mask:#x}')
                bad += 1
            if base & 0xffff:
                print(f'  base not 64K aligned: {name} {reg} {base:#x}')
                bad += 1
            if off >= 0x10000:
                print(f'  offset beyond 64K: {name} {reg} {off:#x}')
                bad += 1
    if t['save_sleep'] != t['save_sleep_usbl2']:
        print('NOTE: save_sleep and save_sleep_usbl2 differ')
    cond, uncond = defaultdict(set), Counter()
    for op, reg, base, off, mask, value, cb, co, cm, cv in t['save_sleep']:
        if op.startswith('PMUCAL_COND_'):
            key = dom.get(co, f'{cb:#x}+{co:#x}') if cb == PMU else f'{cb:#x}+{co:#x}'
            cond[key].add(base)
        elif op in ('PMUCAL_READ', 'PMUCAL_SAVE_RESTORE'):
            uncond[base] += 1
    print('\nsave_sleep: blocks guarded by a domain status (read only when on):')
    for key in sorted(cond):
        print(f'  {key:12} ' + ' '.join(f'{b:#x}' for b in sorted(cond[key])))
    print('\nsave_sleep: blocks read UNCONDITIONALLY (must be powered at save):')
    for base, n in sorted(uncond.items()):
        guarded = [k for k, v in cond.items() if base in v]
        print(f'  {base:#010x} {n:4} rows' + (f'  (also guarded by {",".join(guarded)})' if guarded else ''))
    every = {b for rows in t.values() for r in rows for b in (r[2], r[6]) if b}
    print(f'\ndistinct 64K regions to map: {len(every)}')
    print('FAIL' if bad else 'OK: values inside masks, bases aligned, offsets < 64K')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
