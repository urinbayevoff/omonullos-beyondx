# Incoming SMS: follow rild-lite's output (journal of zril), acknowledge each
# RIL_UNSOL_RESPONSE_NEW_SMS (1003) with SMS_ACKNOWLEDGE (37: 1, 0) so the
# network does not resend it, and append "time sender: text" to INBOX. Each
# one also goes to SMS_LOG for the Messaging app (as ztoscorenetworkingd
# writes it on the S9: "in <unix time> <number> <text, \n escaped>") and is
# shown as a notification.
# Network time (RIL_UNSOL_NITZ_TIME_RECEIVED, 1008: "yy/mm/dd,hh:mm:ss+tz",
# UTC) sets the system clock and the PMIC RTC (zacpm rtc set) when they are
# more than 2 s off.
#   python3 zsms.py [INBOX]   (default /var/lib/zmodem/sms/inbox.txt)
import glob, os, re, socket, subprocess, sys, time

INBOX = sys.argv[1] if len(sys.argv) > 1 else '/var/lib/zmodem/sms/inbox.txt'
SMS_LOG = os.path.realpath('/var/lib/ztoscorenetworking') + '/sms/log'	# -> /var/lib/zrn-modem here
GSM7 = ('@£$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ\x1bÆæßÉ !"#¤%&\'()*+,-./0123456789:;<=>?'
	'¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà')

def ril(cmd):
	s = socket.socket(socket.AF_UNIX); s.connect('/dev/socket/zril')
	s.sendall((cmd + '\n').encode()); time.sleep(0.5); s.close()

def semi(b):	# swapped BCD digits
	return ''.join('%d%d' % (x & 15, x >> 4) for x in b).replace('15', '').rstrip('f')

def digits(b, n):
	d = ''.join('%x%x' % (x & 15, x >> 4) for x in b)
	return d[:n]

def unpack7(b, n, skip=0):
	bits = int.from_bytes(b, 'little'); out = ''
	for i in range(skip, n):
		c = (bits >> (7 * i)) & 0x7f
		out += GSM7[c] if c < len(GSM7) else '?'
	return out

def decode(pdu):	# SMS-DELIVER after the SMSC address
	b = bytes.fromhex(pdu); i = b[0] + 1
	first = b[i]; i += 1
	alen = b[i]; atype = b[i + 1]; i += 2
	ab = b[i:i + (alen + 1) // 2]; i += (alen + 1) // 2
	if atype & 0x70 == 0x50:
		sender = unpack7(ab, alen * 4 // 7)
	else:
		sender = ('+' if atype & 0x70 == 0x10 else '') + digits(ab, alen)
	dcs = b[i + 1]; i += 2 + 7
	udl = b[i]; ud = b[i + 1:]
	hl = ud[0] + 1 if first & 0x40 else 0
	if dcs & 0x0c == 0x08:
		text = ud[hl:udl].decode('utf-16-be', 'replace')
	elif dcs & 0x0c == 0x04:
		text = ud[hl:udl].decode('latin-1')
	else:
		text = unpack7(ud, udl, (hl * 8 + 6) // 7)
	return sender, text

def to_apps(sender, text):
	flat = text.replace('\r', '').replace('\t', ' ').replace('\n', '\\n')
	os.makedirs(os.path.dirname(SMS_LOG), exist_ok=True)
	with open(SMS_LOG, 'a') as f:
		f.write('in\t%d\t%s\t%s\n' % (time.time(), sender, flat))
	note = 'notify Messaging|%s|%.240s\n' % (sender, flat.replace('|', '/'))
	for ctl in glob.glob('/run/user/*/ztmetro.ctl'):	# a FIFO: never block on it
		try:
			fd = os.open(ctl, os.O_WRONLY | os.O_NONBLOCK)
			os.write(fd, note.encode()); os.close(fd)
		except OSError:
			pass

os.makedirs(os.path.dirname(INBOX), exist_ok=True)
j = subprocess.Popen(['journalctl', '-u', 'zril', '-f', '-n', '0', '-o', 'cat'],
		     stdout=subprocess.PIPE, text=True)
def nitz(m):
	import calendar
	y, mo, d, h, mi, sec = (int(x) for x in m.groups())
	t = calendar.timegm((2000 + y, mo, d, h, mi, sec, 0, 0, 0))
	if abs(t - time.time()) > 2:
		subprocess.run(['date', '-u', '-s', '@%d' % t], stdout=subprocess.DEVNULL)
		subprocess.run(['/usr/local/bin/zacpm', 'rtc', 'set'], stdout=subprocess.DEVNULL)

for line in j.stdout:
	m = re.search(r'! 1008 n=\d+ "(\d\d)/(\d\d)/(\d\d),(\d\d):(\d\d):(\d\d)', line)
	if m:
		nitz(m)
		continue
	m = re.search(r'! 1003 n=\d+ "([0-9A-Fa-f]+)"', line)
	if not m:
		continue
	ril('rawi 37 1 0')
	try:
		sender, text = decode(m.group(1))
	except Exception as e:
		sender, text = '?', 'decode: %r pdu %s' % (e, m.group(1))
	with open(INBOX, 'a') as f:
		f.write('%s %s: %s\n' % (time.strftime('%F %T'), sender, text))
	to_apps(sender, text)
	os.sync()
