# cpmem.py [secs [base_hex nMB]]: which MB of the CP's memory change between
# two reads <secs> apart, read only. Default 0xf0000000, 256 MB. The CP's
# code/data (0xf0000000..0xf77fffff) is TrustZone-protected after the
# security request: the AP reads zeros there; VSS, IPC and the rest are
# readable. Shows whether the CP is still writing its shared memory.
import hashlib, mmap, os, sys, time
MB = 1 << 20
secs = float(sys.argv[1]) if sys.argv[1:] else 2
BASE = int(sys.argv[2], 16) if sys.argv[2:] else 0xf0000000
N = int(sys.argv[3]) if sys.argv[3:] else 256
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
def snap():
    out = []
    for i in range(N):
        m = mmap.mmap(fd, MB, mmap.MAP_SHARED, mmap.PROT_READ, offset=BASE + i * MB)
        out.append(hashlib.md5(m[:]).digest())
        m.close()
    return out
a = snap(); time.sleep(secs); b = snap()
ch = [BASE + i * MB for i in range(N) if a[i] != b[i]]
print("changed MB:", len(ch), " ".join("%x" % x for x in ch[:40]))
