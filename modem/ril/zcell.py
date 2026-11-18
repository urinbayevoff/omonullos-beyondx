# The status bar's signal and operator: ask rild-lite (/dev/socket/zril) for
# SIGNAL_STRENGTH (19), OPERATOR (22), VOICE/DATA_REGISTRATION_STATE (20/21)
# every 30 s and when the network state changes (unsol 1000, 1002), and write
# /run/ztoscorenetworking/status the way ztoscorenetworkingd does on the S9
# (ztMETRO reads it when "modem" comes on its control pipe). This Samsung RIL
# does not send unsol 1009, so the signal is polled.
# data_on is the user's mobile data tile, taken as "data on|off" on
# /run/ztoscorenetworking.ctl and kept in /var/lib/zmodem/data_on; while it is
# on the packet context is kept up here (data_tick), data=1 once it is.
# Calls and messages for the Phone and Messaging apps (comm.h), as
# ztoscorenetworkingd does on the S9: "call NUMBER", "answer", "hangup" and
# "sms NUMBER TEXT" (or "smse", escaped) on the control pipe go to the RIL (DIAL 10, ANSWER 40,
# HANGUP 12, SEND_SMS 25); while there is a call its list (9) is asked for
# every 2 s and on unsol 1001, and the status gets call= / call_number=.
# Finished calls go to CALL_LOG, what came of each "sms" to SMS_LOG.
# While a call comes in the ringtone plays on the default sink (pw-play as
# the session's user), at ztMETRO's ring volume; none at zero.
# Once a call is active its audio is asked of the CP as stock's audio HAL
# does (libaudio-ril -> libsecril-client, OEM_HOOK_RAW 59, sound 0x08): voice
# path handset, call clock sync on, call volume; and silence is played so
# the ABOX graph the VSS joins runs. The DSP is told, once that runs,
# as stock's abox.c does: ABOX_START_VSS once a boot (its modem notifier sends it when the CP comes
# online), then audio mode IN_CALL and sound type VOICE ("ABOX Audio Mode",
# "ABOX Sound Type"), NORMAL and SPEAKER again after. When it ends: clock
# off, and the RIL's answers with the DSP's log go to the field logs.
#   python3 zcell.py   (zmodem start runs it while the RIL runs)
# Added for OmonullOS, October 2026.
# November 2026: the ear in a call (ear_tick): the touch IC's ear detection
# (zrn-s6sy761's ear_detect, input "s6sy761 ear detect", SW_FRONT_PROXIMITY)
# darkens the screen through ztMETRO's "prox near|far", as stock does; the
# IC goes to its low power mode while dark (lp_mode), or it stops sensing.
# November 2026: mobile data (data_tick, docs/s10-5g/MOBILE-DATA.md).
import glob, os, re, signal, socket, struct, subprocess, sys, time, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zsend import tpdu

STATUS = '/run/ztoscorenetworking/status'
CTL = '/run/ztoscorenetworking.ctl'
WISH = '/var/lib/zmodem/data_on'	# the S9's /var/lib/ztoscorenetworking is a dangling link here
INVALID = 0x7fffffff
VARDIR = os.path.realpath('/var/lib/ztoscorenetworking')	# -> /var/lib/zrn-modem, made here
SMS_LOG, CALL_LOG = VARDIR + '/sms/log', VARDIR + '/calls'

def bars_of(v):
	"""0..5 from the RIL_SignalStrength ints (Samsung layout: GW 0-2, CDMA 3-4,
	EVDO 5-7, LTE 8-13, TD-SCDMA 14-15, WCDMA 16-19, NR 20-), with the
	thresholds ztoscorenetworkingd uses for AT+CESQ, in dBm."""
	def steps(dbm, top):	# top = the 5-bar level, 12 dB per bar
		for n in range(5, 0, -1):
			if dbm >= top - 12 * (5 - n):
				return n
		return 0
	if len(v) > 9 and 44 <= v[9] <= 140:		# LTE RSRP, positive
		return steps(-v[9], -77)
	if len(v) > 18 and -120 <= v[18] <= -24:	# WCDMA RSCP
		return steps(v[18], -61)
	if len(v) > 16 and 0 <= v[16] <= 31:		# WCDMA ASU
		return steps(-113 + 2 * v[16], -61)
	if 0 <= v[0] <= 31:				# GSM ASU, as RxLev
		return steps(-113 + 2 * v[0], -66)
	return -1

def tech_of(rat):	# RIL_RadioTechnology -> the data icon
	return {20: '5G', 14: '4G', 19: '4G', 15: 'H+', 9: 'H', 10: 'H', 11: 'H',
		3: '3G', 2: 'E', 1: 'G', 16: 'G'}.get(rat, '')

REG = {1: 'home', 5: 'roaming', 2: 'searching', 12: 'searching', 3: 'denied', 13: 'denied'}
now = {'operator': '', 'plmn': '', 'bars': -1, 'voice': 0, 'data': 0, 'rat': 0}
call = {'state': '', 'number': '', 'idx': 0, 'since': 0, 'answered': 0, 'incoming': False}
sms_out = []	# numbers of the SEND_SMS requests not answered yet

def tell_shell(line):
	for ctl in glob.glob('/run/user/*/ztmetro.ctl'):	# a FIFO: never block on it
		try:
			fd = os.open(ctl, os.O_WRONLY | os.O_NONBLOCK)
			os.write(fd, line.encode()); os.close(fd)
		except OSError:
			pass

RINGTONES = ('/usr/local/share/sounds/ztos/ringtones/*.wav', '/usr/local/share/sounds/ztos/ring/*.wav')
# the tune chosen in Settings > ringtones + sounds: a bare name, looked for in
# ztOS's tunes (downloaded with ztos-media), then omonullOS's own; the same
# rule as zt_ringtone_file() in ztmetro/core/zrnvibe.h
RING_DIRS = ('/usr/local/share/sounds/ztos/phone', '/usr/local/share/sounds/ztos/ring')
RING_DEFAULT = 'omonullos-ring.wav'
ringer = None

def ring_volume():	# ztMETRO's "ring_volume = 0..100", as a linear stream volume
	for conf in glob.glob('/home/*/.config/ztmetro/config'):
		try:
			for line in open(conf):
				key, _, value = line.partition('=')
				if key.strip() == 'ring_volume':
					return max(0, min(100, int(value))) / 100
		except (OSError, ValueError):
			pass
	return 0.7	# its default, 21 of 30

def player(files, volume, gap=1):
	"""pw-play FILES[0] as the PipeWire session's user, again and again, GAP s apart"""
	runtime = sorted(glob.glob('/run/user/[1-9]*/pipewire-0'))
	if not files or not volume or not runtime:
		return None
	user = os.stat(runtime[0]).st_uid
	return subprocess.Popen(['setpriv', '--reuid=%d' % user, '--regid=%d' % user, '--init-groups',
		'env', 'XDG_RUNTIME_DIR=' + os.path.dirname(runtime[0]), 'sh', '-c',
		'while pw-play --volume "$0" "$1"; do sleep %d; done' % gap, '%.2f' % volume, files[0]],
		stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)

def stop(proc):
	if proc:
		try:
			os.killpg(proc.pid, signal.SIGTERM)
		except OSError:
			pass
		proc.wait()

def ringtone():	# the chosen tune's file, else ours, else any we have
	names = []
	for conf in glob.glob('/home/*/.config/ztmetro/ringtone'):
		try:
			name = open(conf).readline().strip()
		except OSError:
			continue
		if name and '/' not in name and not name.startswith('.'):
			names.append(name)
	for name in names + [RING_DEFAULT]:
		for d in RING_DIRS:
			f = os.path.join(d, name)
			if os.access(f, os.R_OK):
				return [f]
	return sorted(f for g in RINGTONES for f in glob.glob(g))

def ring(on):
	global ringer
	stop(ringer)
	ringer = player(ringtone(), ring_volume()) if on else None
	if on:
		keep_graph()	# so that it is long steady by the answer (call_audio)

def silence():
	if not os.path.exists(SILENCE):
		import wave
		with wave.open(SILENCE, 'wb') as w:
			w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
			w.writeframes(bytes(4 * 48000 * 60))	# long: each restart pauses the DSP's PCM
		os.chmod(SILENCE, 0o644)
	return SILENCE

def keep_graph():
	"""silence played on, so the ABOX graph (WDMA3/RDMA7) runs without a
	pause: from the ring of an incoming call (the ringtone's own pw-play
	stops between plays and at the answer), else from the call's audio"""
	global keeper, keeper_since
	if keeper is None or keeper.poll() is not None:
		keeper, keeper_since = player([silence()], 1.0, gap=0), time.time()

def drop_graph():
	global keeper
	stop(keeper)
	keeper = None

SILENCE = '/run/zcell-silence-60s.wav'
OEM_SOUND = {	# libsecril-client: 08 <cmd> 00 <length> <args>
	'path': '0805000601%02x' % 0,	# SetCallAudioPath(handset 1, extra volume 0)
	'clock on': '080a000501', 'clock off': '080a000500',	# SetCallClockSync("on" 1 / "off" 0)
	'volume': '080300060104',	# SetCallVolume(handset 1, 4)
}
keeper, audio_log, audio_on, audio_since, snaps, seen = None, [], False, 0, [], set()
keeper_since = 0	# when the silence started (keep_graph)
LOG_EVERY, log_due = 0.2, 0	# log_due: the next read of the DSP's log while a call lasts
ABOX_IPC = '/sys/module/snd_zrn_abox_bx/parameters/ipc'	# "IPC_SYSTEM 0 msgtype param1"
VSS_STARTED = '/run/zcell-vss-started'
# ABOX_START_VSS a while after boot, as stock's modem notifier sends it when
# the CP comes online: sent with a call's first IN_CALL it faulted the VSS
# (VHMPcmInThread, 0xbcb8) in four first calls of six, never in later ones
VSS_AT = 90	# s of uptime: the DSP and the CP are up
CALL_BUS = '/run/zcall-bus'	# zgpu keeps DRAM and the bus up while it is there

def uptime():
	return float(open('/proc/uptime').read().split()[0])

def start_vss():
	del audio_log[:]
	abox('1 0 0xa0', 'START_VSS')
	time.sleep(3)
	abox('1 0 0x50 0', 'mode NORMAL')	# the DSP still answers?
	with open(VSS_STARTED, 'w') as f:
		f.write('\n'.join(audio_log) + '\n')
	print(' | '.join(audio_log), flush=True)
	del audio_log[:]

def call_bus(on):
	"""MIF/INT at the panel-on rates for the call: with the panel off zgpu
	has them at 421/100 MHz and the DSP faults as the call starts (the
	08:41 and 05:28 calls on 2026-10-20 rang in with the screen off)"""
	if not on:
		try:
			os.unlink(CALL_BUS)
		except OSError:
			pass
		return
	open(CALL_BUS, 'w').close()
	for dom, mhz in (('mif', '1014'), ('int', '534')):
		try:
			subprocess.run(['zacpm', 'dvfs', 'set', dom, mhz], stdout=subprocess.DEVNULL,
				stderr=subprocess.DEVNULL, timeout=10)
		except (OSError, subprocess.SubprocessError) as e:
			audio_log.append('%.3f bus %s %s failed: %s' % (time.time(), dom, mhz, e))
NOISE = re.compile(r'GEAR CHANGED|IPC:DBG|source_update|sink_update|buffer_empty|in-full|mute count|PCMDAI ACK|vpcmout_dai_done|handler\(')

def abox(words, what):
	try:
		with open(ABOX_IPC, 'w') as f:
			f.write(words)
		audio_log.append('%.3f abox %s: %s' % (time.time(), what, words))
	except OSError as e:
		audio_log.append('%.3f abox %s: %s failed: %s' % (time.time(), what, words, e))

def dsplog_back(limit):
	"""the DSP's log, oldest first, as far back as LIMIT bytes (or the ring)
	reaches: the dsplog parameter gives a page, dsplog_skip how far back"""
	P = '/sys/module/snd_zrn_abox_bx/parameters/'
	pages = []
	try:
		for skip in range(0, limit, 4000):
			with open(P + 'dsplog_skip', 'w') as f:
				f.write(str(skip))
			page = open(P + 'dsplog', errors='replace').read()[:-1]
			if page in pages or page.startswith(('no log', 'bad log')):
				break	# round the ring
			pages.append(page)
	except OSError as e:
		pages.append(str(e))
	finally:
		try:
			with open(P + 'dsplog_skip', 'w') as f:
				f.write('0')
		except OSError:
			pass
	# each page is 4094 bytes ending 4000 * k before the newest: drop the overlap
	return ''.join(p[:4000] if i else p for i, p in reversed(list(enumerate(pages))))[-limit:]

def snapshot(raw=False, limit=60000, mark=True):
	"""the DSP's log of the last seconds, into the call's log: its ring holds
	minutes when nothing plays, but only ~0.5 s while a stream runs (~120 KB/s
	of stream lines), so during a call it is read every LOG_EVERY s;
	RAW keeps the stream lines too"""
	if mark:
		audio_log.append('%.3f == dsplog snapshot%s' % (time.time(), ' (raw)' if raw else ''))
	stamped = []	# the DSP writes while the pages are read: put them in time order
	for l in dsplog_back(limit).splitlines():
		m = re.match(r'\[ *(\d+\.\d+)\]', l)
		if m and (raw or not NOISE.search(l)) and l not in seen:
			seen.add(l)
			stamped.append((float(m.group(1)), l))
	audio_log.extend(l for _, l in sorted(stamped))

def call_mode(on):
	"""a call's log and bus hold from its start (dialing or ringing);
	NORMAL and SPEAKER again after (IN_CALL and VOICE: call_audio)"""
	global snaps, log_due
	log_due = time.time() + 1 if on else 0
	if on:
		del audio_log[:]
		seen.clear()
		call_bus(True)
		return
	call_audio(False)
	abox('1 0 0x50 0', 'mode NORMAL')
	abox('1 0 0x60 1', 'type SPEAKER')
	call_bus(False)
	snaps = []
	snapshot()
	logs = '/var/lib/zfield/repo/logs'
	if os.path.isdir(logs):
		with open(time.strftime(logs + '/call-%m%d-%H%M.log'), 'w') as f:
			f.write('\n'.join(audio_log) + '\n')

UPLINK = '/root/tools/zcall-uplink.sh'	# a stand-in uplink for the VSS while the codec is not up

def uplink(what):
	audio_log.append('%.3f uplink %s: start' % (time.time(), what))
	try:
		out = subprocess.run(['sh', UPLINK, what], capture_output=True, text=True, timeout=10)
		audio_log.append('%.3f uplink %s: %s' % (time.time(), what, (out.stdout + out.stderr).strip().replace('\n', ' | ')))
	except (OSError, subprocess.SubprocessError) as e:
		audio_log.append('%.3f uplink %s failed: %s' % (time.time(), what, e))

ABOX_CALL = '/sys/module/snd_zrn_abox_bx/parameters/call'	# the driver's voice call streams (RDMA4, WDMA2)
# the driver's view of the call's path (ztOS a9bcb04's call_diag, Nov 2026):
# routes, the call DMAs' status and their loudest samples lately (rx the
# downlink, tx the uplink, ref the echo reference), every DIAG_EVERY s
CALL_DIAG = '/sys/module/snd_zrn_abox_bx/parameters/call_diag'
DIAG_EVERY, diag_due = 2.0, 0
# the Phone app's speaker button ("speaker on|off"): the driver's amps while a
# call runs, bit 0 the bottom speaker, bit 1 the earpiece (stock handset: 2);
# applied at once. Back to the earpiece when the call ends.
CALL_AMP = '/sys/module/snd_zrn_abox_bx/parameters/call_amp_mask'

# The amps share one volume, "Speaker Playback Volume" (0..768, 0.125 dB
# steps, 768 = -6 dB), set low for music (/etc/zaudio.env VOL 624 = -24 dB):
# fine at the ear, too quiet on the speaker (owner, 11.11). While the speaker
# plays a call it is raised to SPK_CALL_VOL, and put back when it stops.
SPK_CALL_VOL = 720	# -12 dB
spk_saved = None
# The CP's own side of the speaker (ztOS's S9 modemd, Samsung's secril):
# SetCallAudioPath 6 (speaker; handset 1) and SetCallVolume's sound type 0x11
# (handset 0x01) pick the VSS's handsfree tuning, its strong echo canceller:
# with the handset's on the speaker the far end heard itself (11.11 18:43).
call_vol, call_spk = 4, False

def cp_route():
	path = '0805000601%02x' % 0 if not call_spk else '0805000606%02x' % 0
	vol = '08030006%s%02x' % ('11' if call_spk else '01', call_vol)
	for what, v in (('path', path), ('volume', vol)):
		audio_log.append('%.3f > %s %s' % (time.time(), what, v))
		s.sendall(('rawx 59 %s\n' % v).encode())

def spk_volume(v=None):
	try:
		if v is None:
			out = subprocess.run(['amixer', '-c0', 'cget', 'numid=1'], capture_output=True,
				text=True, timeout=5).stdout
			return int(re.search(r': values=(\d+)', out).group(1))
		subprocess.run(['amixer', '-q', '-c0', 'cset', 'numid=1', str(v)], timeout=5)
	except (OSError, subprocess.SubprocessError, AttributeError, ValueError):
		return None

def call_speaker(on):
	global spk_saved, call_spk
	if audio_on and on != call_spk:
		call_spk = on
		cp_route()
	call_spk = on
	try:
		with open(CALL_AMP, 'w') as f:
			f.write('1' if on else '2')
		audio_log.append('%.3f speaker %s' % (time.time(), 'on' if on else 'off'))
	except OSError as e:
		audio_log.append('%.3f speaker %s failed: %s' % (time.time(), on, e))
	if on and spk_saved is None:
		v = spk_volume()
		if v is not None and v < SPK_CALL_VOL:
			spk_saved = v
			spk_volume(SPK_CALL_VOL)
	elif not on and spk_saved is not None:
		spk_volume(spk_saved)
		spk_saved = None

def call_path():
	try:
		with open(CALL_DIAG) as f:
			audio_log.append('%.3f call path: %s' % (time.time(), f.read().strip()))
	except OSError:
		pass	# a driver from before call_diag

def abox_call(on):
	try:
		with open(ABOX_CALL, 'w') as f:
			f.write(str(on))
		audio_log.append('%.3f abox call streams %d' % (time.time(), on))
	except OSError as e:
		audio_log.append('%.3f abox call streams %d failed: %s' % (time.time(), on, e))

def graph_steady(limit=4.0, steady=1.0, running_since=None):
	"""wait for the realtime WDMA3 (CTRL0 bit 0) to run STEADY s; a stream
	that has been playing since RUNNING_SINCE counts from then (a ring's)"""
	t0, since = time.time(), None
	first = True
	while time.time() - t0 < limit:
		try:
			on = int(subprocess.run(['zpoke', '18c53300'], capture_output=True,
				text=True, timeout=2).stdout.split()[-1], 16) & 1
		except (OSError, subprocess.SubprocessError, ValueError, IndexError):
			on = 0
		if on and first and running_since:
			since = running_since	# it ran before we looked
		first = False
		since = (since or time.time()) if on else None
		if since and time.time() - since >= steady:
			break
		time.sleep(0.1)
	audio_log.append('%.3f graph %s after %.1f s' % (time.time(),
		'steady' if since else 'NOT running', time.time() - t0))

def call_audio(on):
	"""once the call is active: its voice path, clock and volume of the CP,
	and silence played so the ABOX graph runs; clock off when it ends"""
	global keeper, audio_on, audio_since, snaps, diag_due, call_vol
	if on:
		audio_on, audio_since = True, time.time()
		for what in ('path', 'clock on', 'volume'):
			audio_log.append('%.3f > %s %s' % (time.time(), what, OEM_SOUND[what]))
			s.sendall(('rawx 59 %s\n' % OEM_SOUND[what]).encode())
		# IN_CALL and VOICE once the silence's streams run steady: sent while
		# WDMA3/RDMA7 were paused (the ringtone just gone) or 0.3 s after they
		# started, the first call of a boot faulted the DSP (a jump to its
		# reset code, 0xbcb8, at SET CALLPATH); with the ringtone playing on, not.
		# An incoming call's silence started with its ring (keep_graph): by the
		# answer it has run for seconds and the wait is one look (15.11; the
		# answer to the voice was ~2.3 s, 1.1 of it this wait)
		ringing = keeper is not None and keeper.poll() is None
		keep_graph()
		graph_steady(running_since=keeper_since if ringing else None)
		abox('1 0 0x50 2', 'mode IN_CALL')
		abox('1 0 0x60 0', 'type VOICE')
		snaps = [(time.time() + 0.5, True)]
		# was 0.5 s, for the silence's streams to open first: graph_steady
		# waits for that now; the whole wait was ~2.8 s from the answer (12.11)
		time.sleep(0.1)
		uplink('on')
		abox_call(1)
		time.sleep(1)	# the VSS has its streams running: what the DMAs hold
		uplink('regs')
		call_path()
		diag_due = time.time() + DIAG_EVERY
		return
	diag_due = 0
	if audio_on:
		s.sendall(('rawx 59 %s\n' % OEM_SOUND['clock off']).encode())
		uplink('regs')
		call_path()
		abox_call(0)
		uplink('off')
	audio_on = False
	call_speaker(False)
	call_vol = 4	# OEM_SOUND's, where the next call starts
	s.sendall(b'rawi 53 0\n')	# unmuted for the next call, whoever muted this one
	drop_graph()

def log_line(path, line):
	os.makedirs(os.path.dirname(path), exist_ok=True)
	with open(path, 'a') as f:
		f.write(line)

def call_set(state, number=''):
	now = time.time()
	t = int(now)
	if state and not call['state']:
		call.update(since=t, answered=0, incoming=state == 'incoming')
	if state == 'active' and not call['answered']:
		call['answered'] = t
	if not state and call['state']:	# over: into the log
		kind = 'out' if not call['incoming'] else 'in' if call['answered'] else 'missed'
		log_line(CALL_LOG, '%s\t%d\t%s\t%d\n' % (kind, call['since'], call['number'] or 'unknown',
			t - call['answered'] if call['answered'] else 0))
		if kind == 'out' and not call['answered']:	# why it never got through
			s.sendall(b'raw0 18\n')	# LAST_CALL_FAIL_CAUSE
		if kind == 'missed':
			tell_shell('notify Phone|Javobsiz qönğiroq|%s\n' % (call['number'] or 'Nomaʼlum raqam'))
		call.update(number='', idx=0)
	if state == 'incoming' and call['state'] != 'incoming':
		# the screen on first, as Android does for a call (a call can wake
		# the phone from a system sleep, wake bit 8, with the screen off);
		# ztMETRO's own idle timer puts it out again. SMS does not light it:
		# no proximity sensor yet, so not in a pocket for every message
		tell_shell('screen on\n')
		tell_shell('notify Phone|Kiruvçi qönğiroq|%s\n' % (number or 'Javob beriş uçun Telefonni oçing'))
	if (state == 'incoming') != (call['state'] == 'incoming'):
		ring(state == 'incoming')
		if not state and not audio_on:
			drop_graph()	# missed or turned down: its ring's silence goes too
	try:	# the call's audio never takes the call (its status) down with it
		# an incoming call tells the DSP at the answer, as Android's IN_CALL is:
		# IN_CALL 0.3 s after the ringtone's stream started faulted the DSP (09:03)
		if state and state != 'incoming' and not call.get('mode'):
			if call['state'] == 'incoming' and (keeper is None or keeper.poll() is not None):
				time.sleep(0.3)	# the ringtone's pw-play gone, the sink steady
				# (not needed while the ring's silence plays on: the sink never paused)
			call_mode(True)
			call['mode'] = True
		if state == 'active' and call['state'] != 'active':	# the voice's delay starts here
			audio_log.append('%.3f call active' % now)
		# an outgoing call's audio from dialing, as Android's IN_CALL is: the
		# network's ringback and its "busy"/"not reachable" announcements are
		# in-band (early media); an incoming one's at the answer, as before
		if state in ('dialing', 'ringing', 'active') and not audio_on:
			call_audio(True)
		elif not state and call.get('mode'):
			call_mode(False)
			call['mode'] = False
	except Exception:
		import traceback
		log_line('/var/lib/zfield/repo/logs/zcell-err.log', time.strftime('%F %T ') + traceback.format_exc())
	call['state'] = state
	if number:
		call['number'] = number
	publish()

# Mobile data. While the tile is on and the phone is registered at home, one
# packet context is kept up through the RIL (SETUP_DATA_CALL 27, strings as
# libsec-ril.so reads them, see zdata-try.sh) with a default route behind
# Wi-Fi's, so it takes over by itself when Wi-Fi goes. Never set up in roaming
# or during a call. The DNS servers are given to resolved only while no other
# default route is in front: the operator's own would time out over Wi-Fi.
# None of this may touch calls and messages: every entry goes through data_safe.
APN_FILE = '/etc/ztoscorenetworking/apn'	# Settings > cellular + SIM > access point
APN_AUTO = "# zcell: the operator's default, for "	# first line + PLMN: not the user's
APNS = {	# stock apns-conf.xml's internet entries: apn, user, password, auth
	'43404': ('internet.beeline.uz', 'beeline', 'beeline', 1),
	'43405': ('internet', '', '', 0),
	'43406': ('default', '', '', 0),
	'43407': ('net.ums.uz', '', '', 0),
	'43408': ('internet.uzmobile.uz', '', '', 0),
}
DATA_STATE = '/run/zcell-data'	# "CID", the context a restarted zcell takes back
DATA_METRIC = 700		# Wi-Fi has 600
DATA_WAIT, ADOPT_WAIT = 60, 15	# s for the RIL's answers
DATA_BACKOFF = (5, 15, 30, 60, 120, 300)
STALL_EVERY, STALL_LIMIT = 30, 3	# that many periods of sending with nothing back
DATA_ENTRY = re.compile(r'\{cid (\d+) active (\d+) status (-?\d+) retry (-?\d+) (\S*) (\S+) '
	r'addr (.*?) dns (.*?) gw (.*?) mtu (-?\d+)\}')
data = {'state': 'idle', 'cid': 0, 'ifn': '', 'addr': '', 'dns': [], 'dns_on': False,
	'since': 0, 'retry': 0, 'fails': 0, 'tick': 0, 'stall': 0, 'stall_due': 0, 'rx': 0, 'tx': 0}
ATTACH_EVERY, ATTACH_RETRY, ATTACH_TRIES = 5, 30, 3	# s, s, answers with an error
attach = {'plmn': '', 'sent': None, 'due': 0, 'tries': 0, 'operator': ''}
# The network type, Settings > cellular + SIM: "net lte|3g" on CTL, kept in
# NET_FILE. There is no VoLTE, so on LTE a call falls back to 3G (CSFB), which
# is known to work only on the networks in LTE_BY_DEFAULT: the others start on
# 3G until the user picks 4G.
NET_FILE = '/var/lib/zmodem/net_mode'
NET_TYPES = {'lte': 9, '3g': 0}	# RIL preferred network: LTE/WCDMA/GSM, WCDMA first
LTE_BY_DEFAULT = ('43408',)
NET_EVERY = 30	# s, until the modem has answered
nettype = {'chosen': '', 'have': None, 'asked': None, 'due': 0}
# The modem is told when the screen is off (RIL_REQUEST_SCREEN_STATE), as
# Android does: with mobile data as the only network and the modem not told, its
# network state messages woke the phone from every sleep within seconds.
SCREEN_REQ = 61
screen = {'told': None, 'due': 0}
ANDROID_ROOT = '/run/android'
ANDROID_LIBS = '/apex/com.android.i18n/lib64:/apex/com.android.runtime/lib64'

def data_safe(f, *args):
	try:
		f(*args)
	except Exception:
		print('data: ' + traceback.format_exc(), flush=True)
		data['tick'] = time.time() + 30

def net(*cmd):
	try:
		return subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
			timeout=5).returncode == 0
	except (OSError, subprocess.SubprocessError):
		return False

def data_entries(text):
	return [{'cid': int(e[0]), 'active': int(e[1]), 'status': int(e[2]), 'retry': int(e[3]),
		'ifn': e[5], 'addr': e[6].split(), 'dns': e[7].split(), 'mtu': int(e[9])}
		for e in DATA_ENTRY.findall(text)]

def apn_of_operator(plmn=None):
	return APNS.get(plmn or now['plmn'], ('internet', '', '', 0))

def apn_file_sync():
	"""Settings shows and edits APN_FILE: keep the operator's APN there, for
	the SIM in the phone, until the user saves one of their own (Settings
	writes its own first line). The user's, so that Settings may overwrite it."""
	if not now['plmn']:
		return
	mark = APN_AUTO + now['plmn'] + '\n'
	try:
		first = open(APN_FILE).readline()
		if first == mark or not first.startswith(APN_AUTO):
			return
	except OSError:
		pass
	apn, user, password, auth = apn_of_operator()
	os.makedirs(os.path.dirname(APN_FILE), exist_ok=True)
	with open(APN_FILE + '.new', 'w') as f:
		f.write('%sapn = %s\nuser = %s\npassword = %s\nauth = %s\n' % (mark, apn, user, password,
			('none', 'pap', 'chap')[auth]))
	homes = sorted(glob.glob('/run/user/[1-9]*'))
	os.chown(APN_FILE + '.new', os.stat(homes[0]).st_uid if homes else 1000, -1)
	os.chmod(APN_FILE + '.new', 0o644)
	os.rename(APN_FILE + '.new', APN_FILE)

def apn_conf(plmn=None):
	"""apn, user, password, auth: the one saved in Settings, else the operator's"""
	conf, auths = {}, {'none': 0, 'pap': 1, 'chap': 2}
	try:
		with open(APN_FILE) as f:
			if f.readline().startswith(APN_AUTO):
				return apn_of_operator(plmn)
			f.seek(0)
			for line in f:
				key, eq, value = line.partition('=')
				if eq and not key.lstrip().startswith('#'):
					conf[key.strip()] = value.strip()
	except OSError:
		pass
	got = (conf.get('apn', ''), conf.get('user', ''), conf.get('password', ''),
		auths.get(conf.get('auth', 'none'), 0))
	# rild-lite splits its commands at blanks; "-" would be NULL, "" empty
	if got[0] and all(re.fullmatch(r'[^\s"]*', v) and v != '-' for v in got[:3]):
		return got
	return apn_of_operator(plmn)

def reg_plmn(rest):
	"""MCC and MNC in the cell identity of a registration answer: there
	seconds after the modem starts, long before the operator's name"""
	m = re.search(r'([0-9a-f]+)$', rest)
	found = m and re.search(rb'(\d{3})\0(\d{2,3})\0', bytes.fromhex(m.group(1)[:len(m.group(1)) & ~1]))
	return (found.group(1) + found.group(2)).decode() if found else ''

def sim_operator(plmn):
	"""libsec-ril answers SET_INITIAL_ATTACH_APN with success but drops it
	("invalid sim operator") until this property is set; on Android the
	telephony framework sets it"""
	if attach['operator'] == plmn:
		return True
	try:
		done = subprocess.run(['chroot', ANDROID_ROOT, '/system/bin/setprop',
			'gsm.sim.operator.numeric', plmn], env=dict(os.environ, LD_LIBRARY_PATH=ANDROID_LIBS),
			stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5).returncode == 0
	except (OSError, subprocess.SubprocessError):
		done = False
	if done:
		attach['operator'] = plmn
	else:
		print('attach: could not set the SIM operator', flush=True)
	return done

def net_load():
	try:
		with open(NET_FILE) as f:
			v = f.read().strip()
	except OSError:
		v = ''
	nettype['chosen'] = v if v in NET_TYPES else ''

def net_mode():
	"""'lte' or '3g'; '' while the network is not known and nothing was chosen"""
	if nettype['chosen']:
		return nettype['chosen']
	plmn = now['plmn'] or attach['plmn']
	return '' if not plmn else 'lte' if plmn in LTE_BY_DEFAULT else '3g'

def net_choose(mode):
	try:
		with open(NET_FILE + '.new', 'w') as f:
			f.write(mode + '\n')
		os.replace(NET_FILE + '.new', NET_FILE)
	except OSError as e:
		print('net: not saved: %s' % e, flush=True)
	nettype.update(chosen=mode, due=0)
	print('net: %s chosen' % mode, flush=True)

def net_tick(t):
	"""read the modem's preferred network once, change it only if it differs"""
	mode = net_mode()
	if not mode or not (now['plmn'] or attach['plmn']) or call['state'] or t < nettype['due']:
		return
	if nettype['have'] is None:
		nettype['due'] = t + NET_EVERY
		s.sendall(b'raw0 74\n')
	elif nettype['have'] != NET_TYPES[mode]:
		nettype.update(asked=NET_TYPES[mode], due=t + NET_EVERY)
		s.sendall(b'rawi 73 %d\n' % nettype['asked'])

def net_answer(req, ok, rest):
	if not ok:
		print('net: request %d refused' % req, flush=True)
	elif req == 74:
		nettype.update(have=int(rest.split()[0]), due=0)
	elif nettype['asked'] is not None:
		nettype.update(have=nettype['asked'], asked=None, due=0)
		attach.update(sent=None, due=0, tries=0)	# LTE again: it needs its access point again
		print('net: modem set to %d' % nettype['have'], flush=True)

def attach_tick(t):
	"""LTE asks for its access point while attaching; without one the modem
	gives up after ~20 s and stays on 3G"""
	plmn = now['plmn'] or attach['plmn']
	if not plmn or call['state'] or t < attach['due'] or nettype['have'] != NET_TYPES['lte'] \
			or net_mode() != 'lte':
		return
	attach['due'] = t + ATTACH_EVERY
	conf = apn_conf(plmn)
	if conf == attach['sent']:
		return
	if not sim_operator(plmn):
		attach['due'] = t + ATTACH_RETRY
		return
	q = lambda v: v or '""'
	s.sendall(('attach %s IP %s %s %d\n' % (conf[0], q(conf[1]), q(conf[2]), conf[3])).encode())
	attach['sent'] = conf
	print('attach: apn %s' % conf[0], flush=True)

def attach_answer(ok, e):
	if ok:
		attach['tries'] = 0
		return
	attach['tries'] += 1
	print('attach: refused, e=%s' % e, flush=True)
	if attach['tries'] < ATTACH_TRIES:
		attach.update(sent=None, due=time.time() + ATTACH_RETRY)

def data_setup():
	apn, user, password, auth = apn_conf()
	q = lambda v: v or '""'
	# radioTech profile apn user password authType protocol roamingProtocol
	# supportedTypes bearerBitmask modemCognitive mtu mvnoType mvnoMatch roamingAllowed
	s.sendall(('raws 27 %d 0 %s %s %s %d IP IP 1 0 0 0 "" "" 1\n' % (now['rat'], apn,
		q(user), q(password), auth)).encode())
	data.update(state='asking', since=time.time())
	print('data: asking, apn %s, radio tech %d' % (apn, now['rat']), flush=True)

def data_dns(on):
	if on == data['dns_on']:
		return
	data['dns_on'] = on
	if on and data['dns']:
		net('resolvectl', 'dns', data['ifn'], *data['dns'])
		net('resolvectl', 'default-route', data['ifn'], 'yes')
	else:
		net('resolvectl', 'revert', data['ifn'])

def route_in_front():
	"""a default route that is used before ours (Wi-Fi's)?"""
	with open('/proc/net/route') as f:
		for line in f.readlines()[1:]:
			w = line.split()
			if w[0] != data['ifn'] and w[1] == '00000000' and w[7] == '00000000' and \
					int(w[3], 16) & 1 and int(w[6]) < DATA_METRIC:
				return True
	return False

def data_net_off():
	ifn, addr = data['ifn'], data['addr']
	if not ifn:
		return
	data_dns(False)
	net('ip', 'route', 'del', 'default', 'dev', ifn, 'metric', str(DATA_METRIC))
	if addr:
		net('ip', 'addr', 'del', addr, 'dev', ifn)
	net('ip', 'link', 'set', ifn, 'down')
	data.update(ifn='', addr='', dns=[])

def data_forget(state='idle'):
	data_net_off()
	data.update(state=state, cid=0, stall=0)
	try:
		os.unlink(DATA_STATE)
	except OSError:
		pass
	publish()

def data_failed(why, retry=0):
	wait = DATA_BACKOFF[min(data['fails'], len(DATA_BACKOFF) - 1)]
	data['fails'] += 1
	data['retry'] = time.time() + max(wait, retry)
	print('data: %s, again in %d s' % (why, data['retry'] - time.time()), flush=True)
	data_forget()

def data_up(e):
	ifn, addr = e['ifn'], e['addr'][0] if e['addr'] else ''
	if not re.fullmatch(r'rmnet[0-7]', ifn) or not re.fullmatch(r'[0-9.]+(/\d+)?', addr):
		s.sendall(('raws 41 %d 0\n' % e['cid']).encode())
		data_failed('odd answer: %s %s' % (ifn, addr))
		return
	if '/' not in addr:
		addr += '/32'
	if data['ifn'] and (data['ifn'], data['addr']) != (ifn, addr):
		data_net_off()
	net('ip', 'link', 'set', ifn, 'up')
	if 576 <= e['mtu'] <= 1500:
		net('ip', 'link', 'set', ifn, 'mtu', str(e['mtu']))
	ok = net('ip', 'addr', 'replace', addr, 'dev', ifn)
	ok = net('ip', 'route', 'replace', 'default', 'dev', ifn, 'metric', str(DATA_METRIC)) and ok
	data.update(ifn=ifn, addr=addr, cid=e['cid'], dns=[d for d in e['dns'] if ':' not in d])
	if not ok:
		s.sendall(('raws 41 %d 0\n' % e['cid']).encode())
		data_failed('could not set up ' + ifn)
		return
	data.update(state='up', fails=0, stall=0, stall_due=time.time() + STALL_EVERY, rx=-1, tx=-1)
	with open(DATA_STATE, 'w') as f:
		f.write('%d\n' % e['cid'])
	print('data: up on %s (cid %d)' % (ifn, e['cid']), flush=True)
	data_dns(not route_in_front())
	publish()

def data_down(retry=2):
	s.sendall(('raws 41 %d 0\n' % data['cid']).encode())
	print('data: down', flush=True)
	data['retry'] = time.time() + retry
	data_forget()

def data_start():
	"""after a restart of zcell: the context it had may still be up"""
	try:
		data['cid'] = int(open(DATA_STATE).read())
	except (OSError, ValueError):
		return
	s.sendall(b'raw0 57\n')
	data.update(state='adopting', since=time.time())

def data_answer(req, ok, rest):
	entries = data_entries(rest) if ok else []
	if req == 27 and data['state'] == 'asking':
		e = entries[0] if entries else None
		if not ok or not e:
			data_failed('refused: ' + rest[:80])
		elif e['status'] or not e['addr']:
			data_failed('status %d' % e['status'], max(e['retry'], 0) // 1000)
		else:
			data_up(e)
	elif req == 57 and data['state'] == 'adopting':
		e = next((e for e in entries if e['cid'] == data['cid'] and e['active'] and e['addr']), None)
		if e:
			data_up(e)
		else:
			data_forget()
	elif req == 57 and data['state'] == 'up' and entries:
		data_list(entries)

def data_list(entries):
	e = next((e for e in entries if e['cid'] == data['cid']), None)
	if not e or not e['active']:
		data_failed('the network dropped it')
	elif e['addr'] and e['addr'][0].split('/')[0] != data['addr'].split('/')[0]:
		data_up(e)

def data_changed(line):	# unsol 1010, DATA_CALL_LIST_CHANGED
	if data['state'] != 'up':
		return
	if line.startswith('! 1010 n=0'):
		data_failed('the network dropped it')
		return
	entries = data_entries(line)
	if entries:
		data_list(entries)

def data_stalled(t):
	"""packets go out and for a long time none come back: connect again"""
	data['stall_due'] = t + STALL_EVERY
	base = '/sys/class/net/%s/statistics/' % data['ifn']
	try:
		rx, tx = int(open(base + 'rx_packets').read()), int(open(base + 'tx_packets').read())
	except (OSError, ValueError):
		data_failed('the interface went away')
		return
	stuck = data['tx'] >= 0 and tx - data['tx'] >= 5 and rx == data['rx']
	data.update(rx=rx, tx=tx, stall=data['stall'] + 1 if stuck else 0)
	if data['stall'] >= STALL_LIMIT:
		print('data: nothing comes back', flush=True)
		data['fails'] += 1
		data_down(5)

def screen_is_on():
	"""None when there is no panel to ask"""
	for p in glob.glob('/sys/class/drm/card*-DSI-1/dpms'):
		try:
			with open(p) as f:
				return f.read().strip() != 'Off'
		except OSError:
			pass
	return None

def screen_tick(t):
	on = screen_is_on()
	if on is None or t < screen['due']:
		return
	on = on or bool(call['state'])	# the ear darkens the screen in a call: nothing changes for the modem
	if on != screen['told']:
		screen.update(told=on, due=0)
		s.sendall(b'rawi %d %d\n' % (SCREEN_REQ, on))

def screen_answer(ok):
	if not ok:
		print('screen: the modem refused the screen state', flush=True)
		screen.update(told=None, due=time.time() + NET_EVERY)

def data_tick():
	t = time.time()
	if t < data['tick']:
		return
	data['tick'] = t + 1
	screen_tick(t)
	net_tick(t)
	attach_tick(t)
	st, wanted = data['state'], os.path.exists(WISH)
	if st == 'idle':
		if wanted and now['data'] == 1 and now['rat'] and not call['state'] and t >= data['retry'] \
				and (now['plmn'] or os.path.exists(APN_FILE)):
			data_setup()
	elif st == 'asking':
		if t - data['since'] > DATA_WAIT:
			data_failed('no answer')
	elif st == 'adopting':
		if t - data['since'] > ADOPT_WAIT:
			data_forget()
	elif not wanted or now['data'] == 5:	# up: off, or roaming now
		data_down()
	else:
		data_dns(not route_in_front())
		if t >= data['stall_due']:
			data_stalled(t)

def data_apn():	# a new access point was saved
	data.update(fails=0, retry=0, tick=0)
	attach.update(due=0, tries=0)
	if data['state'] == 'up':
		data_down(1)

def command(cmd):
	global poll, call_vol
	if cmd in ('data on', 'data off'):
		wish(cmd == 'data on')
		data.update(fails=0, retry=0, tick=0)
	elif cmd == 'apn':
		data_safe(data_apn)
	elif cmd in ('net lte', 'net 3g'):
		net_choose(cmd[4:])
		data['tick'] = 0
		publish()
	elif cmd.startswith('call '):
		number = cmd[5:].strip()
		if re.fullmatch(r'[0-9+*#]{1,30}', number) and not call['state']:
			call_set('dialing', number)
			s.sendall(('dial %s\n' % number).encode())
			poll = time.time() + 1.5
	elif cmd == 'answer':
		s.sendall(b'raw0 40\n'); poll = time.time() + 0.5
	elif cmd in ('speaker on', 'speaker off'):
		call_speaker(cmd == 'speaker on')
	elif re.fullmatch(r'volume [0-5]', cmd):
		# the volume keys in a call (ztMETRO): SetCallVolume(handset 1, level),
		# Samsung's audio HAL's 0..5; every call starts at OEM_SOUND's 4
		call_vol = int(cmd[7:])
		if audio_on:
			v = '08030006%s%02x' % ('11' if call_spk else '01', call_vol)
			audio_log.append('%.3f > volume %s' % (time.time(), v))
			s.sendall(('rawx 59 %s\n' % v).encode())
	elif cmd in ('mute on', 'mute off'):
		# RIL_REQUEST_SET_MUTE (53): the modem sends silence, the mics stay on
		s.sendall(('rawi 53 %d\n' % (cmd == 'mute on')).encode())
	elif cmd == 'hangup':
		if call['idx']:
			s.sendall(('rawi 12 %d\n' % call['idx']).encode())
		poll = time.time() + 0.5
	elif cmd.startswith('sms ') or cmd.startswith('smse '):
		# "smse" (ztOS BETA-1's Messaging): \n, \t and \\ are escaped
		escaped = cmd.startswith('smse ')
		number, _, text = cmd[5 if escaped else 4:].partition(' ')
		if escaped:
			text = re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), text)
		else:
			text = text.replace('\\n', '\n')
		try:
			pdu = tpdu(number, text).hex().upper()
		except (ValueError, IndexError):
			log_line(SMS_LOG, 'out\t%d\t%s\tfailed\n' % (time.time(), number))
			return
		sms_out.append(number)
		s.sendall(('raws 25 - %s\n' % pdu).encode())

CALL_STATE = {0: 'active', 1: 'active', 2: 'dialing', 3: 'ringing', 4: 'incoming', 5: 'incoming'}

# The ear in a call: on while it dials, rings out or talks on the handset
# (not on the speaker, not while it rings in). Only with zrn-s6sy761 bound
# (the built-in s6sy761 has no ear_detect): nothing happens without it.
EAR_DEV = '/sys/bus/i2c/devices/1-0048'
ear = {'fd': None, 'near': False, 'lp_at': 0.0, 'far_at': 0.0}
# A far is believed only after it held EAR_FAR_HOLD s: held normally at the
# ear the IC flickers to far for 0.15-0.57 s (owner 11-16, A/B test). Its
# first report in low power is a false far, 0.7 to 7 s long, with the ear
# there: no far counts before EAR_LP_SETTLE s in low power (a real one then
# lights it late, the power key still does at once).
EAR_FAR_HOLD, EAR_LP_SETTLE = 0.8, 4.0

def ear_input():
	for name in glob.glob('/sys/class/input/event*/device/name'):
		try:
			with open(name) as f:
				if f.read().strip() == 's6sy761 ear detect':
					return '/dev/input/' + name.split('/')[4]
		except OSError:
			pass
	return None

def ear_mode(v):
	try:
		with open(EAR_DEV + '/ear_detect', 'w') as f:
			f.write('%d\n' % v)
		return True
	except OSError:
		return False

def ear_lp(v):	# the IC's low power mode while dark: it senses the ear on
	try:		# with the panel off (stock enters it at display off)
		with open(EAR_DEV + '/lp_mode', 'w') as f:
			f.write('%d\n' % v)
	except OSError:
		pass

def ear_say(near):
	ear['far_at'] = 0.0
	if near != ear['near']:
		ear['near'] = near
		audio_log.append('%.3f ear %s' % (time.time(), 'near' if near else 'far'))
		if near:
			tell_shell('prox near\n')
			time.sleep(0.15)	# the panel off first, as stock
			ear_lp(1)
			ear['lp_at'] = time.time()
		else:
			ear_lp(0)
			tell_shell('prox far\n')

def ear_event(near):	# a report from the IC: a far waits (see EAR_FAR_HOLD)
	t = time.time()
	if near or not ear['near']:
		ear_say(near)
		return
	if not ear['far_at']:
		ear['far_at'] = max(t + EAR_FAR_HOLD, ear['lp_at'] + EAR_LP_SETTLE)

def ear_tick():
	want = call['state'] in ('dialing', 'ringing', 'active') and not call_spk
	if want and ear['fd'] is None:
		dev = os.path.exists(EAR_DEV + '/ear_detect') and ear_input()
		if dev and ear_mode(1):
			try:
				ear['fd'] = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
			except OSError:
				ear_mode(0)
	elif not want and ear['fd'] is not None:
		os.close(ear['fd'])
		ear['fd'] = None
		ear_say(False)	# the screen back at the end, or for the speaker
		ear_mode(0)
	if ear['fd'] is None:
		return
	if ear['far_at'] and time.time() >= ear['far_at']:
		ear_say(False)
	try:
		data = os.read(ear['fd'], 24 * 16)
	except BlockingIOError:
		return
	except OSError:	# the device went (unbound): as if far
		os.close(ear['fd'])
		ear['fd'] = None
		ear_say(False)
		return
	for i in range(0, len(data) - 23, 24):	# struct input_event, 64-bit
		typ, code, value = struct.unpack_from('<HHi', data, i + 16)
		if typ == 5 and code == 0x0b:	# EV_SW, SW_FRONT_PROXIMITY (1 near, 1 touch too)
			ear_event(value != 0)

def publish():
	reg = REG.get(now['voice'], 'none')
	if reg not in ('home', 'roaming') and now['data'] in (1, 5):
		reg = REG[now['data']]
	bars = now['bars']
	if bars <= 0 and reg in ('home', 'roaming'):
		bars = 3
	ps = now['data'] in (1, 5)
	text = ('sim=ready\noperator=%s\nbars=%d\nregistered=%s\ntech=%s\nairplane=0\n'
		'data=%d\ndata_on=%d\ncall=%s\ncall_number=%s\nnet_mode=%s\n' % (now['operator'], bars, reg,
		tech_of(now['rat']) if ps else '', data['state'] == 'up', os.path.exists(WISH),
		call['state'] or 'idle', call['number'], net_mode()))
	try:
		with open(STATUS) as f:
			if f.read() == text:
				return
	except OSError:
		pass
	os.makedirs(os.path.dirname(STATUS), exist_ok=True)
	with open(STATUS + '.new', 'w') as f:
		f.write(text)
	os.chmod(STATUS + '.new', 0o644)
	os.rename(STATUS + '.new', STATUS)
	tell_shell('modem\n')

def wish(on):
	try:
		if on:
			open(WISH, 'a').close()
		elif os.path.exists(WISH):
			os.unlink(WISH)
	except OSError:
		pass
	publish()

def gone(*_):
	ring(False)
	if ear['fd'] is not None:
		ear_say(False)
		ear_mode(0)
	stop(keeper)
	for f in (STATUS, CTL):
		try:
			os.unlink(f)
		except OSError:
			pass
	sys.exit(0)

signal.signal(signal.SIGTERM, gone)
s = socket.socket(socket.AF_UNIX); s.connect('/dev/socket/zril'); s.settimeout(0.15)
os.makedirs(VARDIR + '/sms', exist_ok=True)
try:
	os.unlink(CTL)
except OSError:
	pass
net_load()
os.mkfifo(CTL); os.chmod(CTL, 0o666)	# anyone may write, as on the S9
ctl, cbuf = os.open(CTL, os.O_RDWR | os.O_NONBLOCK), b''
buf, due, poll = b'', 0, 0
data_safe(data_start)
try:
	while True:
		try:
			cbuf += os.read(ctl, 512)
		except BlockingIOError:
			pass
		while b'\n' in cbuf:
			cmd, cbuf = cbuf.split(b'\n', 1)
			command(cmd.decode(errors='replace').strip())
		ear_tick()
		data_safe(data_tick)
		if not call['state'] and not os.path.exists(VSS_STARTED) and uptime() > VSS_AT:
			start_vss()
		if time.time() >= due:
			s.sendall(b'raw0 19\nraw0 22\nraw0 20\nraw0 21\n')
			due = time.time() + 30
		if snaps and time.time() >= snaps[0][0]:
			snapshot(snaps.pop(0)[1])
		if diag_due and time.time() >= diag_due:
			call_path()
			diag_due = time.time() + DIAG_EVERY
		if log_due and time.time() >= log_due:
			snapshot(limit=40000, mark=False)
			log_due = time.time() + LOG_EVERY
		if poll and time.time() >= poll:
			s.sendall(b'raw0 9\n')
			poll = time.time() + 2 if call['state'] else 0
		try:
			chunk = s.recv(8192)
		except socket.timeout:
			continue
		if not chunk:
			break
		buf += chunk
		while b'\n' in buf:
			line, buf = buf.split(b'\n', 1)
			line = line.decode(errors='replace')
			if re.match(r'! (1000|1002) ', line):
				due = min(due, time.time() + 2)	# let the change settle
				continue
			if line.startswith('! 1001 '):	# CALL_STATE_CHANGED
				poll = time.time()
				continue
			if line.startswith('! 1010 '):
				data_safe(data_changed, line)
				continue
			m = re.match(r'< (\d+) #\d+ e=(\d+) n=\d+ ?(.*)', line)
			if not m:
				continue
			req, ok, rest = int(m.group(1)), m.group(2) == '0', m.group(3)
			if req in (27, 57):
				data_safe(data_answer, req, ok, rest)
			if req == 111:
				data_safe(attach_answer, ok, m.group(2))
			if req in (73, 74):
				data_safe(net_answer, req, ok, rest)
			if req == SCREEN_REQ:
				data_safe(screen_answer, ok)
			if req == 59 and audio_log:
				audio_log.append('%.3f < %s' % (time.time(), line))
			if req == 18 and os.path.isdir('/var/lib/zfield/repo/logs'):
				log_line('/var/lib/zfield/repo/logs/call-fail.log', '%s %s\n' % (time.strftime('%F %T'), line))
			if req == 25 and sms_out:
				log_line(SMS_LOG, 'out\t%d\t%s\t%s\n' % (time.time(), sms_out.pop(0), 'ok' if ok else 'failed'))
			elif req == 10 and not ok:
				call_set('')
			if not ok:
				continue
			if req == 9:
				calls = re.findall(r'\{idx (\d+) state (\d+) (in|out) (\S+)\}', rest)
				if not calls:
					call_set('')
				else:
					idx, st, _, number = calls[0]
					call['idx'] = int(idx)
					call_set(CALL_STATE.get(int(st), 'active'), '' if number == '?' else number)
					poll = poll or time.time() + 2
			if req == 19 and re.fullmatch(r'[0-9a-f]+', rest):
				b = bytes.fromhex(rest)
				now['bars'] = bars_of(struct.unpack('<%di' % (len(b) // 4), b[:len(b) // 4 * 4]))
			elif req == 22:
				names = re.findall(r'\[([^\]]*)\]', rest)
				now['operator'] = next((n for n in names if n and n != '-'), '')
				plmn = next((n for n in names if re.fullmatch(r'\d{5,6}', n)), '')
				if plmn != now['plmn']:
					now['plmn'] = plmn
					data_safe(apn_file_sync)
			elif req in (20, 21):
				r = re.match(r'reg (\d+) rat (\d+)', rest)
				if r:
					attach['plmn'] = reg_plmn(rest) or attach['plmn']
					now['voice' if req == 20 else 'data'] = int(r.group(1))
					if req == 21:
						now['rat'] = int(r.group(2))
					else:
						publish()	# 21 follows: written again if it changes
			if req == 21:
				publish()
finally:
	gone()
