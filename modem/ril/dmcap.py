# Record the LTE CP's diagnostic (SDM) stream from /dev/umts_dm0 to a file,
# for dmparse.py on the PC. Packet format and start sequence as in SCAT
# (github.com/fgsect/scat, parsers/samsung/sdmcmd.py, init_diag).
#   python3 dmcap.py OUT SECONDS
import os, select, struct, sys, time

def pkt(cmd, payload, direction=0xa0, group=0):
    n = 2 + 3 + 4 + len(payload) + 2
    return b'\x7f' + struct.pack('<HBHHBBBL', n + 3, 0, n, 0, direction, group, cmd, 0) + payload + b'\x7e'

out, secs = sys.argv[1], float(sys.argv[2])
fd = os.open('/dev/umts_dm0', os.O_RDWR | os.O_NONBLOCK)
init = [pkt(0x00, struct.pack('>L', 0x41414141)),	# CONTROL_START
	pkt(0x06, b'\x05')]				# update period
for sel in (0x10, 0x20, 0x30, 0x40, 0x44):		# common, LTE, EDGE, HSPA, CDMA items: all
	init.append(pkt(sel, b'\xff'))
for ref in (0x12, 0x22, 0x32, 0x42, 0x46):
	init.append(pkt(ref, b'\xff'))
with open(out, 'wb') as f:
	for p in init:
		os.write(fd, p)
		time.sleep(0.05)
	end = time.time() + secs
	n = 0
	while time.time() < end:
		if select.select([fd], [], [], 0.5)[0]:
			b = os.read(fd, 65536)
			f.write(b); n += len(b)
	f.flush()
os.write(fd, pkt(0x02, b''))	# CONTROL_STOP
print('dmcap: %d bytes' % n)
