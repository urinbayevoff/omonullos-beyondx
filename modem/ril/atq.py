# AT commands on one channel only (the RIL owns umts_ipc0/rfs0): atq.py DEV CMD...
import os, select, sys, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)
for cmd in sys.argv[2:]:
    os.write(fd, (cmd + "\r").encode()); buf = b""; end = time.time() + 8
    while time.time() < end and not any(k in buf for k in (b"OK\r\n", b"ERROR")):
        if select.select([fd], [], [], 0.3)[0]: buf += os.read(fd, 4096)
    print(cmd, "=>", buf.decode(errors="replace").replace("\r\n", " | ").strip(), flush=True)
