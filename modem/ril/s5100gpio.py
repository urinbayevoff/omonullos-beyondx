# s5100gpio.py status|on|off: the Galaxy S10 5G's 5G modem (S5100) power
# and status lines, by the GPIO bank registers (our DT has only the ALIVE
# pin controller). Pins from the stock dtbo (mif2_pdata), sequence from
# Samsung's modem_ctrl_s5100.c s5100_on/s5100_off. Only pins 3 and 4 of
# gph1 are touched (the rest of gph1 is the amplifiers' resets and qbt2000).
import mmap, os, struct, sys, time
PIN = {	# name: (bank base, pin, direction)
    "pda_active": (0x10830100, 3, "out"),	# gph1-3, AP2CP_PDA_ACTIVE
    "nreset":     (0x10830100, 4, "out"),	# gph1-4, AP2CP_NRESET_N
    "cp_pwr_on":  (0x15c30280, 0, "out"),	# gpm20
    "dump_noti":  (0x15c30320, 0, "out"),	# gpm25
    "ap2cp_wake": (0x15c30220, 0, "out"),	# gpm17
    "phone_active": (0x15c30380, 0, "in"),	# gpm28
    "cp2ap_wake": (0x15c303c0, 0, "in"),	# gpm30
    "ps_hold":    (0x15c303e0, 0, "in"),	# gpm31
}
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
maps = {}
def reg(addr):
    page = addr & ~0xfff
    if page not in maps:
        maps[page] = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, offset=page)
    return maps[page], addr & 0xfff
def rd(addr):
    m, o = reg(addr); return struct.unpack_from("<I", m, o)[0]
def wr(addr, v):
    m, o = reg(addr); struct.pack_into("<I", m, o, v)
def get(name):
    b, p, _ = PIN[name]
    return (rd(b + 4) >> p) & 1, (rd(b) >> 4 * p) & 0xf
def set_out(name, val):
    b, p, d = PIN[name]
    assert d == "out"
    dat = rd(b + 4)
    wr(b + 4, (dat & ~(1 << p)) | (val << p))		# value first, then
    con = rd(b)
    wr(b, (con & ~(0xf << 4 * p)) | (1 << 4 * p))	# CON = output
def status():
    for n in PIN:
        v, f = get(n)
        print("%-12s %d (con %x)" % (n, v, f))
if __name__ != "__main__":
    cmd = None
else:
    cmd = sys.argv[1] if sys.argv[1:] else "status"
if cmd == "on":
    set_out("ap2cp_wake", 0)	# stock s5100_get_pdata: output, 0
    set_out("pda_active", 1)
    set_out("dump_noti", 0)
    set_out("nreset", 0)
    set_out("cp_pwr_on", 1)
    time.sleep(0.05)
    set_out("nreset", 1)
elif cmd == "off":
    set_out("nreset", 0)
    set_out("cp_pwr_on", 0)
elif cmd not in ("status", None):
    sys.exit("usage: s5100gpio.py status|on|off")
if cmd:
    status()
