# A few PMU CP registers, read only. Only these offsets: reading the PMU's
# CP block word by word (0x15863000..0x3100) hangs the bus (watchdog -> TWRP).
import mmap, os, struct
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
p = mmap.mmap(fd, 0x4000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x15860000)
regs = (("CP_CONFIGURATION", 0x3000), ("CP_STATUS", 0x3004), ("CP_OUT", 0x3020),
        ("CP_IN", 0x3024), ("CP_INT_IN", 0x3040), ("CP_INT_EN", 0x3044), ("CP_INT_TYPE", 0x3048))
print(" ".join("%s=%x" % (n, struct.unpack_from("<I", p, o)[0]) for n, o in regs))
