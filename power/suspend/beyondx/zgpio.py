#!/usr/bin/env python3
# zgpio: one GPIO line through the kernel's GPIO character device (uAPI v2),
# for the S10 5G's sleep work without libgpiod.
#   zgpio.py get CHIP LINE          the line's level, as an input (0/1)
#   zgpio.py hold CHIP LINE [EDGE]  request it as an input with edge events
#                                   (falling by default) and hold it until
#                                   killed: the pin controller makes it an
#                                   EINT, so the PMU's wake mask can use it
# CHIP: /dev/gpiochipN or N. Run as root.
import ctypes, fcntl, os, signal, sys

GPIO_V2_LINES_MAX = 64
GPIO_MAX_NAME_SIZE = 32
GPIO_V2_LINE_NUM_ATTRS_MAX = 10
FLAG_INPUT = 1 << 2
FLAG_EDGE_RISING = 1 << 4
FLAG_EDGE_FALLING = 1 << 5


class Attr(ctypes.Structure):
	_fields_ = [('id', ctypes.c_uint32), ('padding', ctypes.c_uint32), ('value', ctypes.c_uint64)]


class ConfigAttr(ctypes.Structure):
	_fields_ = [('attr', Attr), ('mask', ctypes.c_uint64)]


class LineConfig(ctypes.Structure):
	_fields_ = [('flags', ctypes.c_uint64), ('num_attrs', ctypes.c_uint32),
		('padding', ctypes.c_uint32 * 5), ('attrs', ConfigAttr * GPIO_V2_LINE_NUM_ATTRS_MAX)]


class LineRequest(ctypes.Structure):
	_fields_ = [('offsets', ctypes.c_uint32 * GPIO_V2_LINES_MAX),
		('consumer', ctypes.c_char * GPIO_MAX_NAME_SIZE), ('config', LineConfig),
		('num_lines', ctypes.c_uint32), ('event_buffer_size', ctypes.c_uint32),
		('padding', ctypes.c_uint32 * 5), ('fd', ctypes.c_int32)]


class LineValues(ctypes.Structure):
	_fields_ = [('bits', ctypes.c_uint64), ('mask', ctypes.c_uint64)]


def _iowr(nr, size):
	return (3 << 30) | (size << 16) | (0xB4 << 8) | nr


GET_LINE = _iowr(0x07, ctypes.sizeof(LineRequest))
GET_VALUES = _iowr(0x0E, ctypes.sizeof(LineValues))


def request(chip, line, flags):
	path = chip if chip.startswith('/') else '/dev/gpiochip' + chip
	fd = os.open(path, os.O_RDWR)
	try:
		req = LineRequest()
		req.offsets[0] = line
		req.num_lines = 1
		req.consumer = b'zgpio'
		req.config.flags = flags
		fcntl.ioctl(fd, GET_LINE, req)
		return req.fd
	finally:
		os.close(fd)


def level(lfd):
	v = LineValues(0, 1)
	fcntl.ioctl(lfd, GET_VALUES, v)
	return v.bits & 1


def main():
	if len(sys.argv) < 4 or sys.argv[1] not in ('get', 'hold'):
		sys.exit(__doc__ or 'usage: zgpio.py get|hold CHIP LINE [falling|rising|both]')
	what, chip, line = sys.argv[1], sys.argv[2], int(sys.argv[3])
	if what == 'get':
		lfd = request(chip, line, FLAG_INPUT)
		print(level(lfd))
		return
	edge = sys.argv[4] if len(sys.argv) > 4 else 'falling'
	flags = FLAG_INPUT | {'falling': FLAG_EDGE_FALLING, 'rising': FLAG_EDGE_RISING,
		'both': FLAG_EDGE_FALLING | FLAG_EDGE_RISING}[edge]
	lfd = request(chip, line, flags)
	print('held %s %d (%s), level %d' % (chip, line, edge, level(lfd)), flush=True)
	signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
	import struct, time
	while True:
		ev = os.read(lfd, 48)	# struct gpio_v2_line_event: u64 ts_ns, u32 id, offset, seqno, line_seqno
		if len(ev) >= 12:
			ts, eid = struct.unpack_from('<QI', ev)
			print('%s event %s (%d)' % (time.strftime('%T'), 'rising' if eid == 1 else 'falling', eid), flush=True)


if __name__ == '__main__':
	main()
