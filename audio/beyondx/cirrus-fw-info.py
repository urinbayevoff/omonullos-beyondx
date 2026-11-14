#!/usr/bin/env python3
"""cirrus-fw-info.py FILE... -- what is inside Cirrus HALO firmware (.wmfw) and
coefficient (.bin, WMDR) files: algorithms, their controls, the coefficient
blocks and the build notes. Read only. Used for the CS35L40 speaker
protection files in stock VENDOR (/mnt/vendor/firmware/cs35l40-*), see
docs/s10-5g/SPK-PROT.md. Formats as in the kernel's cs_dsp.c.

Added for OmonullOS, November 2026.
"""
import struct, sys


def pstr(d, o, sz):
    """a length-prefixed string, padded to 4 bytes (cs_dsp_coeff_parse_string)"""
    n = int.from_bytes(d[o:o + sz], 'little')
    return d[o + sz:o + sz + n].decode(errors='replace'), o + ((n + sz + 3) & ~3)


def wmfw(p, d):
    hl, = struct.unpack_from('<I', d, 4)
    print(p, 'WMFW core', d[10], 'ver', d[11], 'size', len(d))
    off = hl
    while off + 8 <= len(d):
        w, ln = struct.unpack_from('<II', d, off)
        typ, data = w >> 24, d[off + 8:off + 8 + ln]
        if typ == 0xfe:
            print('  info:', data.split(b'\0')[0].decode(errors='replace'))
        elif typ == 0xf2:
            o = 4
            aid, = struct.unpack_from('<I', data, 0)
            name, o = pstr(data, o, 1)
            _, o = pstr(data, o, 2)
            nc, = struct.unpack_from('<I', data, o)
            o += 4
            print('  alg %06x %s, %d controls' % (aid, name, nc))
            for _ in range(nc):
                o0 = o
                coff, mem, blk = struct.unpack_from('<HHI', data, o)
                o += 8
                short, o = pstr(data, o, 1)
                _, o = pstr(data, o, 1)
                _, o = pstr(data, o, 2)
                ctype, flags, clen = struct.unpack_from('<HHI', data, o)
                print('    %-28s mem %x off 0x%03x len %d flags %x' % (short, mem, coff, clen, flags))
                o = o0 + 8 + blk
        else:
            print('  region type %02x addr %06x len %d' % (typ, w & 0xffffff, ln))
        off += 8 + ln


def wmdr(p, d):
    hl, = struct.unpack_from('<I', d, 4)
    print(p, 'WMDR size', len(d))
    o = hl
    while o + 20 <= len(d):
        boff, typ, aid, aver, sr, ln = struct.unpack_from('<HHIIII', d, o)
        if typ == 0xfe00:
            print('  info:', d[o + 20:o + 20 + ln].split(b'\0')[0].decode(errors='replace'))
        else:
            print('  block off 0x%x type %x alg %x ver %x len %d' % (boff, typ, aid, aver, ln))
        o += 20 + ((ln + 3) & ~3)


for p in sys.argv[1:]:
    d = open(p, 'rb').read()
    {b'WMFW': wmfw, b'WMDR': wmdr}.get(d[:4], lambda p, d: print(p, 'unknown'))(p, d)
