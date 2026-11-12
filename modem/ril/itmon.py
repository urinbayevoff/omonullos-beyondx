# itmon.py [NODE...]: read the Exynos 9820 bus monitor (ITMON) error
# registers, read only (addresses and layout from Samsung's
# exynos9820-itmon.c). Shows whether a master (the CP) hit a bus error or
# timeout. INT_INFO bit 26 = valid, 27 = occurred, 31:28 = error code
# (0 SLVERR, 1 DECERR, 2 unsupported, 3 power down, 6 timeout).
#   itmon.py on   enable error reporting on all nodes and the timeout
#                 monitor on the slave nodes, as Samsung's itmon_init (the
#                 interrupt has no handler here: we only read the latches)
import mmap, os, struct, sys
NODES = {
    "CP_DATA_0": 0x1A843000, "CP_DATA_1": 0x1A853000, "CORE_DP": 0x1A893000,
    "DREX_IRPS0": 0x1A873000, "DREX_IRPS1": 0x1A883000,
    "SCI_IRPM": 0x1AA43000, "CORE_DP_M": 0x1AA53000,
    "ALIVE": 0x1AC83000, "CP_PERI": 0x1AC73000, "SFR_CORE": 0x1AC33000,
    "TREX_DP_CORE": 0x1AC23000, "CPUCL0": 0x1AC43000,
    "SP": 0x1B1D3000, "BUSC_DP": 0x1B1E3000,
}
SLAVES = {"CORE_DP", "DREX_IRPS0", "DREX_IRPS1", "ALIVE", "CP_PERI",
          "SFR_CORE", "TREX_DP_CORE", "CPUCL0", "SP", "BUSC_DP"}
TMOUT = 0x2000
KIND = {0x0: "req_r", 0x20: "req_w", 0x40: "resp_r", 0x60: "resp_w"}
if sys.argv[1:] == ["on"]:
    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    for name, base in NODES.items():
        m = mmap.mmap(fd, 0x3000, mmap.MAP_SHARED, offset=base)
        w = lambda o, v: struct.pack_into("<I", m, o, v)
        if name in SLAVES:
            w(TMOUT + 0x10, 1)		# DBG_CTL: timeout monitor on
            w(TMOUT + 0x14, 0xFFFFF)	# TMOUT_INIT_VAL, as stock
        for off in KIND:
            w(off + 4, 1)		# INT_CLR
            w(off, 1)			# INT_MASK: report errors
        m.close()
    sys.exit(0)
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
for name in sys.argv[1:] or NODES:
    m = mmap.mmap(fd, 0x3000, mmap.MAP_SHARED, mmap.PROT_READ, offset=NODES[name])
    r = lambda o: struct.unpack_from("<I", m, o)[0]
    out = []
    for off, k in KIND.items():
        mask, info = r(off), r(off + 8)
        s = "%s mask %x info %08x" % (k, mask, info)
        if info & (3 << 26):
            s += " ext %08x %08x %08x" % (r(off + 0x10), r(off + 0x14), r(off + 0x18))
        out.append(s)
    if name in SLAVES:
        out.append("tmout ctl %x buf %08x" % (r(TMOUT + 0x10), r(TMOUT + 0x1C)))
    print("%-12s %s" % (name, " | ".join(out)))
    m.close()
