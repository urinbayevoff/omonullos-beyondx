# raw words of the realtime rings (WDMA3, RDMA7): how the 24-bit samples sit in
# their 32-bit containers (top byte 00/ff/other), reads only, via /dev/mem
import mmap, os, struct, collections
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
def rd(pa, n):
    base = pa & ~0xfff
    m = mmap.mmap(fd, (pa - base + n + 0xfff) & ~0xfff, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
    d = m[pa - base:pa - base + n]; m.close(); return d
def u32(pa): return struct.unpack('<I', rd(pa, 4))[0]
lv1 = u32(0x18e0000c) << 12
def v2p(iova):
    e = u32(lv1 + (iova >> 20) * 4)
    if e & 3 == 2: return ((e & ~0xfffff) << 4) | (iova & 0xfffff)
    p = u32(((e & ~0xf) << 4) + ((iova >> 12) & 0xff) * 4)
    return ((p & ~0xff) << 4) | (iova & 0xfff)
def dump(name, iova, nbytes, ch):
    d = b''.join(rd(v2p(iova + o), min(4096, nbytes - o)) for o in range(0, nbytes, 4096))
    w = struct.unpack('<%dI' % (len(d) // 4), d)
    top = collections.Counter('%02x' % (x >> 24) if x >> 24 in (0, 0xff) else 'other' for x in w[0::ch] if x)
    big = sorted(set(w[0::ch]), key=lambda x: -abs(struct.unpack('<i', struct.pack('<I', x))[0]))[:4]
    print(name, 'ch0 top bytes', dict(top), 'nonzero', sum(1 for x in w[0::ch] if x), 'of', len(w) // ch,
          'sample', ' '.join('%08x' % x for x in w[0:ch * 6:ch]), 'extremes', ' '.join('%08x' % x for x in big))
dump('WDMA3', 0x92300000, 480 * 2 * 4 * 4, 2)
dump('RDMA7', 0x91700000, 480 * 4 * 4 * 4, 4)
