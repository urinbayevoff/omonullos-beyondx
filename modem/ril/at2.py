# Hold the RIL channels open (umts_ipc0, umts_rfs0: the modem waits for them),
# wait, show what the modem sends on them, then try AT commands on a channel.
#   python3 at2.py /dev/umts_router [wait-seconds] CMD...
import os, select, sys, time
dev = sys.argv[1]
wait = float(sys.argv[2])
keep = {os.open(p, os.O_RDWR | os.O_NONBLOCK): p for p in ("/dev/umts_ipc0", "/dev/umts_rfs0")}
fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
names = dict(keep); names[fd] = dev
def pump(secs):
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select(list(names), [], [], 0.2)
        for f in r:
            data = os.read(f, 4096)
            print("<- %s %r" % (names[f], data[:200]), flush=True)
pump(wait)
for cmd in sys.argv[3:]:
    os.write(fd, (cmd + "\r").encode())
    print("-> %s" % cmd, flush=True)
    pump(3)
