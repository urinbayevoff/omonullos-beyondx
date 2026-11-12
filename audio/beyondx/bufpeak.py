# peak per slot of ABOX buffers, walking the SysMMU tables via /dev/mem (reads only)
import mmap, os, struct, sys
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
def rd(pa, n):
    base = pa & ~0xfff
    m = mmap.mmap(fd, (pa - base + n + 0xfff) & ~0xfff, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
    d = m[pa - base:pa - base + n]; m.close(); return d
def u32(pa): return struct.unpack('<I', rd(pa, 4))[0]
lv1 = u32(0x18e0000c) << 12
def v2p(iova):
    e = u32(lv1 + (iova >> 20) * 4)
    if e & 3 == 2: return ((e >> 20) << 24 if False else ((e & ~0xfffff) << 4)) | (iova & 0xfffff)
    t = (e & ~0xf) << 4
    p = u32(t + ((iova >> 12) & 0xff) * 4)
    return ((p & ~0xff) << 4) | (iova & 0xfff)
def peak(name, iova, nbytes, ch, w):
    pa = v2p(iova)
    d = b''.join(rd(v2p(iova + o), min(4096, nbytes - o)) for o in range(0, nbytes, 4096))
    fmt = '<%d%s' % (len(d) // w, 'i' if w == 4 else 'h')
    s = struct.unpack(fmt, d)
    sx = lambda x: ((x & 0xffffff) ^ 0x800000) - 0x800000 if w == 4 and x & ~0xffffff in (0, -0x1000000) else x
    print(name, hex(pa), ' '.join('ch%d=%x' % (c, max(abs(sx(x)) for x in s[c::ch])) for c in range(ch)))
print('lv1', hex(lv1))
peak('VDMA0', 0x94000000, 8192, 2, 2)
peak('HW0  ', 0x91100000, 8192, 2, 4)
wch = int(sys.argv[1]) if len(sys.argv) > 1 else 2
peak('WDMA3', 0x92300000, 480*wch*4*4, wch, 4)
peak('RDMA7', 0x91700000, 480*4*4*4, 4, 4)
