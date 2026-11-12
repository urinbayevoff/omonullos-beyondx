# s5100spi.py: first stage of the 5G modem's (S5100) boot from user space:
# its bootloader (the BOOT entry of RADIO2's TOC, read only) over SPI_CAM0
# (0x10850000) as Samsung's boot_device_spi.c IOCTL_MODEM_XMIT_BOOT does,
# then wait for CP2AP_WAKEUP (gpm30) to go high. The modem is powered
# off and on here (s5100gpio.py's sequence), the image follows 30 ms after
# reset as with stock cbd, with the SPI block set up beforehand.
# SPI: 8-bit, mode 0, 1.625 MHz as stock (spi-max-frequency): the SPI block
# divides its source by 4, and stock's clk_set_rate(6.5 MHz) can only be met
# from OSCCLK: MUX_CLKCMU_PERIC1_SPI_CAM0_USER -> OSCCLK_PERIC1 (26 MHz),
# DIV_CLK_PERIC1_SPI_CAM0 /4 (from PLL_SHARED2 the slowest is 12.5 MHz).
# Chip select held low by hand for the whole image, so a FIFO underrun only
# pauses the clock.
import mmap, os, struct, sys, time
sys.stdout.reconfigure(line_buffering=True)
SPI, GPC1, DIV, MUX = 0x10850000, 0x10830080, 0x10801810, 0x108001a0
GPM30_DAT = 0x15c303c4
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
maps = {}
def reg(a):
    p = a & ~0xfff
    if p not in maps:
        maps[p] = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, offset=p)
    return maps[p], a & 0xfff
def rd(a):
    m, o = reg(a); return struct.unpack_from("<I", m, o)[0]
def wr(a, v):
    m, o = reg(a); struct.pack_into("<I", m, o, v)

def boot_image():
    dev = os.path.realpath("/dev/disk/by-partlabel/radio2")
    with open(dev, "rb") as f:
        toc = f.read(0x200)
        for i in range(0, 0x200, 32):
            name = toc[i:i + 12].split(b"\0")[0]
            off, addr, size = struct.unpack_from("<3I", toc, i + 12)
            if name == b"BOOT":
                f.seek(off)
                return f.read(size)
    sys.exit("s5100spi: no BOOT in radio2's TOC")

img = boot_image()
print("BOOT %d bytes" % len(img))
# pins gpc1-0..3: SPI (function 2), no pull
wr(GPC1, (rd(GPC1) & ~0xffff) | 0x2222)
wr(GPC1 + 8, rd(GPC1 + 8) & ~0xffff)
# clock: OSCCLK 26 MHz / 4 = 6.5 MHz source, 1.625 MHz SCK
wr(MUX, rd(MUX) & ~(1 << 4))
wr(DIV, (rd(DIV) & ~0xf) | 3)
time.sleep(0.001)
# USI_CON: the block comes out of S-Boot held in reset (1); stock's
# exynos_usi_init writes 0 before any transfer
wr(SPI + 0xc4, 0)
# controller: reset, byte mode, no DMA/irq, mode 0, manual CS
wr(SPI + 0x10, 0)			# INT_EN
wr(SPI + 0x28, 0)			# SWAP_CFG
wr(SPI + 0x0c, 1)			# CS inactive
wr(SPI + 0x00, 1 << 5)			# CH_CFG: SW_RST
time.sleep(0.001)
wr(SPI + 0x00, 0)
wr(SPI + 0x24, 0x1f); wr(SPI + 0x24, 0)	# PENDING_CLR
wr(SPI + 0x08, 0)			# MODE_CFG: byte, no DMA
wr(SPI + 0x2c, 1)			# FB_CLK: feedback delay 1
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import s5100gpio as g			# sets nothing on import with no argv
g.set_out("nreset", 0); g.set_out("cp_pwr_on", 0)
time.sleep(0.5)
g.set_out("ap2cp_wake", 0)	# stock get_pdata: output 0
g.set_out("pda_active", 1); g.set_out("dump_noti", 0)
g.set_out("cp_pwr_on", 1); time.sleep(0.05); g.set_out("nreset", 1)
time.sleep(0.03)
wr(SPI + 0x24, 0x1f); wr(SPI + 0x24, 0)	# PENDING_CLR again
DBG = bool(os.environ.get("DBG"))
if DBG:
    print("before: ch %08x mode %08x cs %08x st %08x usi %08x" % (rd(SPI), rd(SPI + 8), rd(SPI + 0xc), rd(SPI + 0x14), rd(SPI + 0xc4)))
if os.environ.get("MANUALCS"):
    wr(SPI + 0x0c, 0)			# CS active, by hand
else:					# stock: auto CS, nCS high 2 cycles
    wr(SPI + 0x0c, (1 << 1) | (2 << 4))	# between frames (enable_cs)
wr(SPI + 0x00, 1)			# TX channel on
t0 = time.monotonic()
# 32-byte chunks, each sent out before the next (the FIFO overruns at 52
# bytes here, and the status register's FIFO
# level field is not laid out as spi-s3c64xx expects; TX_DONE, bit 25, is
# reliable: FIFO empty and the shifter idle)
m, o = reg(SPI + 0x18)
n = len(img)
for i in range(0, n, 32):
    for b in img[i:i + 32]:
        struct.pack_into("<I", m, o, b)
    t1 = time.monotonic()
    while not (rd(SPI + 0x14) >> 25) & 1:
        if time.monotonic() - t1 > 0.5:
            sys.exit("s5100spi: TX stuck at byte %d, status %08x" % (i, rd(SPI + 0x14)))
    if DBG and rd(SPI + 8) != 0:
        print("MODE_CFG changed to %08x at byte %d" % (rd(SPI + 8), i)); DBG = False
    if rd(SPI + 0x14) & 0x3c:
        sys.exit("s5100spi: FIFO error at byte %d, status %08x" % (i, rd(SPI + 0x14)))
dt = time.monotonic() - t0
wr(SPI + 0x0c, 1)			# CS inactive
wr(SPI + 0x00, 0)
print("sent in %.3f s (%.2f Mbit/s), status %08x" % (dt, n * 8 / dt / 1e6, rd(SPI + 0x14)))
for k in range(200):			# as check_cp_status: 200 x 20 ms
    if rd(GPM30_DAT) & 1:
        print("CP2AP_WAKEUP high after %d ms" % (k * 20)); break
    time.sleep(0.02)
else:
    print("CP2AP_WAKEUP stayed low")
