#!/usr/bin/env python3
"""unmute-test.py: is the CP holding the uplink muted? (run as root)

Waits for the next call (driver param call=1), then, while the call runs,
sends the two "microphone on" requests stock Android may send at every call
start and zcell never does, a few seconds apart, so the far end can tell
which one let the sound through:

  +4 s   RIL_REQUEST_SET_MUTE (53) [0]        (the standard RIL one)
  +12 s  OEM_HOOK_RAW 59  08 0B 00 05 00      (libsecril-client SetMute(0),
                                               the CP's NS_SOUND_MIC_MUTE_REQ)

With `mic-test.sh tone` armed the far end hears a steady 1 kHz beep from the
moment the right request lands. Every answer is printed with its time.
Only RIL requests; no register writes. Added for OmonullOS, November 2026.
"""
import socket
import sys
import time

CALL = '/sys/module/snd_zrn_abox_bx/parameters/call'
STEPS = [(4, 'rawi 53 0', '53'), (12, 'rawx 59 080b000500', '59')]


def say(msg):
	print('%s %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


def ask(cmd, req):
	s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	s.connect('/dev/socket/zril')
	s.sendall((cmd + '\n').encode())
	s.settimeout(1)
	buf, end = '', time.time() + 10
	while time.time() < end:
		try:
			data = s.recv(65536)
		except socket.timeout:
			continue
		if not data:
			break
		buf += data.decode(errors='replace')
		for line in buf.splitlines():
			if line.startswith('< %s ' % req):
				s.close()
				return line
	s.close()
	return 'no answer'


def main():
	wait = float(sys.argv[1]) if len(sys.argv) > 1 else 1800
	say('waiting up to %d s for a call (call=1)...' % wait)
	end = time.time() + wait
	while open(CALL).read().strip() != '1':
		if time.time() > end:
			sys.exit('no call')
		time.sleep(0.3)
	t0 = time.time()
	say('call streams on')
	say('GET_MUTE before: %s' % ask('raw0 54', '54'))
	for at, cmd, req in STEPS:
		while time.time() < t0 + at:
			if open(CALL).read().strip() != '1':
				sys.exit(say('call ended before +%d s' % at))
			time.sleep(0.1)
		say('+%d s  %s -> %s' % (at, cmd, ask(cmd, req)))
	say('GET_MUTE after: %s' % ask('raw0 54', '54'))


if __name__ == '__main__':
	main()
