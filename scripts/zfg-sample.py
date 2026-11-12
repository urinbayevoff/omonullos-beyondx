# Sample MAX77705 fuel gauge (0x36) every 1 s: time, QH (0x4d), Current, AvgCurrent, RepSOC. Read only.
import subprocess, sys, time, glob, os
bus = os.path.basename(glob.glob('/sys/bus/platform/devices/15d60000.i2c/i2c-*')[0]).split('-')[1]
def r16(reg):
    o = subprocess.run(['i2ctransfer','-y',bus,'w1@0x36',reg,'r2'],capture_output=True,text=True).stdout.split()
    return int(o[0],16) | (int(o[1],16) << 8)
def s16(v): return v - 65536 if v >= 32768 else v
out = open(sys.argv[1], 'a', buffering=1)
while True:
    try:
        t = time.time(); qh = r16('0x4d'); cur = s16(r16('0x0a')); avg = s16(r16('0x0b')); soc = r16('0x06')
        out.write(f"{t:.2f},{qh},{cur*0.3125:.0f},{avg*0.3125:.0f},{soc/256:.2f}\n")
    except Exception as e:
        out.write(f"{time.time():.2f},err,{e}\n")
    time.sleep(1)
