# memdiff.py save <file> [base_hex [size_hex]] | diff <a> <b> [base_hex]:
# save a physical range (default the IPC region 0xfb200000, 1 MB) read
# only, or list the 32-bit words that differ between two saves.
import mmap, os, struct, sys
if sys.argv[1] == "save":
    base = int(sys.argv[3], 16) if sys.argv[3:] else 0xfb200000
    size = int(sys.argv[4], 16) if sys.argv[4:] else 0x100000
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
    open(sys.argv[2], "wb").write(m[:])
else:
    a, b = open(sys.argv[2], "rb").read(), open(sys.argv[3], "rb").read()
    base = int(sys.argv[4], 16) if sys.argv[4:] else 0xfb200000
    n = 0
    for o in range(0, min(len(a), len(b)), 4):
        x, y = struct.unpack_from("<I", a, o)[0], struct.unpack_from("<I", b, o)[0]
        if x != y:
            n += 1
            if n <= 80:
                print("%08x +%05x: %08x -> %08x" % (base + o, o, x, y))
    print(n, "words differ")
