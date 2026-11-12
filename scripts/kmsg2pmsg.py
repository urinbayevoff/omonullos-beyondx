# relay new kernel log records to pstore pmsg and to a file on UFS (fsync per
# record), so the last lines survive a hang and watchdog reset
# (on the phone: /root/tools/kmsg2pmsg.py; run pinned to another CPU,
# e.g. systemd-run --unit=kmsgrelay -p CPUAffinity=7 python3 ...; after a
# hang read /data/root/kmsg-trace.log in TWRP.) Added for OmonullOS, October 2026.
import os
k = os.open('/dev/kmsg', os.O_RDONLY)
os.lseek(k, 0, os.SEEK_END)
p = os.open('/dev/pmsg0', os.O_WRONLY)
f = os.open('/root/kmsg-trace.log', os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
def out(b):
    os.write(p, b)
    os.write(f, b)
    os.fsync(f)
out(b'--- kmsg relay start\n')
while True:
    try:
        r = os.read(k, 8192)
    except BrokenPipeError:
        continue
    out(r.split(b';', 1)[-1])
