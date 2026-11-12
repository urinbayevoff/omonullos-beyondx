#!/usr/bin/env python3
"""zacpm: the Galaxy S10 5G's PMICs through the APM firmware (ACPM), from
user space.

  zacpm info                       the MFD channel the APM firmware publishes
  zacpm read <type> <reg> [count]  read PMIC registers (type: 1 = S2MPS19
                                   PMIC, 2 = its RTC; speedy channel 0)
  zacpm ldo <n>...                 S2MPS19 LDOn: control register, on/off,
                                   voltage (read only)
  zacpm ldo2 <n>...                S2MPS20 (sub PMIC, speedy 1) LDOn, the
                                   same, read only
  zacpm meter [mux]                the S2MPS19 current meter: each channel's
                                   BUCK and its current (mA, ~16 s average);
                                   mux: the BUCK each channel reads, as set
  zacpm meter set <r0>..<r7>       point the 8 channels at rails r0..r7 (BUCK
                                   1-12, or LDOn as Ln, 1-29) and turn it on
  zacpm meter2 [mux|set r0 r1 r2]  the same for the S2MPS20 (3 channels,
                                   BUCK 1-3 or L1-L12)
  zacpm touch on|off               the touch rails LDO19M (1.8 V) and LDO27M
                                   (3.0 V): enable bits 7:6 only
  zacpm codec on|off               the CS47L93's core rail LDO18M (1.2 V):
                                   enable bits 7:6 only
  zacpm dvfs                       what the APM's DVFS says each domain runs
                                   at (FREQ_GET), and the CPU PLLs
  zacpm dvfs table                 the APM's rate/voltage levels (fvmap)
  zacpm dvfs set cpuclN|g3d|mif|int <MHz>
                                   a CPU cluster, the GPU, DRAM (MIF) or the
                                   internal bus to one of its fvmap levels,
                                   no higher than the stock maximum
  zacpm dvfs init mif|int          SET_INIT_FREQ as stock exynos-devfreq
                                   does at boot, at the stock DT boot_info
                                   rate (MIF 2093000, INT 534000 kHz)
  zacpm rtc [--epoch]              the S2MPS19 RTC's time (UTC), which runs
                                   while the phone is off
  zacpm rtc set                    write the system time (UTC) to it

Added for OmonullOS, October 2026. The only writes are the touch rails' enable
bits, CPU and G3D DVFS requests (the owner's «ha»s, 2026-10-17), MIF and INT
DVFS requests (2026-10-18; stock exynos-devfreq's cal_dfs_set_rate is the same
FREQ_REQ), AUD DVFS during calls (owner's «ha» 2026-10-19 17:10), the codec rail
LDO18M's enable bits (owner's «ha» 2026-11-11) and the RTC's
UPDATE and time registers (2026-10-18), the current meter's ADC_CTRL1..3 and
ADC_DATA (0xbf-0xc2, owner's «ha» 2026-10-18 23:30) and the S2MPS20's
(0x49-0x4c, «ha» 2026-10-18 23:55); anything else needs
its own «ha» (CLAUDE.md rules 3 and 5). DVFS never names a voltage: the APM
takes it from its own table for the level asked for. The voltage bits are checked against the stock
values first, and the old value is put back if the read-back is off.

The S2MPS19 sits on a speedy bus that only the APM reaches. The AP puts a
four-word command into a ring in the APM's shared SRAM and rings its mailbox;
the answer comes back in a second ring. This is the S9 port's s9p-acpm.c
(third_party/s9plus/src/soc), which follows Samsung's acpm_ipc.c /
acpm_mfd.c, moved to the 9820: mailbox 0x15900000 and initdata 0x7000 from
the stock tree's acpm_ipc node (the 9810 had 0x14100000 and 0x7f00), the
MFD channel is 2 there too (s2mps19mfd: acpm-ipc-channel 2, speedy 0).
DVFS requests go on channel 0 (stock clock-controller@1a240000 node; the
acpm_dvfs node's channel 5 is a one-entry buffer the APM writes MIF
notifications to): Samsung's cal-if acpm_dvfs.c sends
{domain, rate in kHz, FREQ_REQ 0 or FREQ_GET 1, 0} and the APM picks the PLL
and, from the chip's fused ASV table, the voltage; the answer's word 1 is the
rate. Domains: acpm_dvfs_exynos9820.h.
The command is Samsung's set_protocol layout: word 0 = channel << 12 |
type << 8 | reg, sequence number in bits 21:16; word 1 = function (0 read)
and, in the answer, the value in bits 15:8 and the firmware's return code in
31:24.

Every access is a 32-bit load or store through a /dev/mem mapping (the
mailbox is a register block). The SRAM's first three pages fault, so the
mapping starts at 0x3000, as on the 9810.
"""
import fcntl
import mmap
import os
import sys
import time

MBOX_PHYS = 0x15900000
SRAM_PHYS = 0x02039000
SRAM_SIZE = 0x30000
SRAM_MAP_OFF = 0x3000
INITDATA = 0x7000

INTGR0, INTCR1, INTSR1 = 0x08, 0x20, 0x28
MFD_CHANNEL = 2
DVFS_CHANNEL = 0
FREQ_REQ, FREQ_GET = 0, 1
# No SET_INIT_FREQ (DATA_INIT 5, 3) for the CPU domains: Samsung sends it for
# devfreq domains only, and for cpucl0 it hung the APM (bx26, log.md 08:14).
# exynos-acme only ever sends FREQ_REQ and reads the rate back from the CMU.
# The DVFS plugin's fvmap (acpm.c plugins_init: plugin base + the stock acpm
# node's fvmap_offset 0xa400): a 24-byte fvmap_header per domain, then each
# domain's {rate kHz, volt uV} levels at o_ratevolt, highest first. Read on
# bx26: the init levels are the rates S-Boot left (1053 / 1222 / 1378 MHz).
FVMAP = 0x1ec00 + 0xa400
# Stock maxima (Samsung's policy for the G977N); the fvmap goes higher. g3d:
# the stock mali node's gpu_max_clock. Samsung's GPU driver sends FREQ_REQ
# (cal_dfs_set_rate) for it too, never SET_INIT_FREQ.
# exynos-devfreq's exynos_acpm_set_init_freq: {id, kHz, DATA_INIT, SET_INIT_FREQ};
# stock sends it for the two use_acpm devfreqs only, never for the CPUs (a
# SET_INIT_FREQ to a CPU domain hangs the APM). Owner's «ha», 2026-10-17 16:40.
DATA_INIT, SET_INIT_FREQ = 5, 3
DVFS_INIT_KHZ = {"mif": 2093000, "int": 534000}
DVFS_MAX_MHZ = {"cpucl0": 1950, "cpucl1": 2314, "cpucl2": 2730, "g3d": 702,
                "mif": 2093, "int": 534,
                # ABOX: stock's pm-qos-aud gear 1, asked for during calls
                "aud": 1180}
# CMU_CPUCLn: PLL_CON0 (M 25:16, P 13:8, S 2:0); cluster 0 runs off its APLL
# while MUX_PLL_CPUCL0 (0x1008) is 1; CMU_G3D PLL_G3D (156 MHz from S-Boot)
PLL = {"cpucl0": (0x1d020000, 0x160, 0x100), "cpucl1": (0x1d030000, 0x120, None),
       "cpucl2": (0x1d120000, 0x120, None), "g3d": (0x1a240000, 0x140, None)}
DVFS_DOMAINS = ("mif", "int", "cpucl0", "cpucl1", "cpucl2", "npu", "disp", "score",
                "aud", "cp", "g3d", "intcam", "cam", "iva", "mfc")
SPEEDY_CHANNEL = 0
FUNC_READ, FUNC_UPDATE = 0, 2
FW_IPC_CHANNELS, FW_IPC_AP_MAX = 2, 6
CH_SIZE = 18 * 4
CH = dict(id=0, tx_rear=4, tx_front=5, tx_base=6, rx_rear=10, rx_front=11,
          rx_base=12, q_len=13, q_elem_size=14)
REPLY_TIMEOUT = 0.6
RETRIES = 3
LOCK = "/run/zacpm.lock"
# S2MPS19 current meter (Samsung regulator/s2mps19_powermeter.c, async mode as
# the stock DT: adc_mode 1, adc_sync_mode 2). CTRL3: EN bit 7, pointer 5:0;
# pointer 0x20+ch selects channel ch's mux (DATA = BUCK number), 0x00+ch its
# 8-bit current. Mux 1-12 = BUCKn, 0x41-0x5d = LDO1-29. mA per step: BUCK2
# and BUCK6 31.25 (dual phase), other BUCKs 15.625; LDOs by their rating
# (current_ldo_coeffs: 150 mA -> 1.171815, 300 -> 2.34375, ... 800 -> 6.25).
# The S2MPS20's (s2mps20_powermeter.c, speedy 1): 3 channels, mux pointer
# 0x10+ch, mux 1-3 = BUCKn (BUCK1 dual phase), 0x11-0x1c = LDO1-12.
METER = {
    "m": dict(speedy=None, regs=(0xbf, 0xc0, 0xc1, 0xc2), nch=8, mux=0x20, nbuck=12,
              dual=(2, 6), ldo=(300, 450, 800, 600, 450, 150, 150, 300, 150, 300, 150, 150,
                                150, 150, 800, 300, 450, 600, 150, 300, 300, 150, 300, 150,
                                450, 300, 450, 150, 300)),
    "s": dict(speedy=1, regs=(0x49, 0x4a, 0x4b, 0x4c), nch=3, mux=0x10, nbuck=3,
              dual=(1,), ldo=(300, 450, 300, 150, 150, 150, 300, 450, 600, 300, 150, 150)),
}
LDO_STEP = {150: 1.171815, 300: 2.34375, 450: 3.515625, 600: 4.6875, 800: 6.25}


def ldo_base(c):	# mux value of LDO1: 0x41 on the S2MPS19, 0x11 on the S2MPS20
    return 0x41 if c["nbuck"] == 12 else 0x11


def rail(c, m, pm):
    return "BUCK%d%s" % (m, pm.upper()) if m <= c["nbuck"] else "LDO%d%s" % (
        m - ldo_base(c) + 1, pm.upper())


def rail_step(c, m):
    if m <= c["nbuck"]:
        return 31.25 if m in c["dual"] else 15.625
    return LDO_STEP[c["ldo"][m - ldo_base(c)]]
ADC_EN, ADC_ASYNCRD, ADC_CAL_EN, ADC_PTR = 0x80, 0x80, 0x40, 0x3f

# S2MPS19 (Samsung s2mps19-regulator.h): LnCTRL, enable bits 7:6, VSEL 5:0
L1CTRL = 0x46
ENABLE_SHIFT, VSEL_MASK = 6, 0x3f
ENABLE_MASK = 3 << ENABLE_SHIFT

# The registers zacpm may write, with the VSEL the stock tree sets for them
# (dtbo 08: tsp_io 1.8 V, tsp_avdd 3.0 V); nothing else is ever written.
WRITABLE = {0x58: ("LDO19M", 44), 0x60: ("LDO27M", 48)}
# The codec's core rail (stock beyondx DT ldo18_reg: LDO18M vdd_codec_1p2,
# DCVDD/CPVDD2 of the CS47L93, 1.2 V = 700 mV + 40 x 12.5 mV, not always on;
# the owner's «ha» 2026-11-11). Kept apart from WRITABLE: "touch" walks that.
CODEC_RAILS = {0x57: ("LDO18M", 40)}

# S2MPS19 RTC (type 2; Samsung rtc-s2mps19.c, rtc-s2mp.h without
# CONFIG_RTC_DRV_S2MP): binary, 24 h with bit 6 set from 12:00, weekday as
# one bit, year from 2000. UPDATE bit 0 (RUDR) latches the counters into
# the time registers for reading, bit 1 (WUDR) loads them from there.
RTC_UPDATE, RTC_SEC, RUDR, WUDR = 0x02, 0x05, 0x01, 0x02


class Mem:
    def __init__(self, phys, size):
        self.fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        self.m = mmap.mmap(self.fd, size, mmap.MAP_SHARED,
                           mmap.PROT_READ | mmap.PROT_WRITE, offset=phys)
        self.w = memoryview(self.m).cast("I")

    def rd(self, off):
        return self.w[off // 4]

    def wr(self, off, v):
        self.w[off // 4] = v & 0xffffffff


class Acpm:
    def __init__(self, channel=MFD_CHANNEL):
        self.mbox = Mem(MBOX_PHYS, 0x1000)
        self.sram = Mem(SRAM_PHYS + SRAM_MAP_OFF, SRAM_SIZE - SRAM_MAP_OFF)
        self.ch = self.find_channel(channel)
        self.seq = (int(time.time()) % 63) + 1

    def s(self, off):
        return self.sram.rd(off - SRAM_MAP_OFF)

    def sw(self, off, v):
        self.sram.wr(off - SRAM_MAP_OFF, v)

    def find_channel(self, cid):
        table = self.s(INITDATA + FW_IPC_CHANNELS * 4)
        ap_max = self.s(INITDATA + FW_IPC_AP_MAX * 4)
        if not 0 < ap_max < 32 or not SRAM_MAP_OFF <= table < SRAM_SIZE:
            raise SystemExit("zacpm: no APM channel table at initdata 0x%x "
                             "(channels 0x%x, ap_max %d)" % (INITDATA, table, ap_max))
        for i in range(ap_max):
            base = table + i * CH_SIZE
            if self.s(base) & 0xff == cid:
                ch = {k: self.s(base + 4 * v) for k, v in CH.items()}
                ch["id"] &= 0xff
                if ch["q_len"] < 2 or ch["q_elem_size"] < 16:
                    raise SystemExit("zacpm: channel %d unusable: %r" % (cid, ch))
                return ch
        raise SystemExit("zacpm: no channel %d among %d" % (cid, ap_max))

    def consume_rx(self, want=None):
        c = self.ch
        front, rear = self.s(c["rx_front"]), self.s(c["rx_rear"])
        found = None
        i = rear
        while i != front:
            base = c["rx_base"] + c["q_elem_size"] * i
            w0 = self.s(base)
            if want is not None and (w0 >> 16) & 0x3f == want:
                found = [self.s(base + 4 * k) for k in range(4)]
            i = (i + 1) % c["q_len"]
        if rear != front:
            self.sw(c["rx_rear"], front)
        self.mbox.wr(INTCR1, 1 << c["id"])
        return found

    def send(self, cmd):
        c = self.ch
        front = self.s(c["tx_front"])
        nxt = (front + 1) % c["q_len"]
        t0 = time.monotonic()
        while nxt == self.s(c["tx_rear"]):
            if time.monotonic() - t0 > 0.02:
                return False
            time.sleep(0.0001)
        for k in range(4):
            self.sw(c["tx_base"] + c["q_elem_size"] * front + 4 * k, cmd[k])
        self.sw(c["tx_front"], nxt)
        self.mbox.wr(INTGR0, (1 << c["id"]) << 16)
        return True

    def xfer(self, func, typ, reg, val=0, mask=0, speedy=None):
        # word 1: function, then the update value in 15:8 and mask in 23:16
        # (Samsung acpm_mfd.h SHIFT_UPDATE_VAL / SHIFT_UPDATE_MASK)
        speedy = SPEEDY_CHANNEL if speedy is None else speedy
        cmd = [(speedy & 0xf) << 12 | (typ & 0xf) << 8 | (reg & 0xff),
               (func & 0xff) | (val & 0xff) << 8 | (mask & 0xff) << 16, 0, 0]
        r = self.call(cmd)
        if r is None:
            raise SystemExit("zacpm: no answer from the APM (type %d reg 0x%02x)" % (typ, reg))
        return (r[1] >> 8) & 0xff, (r[1] >> 24) & 0xff

    def call(self, cmd):
        """Send four words, return the answer's four (None: no answer).
        One caller at a time: zgov, zgpu and a hand-run zacpm share the rings."""
        with open(LOCK, "a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            return self._call(list(cmd))

    def _call(self, cmd):
        for _ in range(RETRIES):
            self.consume_rx()
            self.seq = self.seq % 63 + 1
            cmd[0] = (cmd[0] & ~(0x3f << 16)) | (self.seq << 16)
            if not self.send(cmd):
                time.sleep(0.002)
                continue
            t0 = time.monotonic()
            while time.monotonic() - t0 < REPLY_TIMEOUT:
                pending = self.mbox.rd(INTSR1) & (1 << self.ch["id"])
                if not pending and self.s(self.ch["rx_rear"]) == self.s(self.ch["rx_front"]):
                    time.sleep(0.0001)
                    continue
                r = self.consume_rx(self.seq)
                if r:
                    return r
        return None

    def read(self, typ, reg, speedy=None):
        val, ret = self.xfer(FUNC_READ, typ, reg, speedy=speedy)
        if ret:
            raise SystemExit("zacpm: APM returned %d for type %d reg 0x%02x" % (ret, typ, reg))
        return val

    def set_enable(self, reg, on):
        """Set or clear LnCTRL's enable bits, for WRITABLE registers only."""
        rails = {**WRITABLE, **CODEC_RAILS}
        if reg not in rails:
            raise SystemExit("zacpm: reg 0x%02x is not writable" % reg)
        name, vsel = rails[reg]
        old = self.read(1, reg)
        if old & VSEL_MASK != vsel:
            raise SystemExit("zacpm: %s 0x%02x has vsel %d, stock is %d; "
                             "not touched" % (name, old, old & VSEL_MASK, vsel))
        want = (old & ~ENABLE_MASK) | (ENABLE_MASK if on else 0)
        if old != want:
            _, ret = self.xfer(FUNC_UPDATE, 1, reg, want & ENABLE_MASK, ENABLE_MASK)
            new = self.read(1, reg)
            if ret or new != want:
                self.xfer(FUNC_UPDATE, 1, reg, old & ENABLE_MASK, ENABLE_MASK)
                raise SystemExit("zacpm: %s: APM returned %d, reads 0x%02x, wanted "
                                 "0x%02x; put back 0x%02x (reads 0x%02x)" %
                                 (name, ret, new, want, old, self.read(1, reg)))
        print("%s ctrl 0x%02x: 0x%02x -> 0x%02x" % (name, reg, old, want))


def rtc_read(a):
    import calendar
    a.xfer(FUNC_UPDATE, 2, RTC_UPDATE, RUDR, RUDR)
    time.sleep(0.002)
    s, mi, h, _, d, mo, y = (a.read(2, RTC_SEC + i) for i in range(7))
    tm = (2000 + (y & 0x7f), mo & 0x0f, d & 0x1f, h & 0x1f, mi & 0x7f, s & 0x7f)
    try:
        return calendar.timegm(tm + (0, 0, 0)), tm
    except (ValueError, OverflowError):
        return None, tm


def rtc_set(a):
    t = time.gmtime()
    if t.tm_year < 2026:
        raise SystemExit("zacpm: the system time (%d) is not set; RTC not written" % t.tm_year)
    vals = [t.tm_sec, t.tm_min, t.tm_hour | (0x40 if t.tm_hour >= 12 else 0),
            1 << ((t.tm_wday + 1) % 7), t.tm_mday, t.tm_mon, t.tm_year - 2000]
    for i, v in enumerate(vals):
        a.xfer(FUNC_UPDATE, 2, RTC_SEC + i, v, 0xff)
    a.xfer(FUNC_UPDATE, 2, RTC_UPDATE, WUDR, WUDR)
    time.sleep(0.002)


def fvmap(a):
    """{domain: (init level, [(kHz, uV), ...])} from the APM's SRAM."""
    import struct

    def rd(off, n):
        b = bytearray()
        for w in range(off & ~3, off + n + 4, 4):
            b += struct.pack("<I", a.s(w))
        return bytes(b[off & 3:(off & 3) + n])
    out = {}
    for i, name in enumerate(DVFS_DOMAINS):
        h = struct.unpack("<6BH2B2x3H3H", rd(FVMAP + 24 * i, 24))
        nlv, init, o_rv = h[1], h[7], h[13]
        out[name] = (init, [struct.unpack("<II", rd(FVMAP + o_rv + 8 * k, 8))
                            for k in range(min(nlv, 32))])
    return out


def pll_mhz(name):
    """What the domain's PLL is set to, read from its CMU."""
    base, con0, apll = PLL[name]
    m = Mem(base, 0x2000)
    v = m.rd(con0)
    if apll is not None and m.rd(0x1008) & 1:
        v = m.rd(apll)
    if not v >> 31:
        return 0.0
    mdiv, pdiv, sdiv = (v >> 16) & 0x3ff, (v >> 8) & 0x3f, v & 7
    return 26.0 * mdiv / pdiv / (1 << sdiv) if pdiv else 0.0


def dvfs(argv):
    a = Acpm(DVFS_CHANNEL)
    fv = fvmap(Acpm())
    if argv[:1] == ["table"]:
        for name, (init, lv) in fv.items():
            print("%-7s init %2d: %s" % (name, init, " ".join(
                "%d/%d" % (r // 1000, v // 1000) for r, v in lv)))
        return 0
    if argv[:1] == ["set"] and len(argv) == 3 and argv[1] in DVFS_MAX_MHZ:
        name, mhz = argv[1], int(argv[2])
        levels = [r for r, _ in fv[name][1]]
        if mhz * 1000 not in levels or mhz > DVFS_MAX_MHZ[name]:
            raise SystemExit("zacpm: %s %d MHz: not a level up to the stock %d MHz (%s)" % (
                name, mhz, DVFS_MAX_MHZ[name], " ".join(str(r // 1000) for r in levels
                                                       if r // 1000 <= DVFS_MAX_MHZ[name])))
        r = a.call([DVFS_DOMAINS.index(name), mhz * 1000, FREQ_REQ, 0])
        print("%s: request %d MHz: %s" % (name, mhz, "no answer" if r is None else "answered"))
    elif argv[:1] == ["init"] and len(argv) == 2 and argv[1] in DVFS_INIT_KHZ:
        name = argv[1]
        r = a.call([DVFS_DOMAINS.index(name), DVFS_INIT_KHZ[name], DATA_INIT, SET_INIT_FREQ])
        print("%s: init %d kHz: %s" % (name, DVFS_INIT_KHZ[name],
                                       "no answer" if r is None else "answered %08x %08x %08x %08x" % tuple(r)))
    elif argv:
        print(__doc__.split("\n\n")[0])
        return 2
    for name in DVFS_DOMAINS:
        r = a.call([DVFS_DOMAINS.index(name), 0, FREQ_GET, 0])
        pll = " (PLL %.0f MHz)" % pll_mhz(name) if name in PLL else ""
        print("%-7s %s%s" % (name, "no answer" if r is None else "%d kHz" % r[1], pll))
    return 0


def meter(a, argv, pm="m"):
    c = METER[pm]
    ADC_CTRL1, ADC_CTRL2, ADC_CTRL3, ADC_DATA = c["regs"]
    nch, sp = c["nch"], c["speedy"]

    def put(reg, val, mask=0xff):	# ADC registers only
        assert ADC_CTRL1 <= reg <= ADC_DATA
        _, ret = a.xfer(FUNC_UPDATE, 1, reg, val, mask, speedy=sp)
        if ret:
            raise SystemExit("zacpm: APM returned %d for reg 0x%02x" % (ret, reg))

    def ptr(p):
        put(ADC_CTRL3, p, ADC_PTR)
        return a.read(1, ADC_DATA, speedy=sp)

    def rd(reg):
        return a.read(1, reg, speedy=sp)

    if argv[:1] == ["set"] and len(argv) == nch + 1:
        lb = ldo_base(c)
        bucks = [lb - 1 + int(r[1:]) if r[:1] in "Ll" else int(r) for r in argv[1:]]
        if not all(1 <= b <= c["nbuck"] or lb <= b < lb + len(c["ldo"]) for b in bucks):
            raise SystemExit("zacpm: rails are BUCK 1-%d or L1-L%d" % (c["nbuck"], len(c["ldo"])))
        put(ADC_CTRL3, 0, ADC_EN)
        if rd(ADC_CTRL1) != 0x2b:	# SMP_NUM 16384, 125 kHz: ~16 s
            put(ADC_CTRL1, 0x2b)
        put(ADC_CTRL2, ADC_CAL_EN, ADC_CAL_EN)
        for ch, b in enumerate(bucks):
            ptr(c["mux"] + ch)
            put(ADC_DATA, b)
        put(ADC_CTRL3, ADC_EN, ADC_EN)
    elif argv not in ([], ["mux"]):
        print(__doc__.split("\n\n")[0])
        return 2
    print("ctrl1..3 %s" % " ".join("0x%02x" % rd(r) for r in (ADC_CTRL1, ADC_CTRL2, ADC_CTRL3)))
    mux = [ptr(c["mux"] + ch) for ch in range(nch)]
    if argv == ["mux"] or not rd(ADC_CTRL3) & ADC_EN:
        print("mux " + " ".join("ch%d=%s" % (ch, rail(c, b, pm)) for ch, b in enumerate(mux)))
        return 0
    put(ADC_CTRL2, ADC_ASYNCRD, ADC_ASYNCRD)
    time.sleep(0.003)
    for ch, b in enumerate(mux):
        v = ptr(ch)
        print("ch%d %-6s %5.0f mA (%d)" % (ch, rail(c, b, pm), v * rail_step(c, b), v))
    return 0


def main(argv):
    if len(argv) > 1 and argv[1] == "dvfs":
        return dvfs(argv[2:])
    if len(argv) > 1 and argv[1] == "rtc":
        a = Acpm()
        if argv[2:] == ["set"]:
            rtc_set(a)
        elif argv[2:] not in ([], ["--epoch"]):
            print(__doc__.split("\n\n")[0])
            return 2
        sec, tm = rtc_read(a)
        if argv[2:] == ["--epoch"]:
            if sec is None:
                return 1
            print(sec)
        else:
            print("%04d-%02d-%02d %02d:%02d:%02d UTC%s" % (tm + ("" if sec else " (invalid)",)))
        return 0
    if argv[1:2] in (["meter"], ["meter2"]):
        return meter(Acpm(), argv[2:], "s" if argv[1] == "meter2" else "m")
    if len(argv) < 2 or argv[1] not in ("info", "read", "ldo", "ldo2", "touch", "codec") or \
            argv[1] in ("touch", "codec") and argv[2:] not in (["on"], ["off"]):
        print(__doc__.split("\n\n")[0])
        return 2
    a = Acpm()
    if argv[1] == "info":
        print("MFD channel %d: %s" % (MFD_CHANNEL,
              " ".join("%s=0x%x" % kv for kv in a.ch.items())))
    elif argv[1] == "read":
        typ, reg = int(argv[2], 0), int(argv[3], 0)
        n = int(argv[4], 0) if len(argv) > 4 else 1
        for r in range(reg, reg + n):
            print("type %d reg 0x%02x = 0x%02x" % (typ, r, a.read(typ, r)))
    elif argv[1] == "touch":
        # I/O rail first on the way up, last on the way down
        on = argv[2] == "on"
        for reg in sorted(WRITABLE, reverse=not on):
            a.set_enable(reg, on)
            time.sleep(0.005)
    elif argv[1] == "codec":
        for reg in CODEC_RAILS:
            a.set_enable(reg, argv[2] == "on")
        time.sleep(0.01)	# stock regulator-enable-ramp-delay 5 ms
    else:
        sub = argv[1] == "ldo2"	# S2MPS20 s2mps20-regulator.h: L1CTRL 0x17
        for s in argv[2:]:
            n = int(s, 0)
            reg = (0x17 if sub else L1CTRL) + n - 1
            v = a.read(1, reg, speedy=1 if sub else None)
            en = (v >> ENABLE_SHIFT) & 3
            print("LDO%d%s ctrl 0x%02x = 0x%02x: enable %d%s, vsel %d" %
                  (n, "S" if sub else "M", reg, v, en, " (on)" if en == 3 else
                   " (off)" if en == 0 else " (by pin/state)", v & VSEL_MASK))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
