#!/usr/bin/env python3
"""zsend: send one SMS through rild-lite (RIL_REQUEST_SEND_SMS 25).

  zsend.py NUMBER TEXT      e.g. zsend.py +998XXXXXXXXX "salom"

ASCII text goes as GSM 7-bit, anything else (ğ, ş, Cyrillic) as UCS-2.
SMSC = NULL, so the modem takes it from the SIM. Added for OmonullOS, October 2026.
"""
import socket, sys, time

def bcd(d):
	if len(d) % 2: d += 'F'
	return bytes(int(d[i + 1] + d[i], 16) for i in range(0, len(d), 2))

def gsm7(text):
	bits, n, out = 0, 0, bytearray()
	for ch in text:
		bits |= (ord(ch) & 0x7f) << n; n += 7
		while n >= 8: out.append(bits & 0xff); bits >>= 8; n -= 8
	if n: out.append(bits & 0xff)
	return bytes(out)

def tpdu(number, text):
	intl = number.startswith('+')
	d = number.lstrip('+')
	head = bytes([0x01, 0x00, len(d), 0x91 if intl else 0x81]) + bcd(d)	# SMS-SUBMIT, MR 0
	if all(32 <= ord(c) < 127 and c not in '@$`[]{}\\^~|_' for c in text):
		if len(text) > 160: raise ValueError('too long for one SMS (160)')
		return head + bytes([0x00, 0x00, len(text)]) + gsm7(text)
	ucs = text.encode('utf-16-be')
	if len(ucs) > 140: raise ValueError('too long for one SMS (70 non-ASCII)')
	return head + bytes([0x00, 0x08, len(ucs)]) + ucs

def main():
	if len(sys.argv) != 3:
		sys.exit(__doc__)
	pdu = tpdu(sys.argv[1], sys.argv[2]).hex().upper()
	s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	s.connect('/dev/socket/zril')
	s.sendall(('raws 25 - %s\n' % pdu).encode())
	s.settimeout(1)
	buf, end = '', time.time() + 60
	while time.time() < end:
		try:
			data = s.recv(65536)
		except socket.timeout:
			continue
		if not data:
			break
		buf += data.decode(errors='replace')
		for line in buf.splitlines():
			if line.startswith('< 25 '):
				print(line)
				sys.exit(0 if ' e=0 ' in line + ' ' else 1)
	sys.exit('no answer from RIL in 60 s')

if __name__ == '__main__':
	main()
