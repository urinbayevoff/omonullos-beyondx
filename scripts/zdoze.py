#!/usr/bin/env python3
# scripts/zdoze.py: the app sleep policy on the S10 5G (beyondx), its step 3
# (the owner's rule, 2026-11-18): a terminal with work running keeps the phone
# awake, an idle one does not; and its step 2a: once the screen has been off for
# FREEZE_AFTER s, every idle tmux pane is frozen (cgroup v2 cgroup.freeze, each
# pane is its own tmux-spawn-*.scope) until the screen comes on again, so an
# idle program in it (Claude: ~1.5 kB every 10 s) cannot keep the modem busy.
# A busy pane is never frozen, nor one running systemd-inhibit (keep awake by
# hand). Step 2b: apps the same way. ztMETRO tells zrnperfd's pipe
# /run/zrnperfd.ctl which app is in front ("focus PID APP_ID"; zrnperfd itself
# is off on the S10, it writes the S9's cpufreq and charger), zdoze reads that
# pipe, moves each app shown (its process and children) into a cgroup of its
# own inside the one it was in (zrnperfd's tickle does the same), and freezes
# the idle ones with the screen off. Never: the Phone, terminals (2a has them),
# Waydroid, the shell, and the apps in Settings' background list
# (~/.config/ztos/background-apps). /run/zdoze/focus = the app in front (zmon).
# /var/lib/zdoze/bgstat: per day and app, the CPU s and I/O kB it used while
# not in front, screen on and screen off apart (to see which apps are worth
# freezing with the screen on too; `zdoze bg` prints it).
# Installed as /usr/local/bin/zdoze, run as root.
#
# A terminal is a foot window or a tmux server (the owner's tmux sessions are
# children of PID 1, not of foot), and its work is every process under it.
# Busy = in the last WINDOW s its processes used more than CPU_PCT % of one
# core, or read and wrote (sockets, pipes, files: /proc/PID/io rchar+wchar)
# more than IO_KB kB. While busy, /run/zdoze/busy holds why and zsleepd does
# not put the phone to sleep. Not below BATT_FLOOR % (then the phone sleeps
# over the work: a frozen job goes on after the wake) and not over a charger
# (zsleepd keeps it awake then anyway). To keep it awake by hand: systemd-inhibit.
#
#   zdoze            the daemon (every TICK s)
#   zdoze sample [N] print each terminal's and pane's numbers N times, nothing written
#   zdoze bg         what each app used while not in front (bgstat)
#   zdoze thaw       thaw every tmux pane (if a killed daemon left one frozen)
# Off switches: /etc/zdoze.off (all), /etc/zdoze-freeze.off (freezing only).
# Log: /var/lib/zdoze/zdoze.log (changes only).
# Added for OmonullOS, November 2026.
import glob, os, select, signal, stat, sys, time

TICK = 5
WINDOW = int(os.environ.get('WINDOW', 30))	# s
CPU_PCT = float(os.environ.get('CPU_PCT', 10))	# % of one core over WINDOW (idle Claude: ~2, blips to 13 with the screen off)
IO_KB = float(os.environ.get('IO_KB', 64))	# kB over WINDOW
BATT_FLOOR = int(os.environ.get('BATT_FLOOR', 15))	# %
ROOTS = ('foot', 'tmux: server')
RUN = '/run/zdoze'
BUSY = RUN + '/busy'
OFF = '/etc/zdoze.off'
FREEZE_OFF = '/etc/zdoze-freeze.off'
FREEZE_AFTER = int(os.environ.get('FREEZE_AFTER', 30))	# s of screen off
CG_USER = '/sys/fs/cgroup/user.slice'
PANES = CG_USER + '/user-*.slice/user@*.service/tmux-spawn-*.scope'
DPMS = (glob.glob('/sys/class/drm/card*-DSI-1/dpms') or [''])[0]
CTL = '/run/zrnperfd.ctl'	# ztMETRO's perf_tell() writes here
APP_CG = 'zdoze-app-'
APPS = RUN + '/apps'	# "cgroup app_id" per line, for a restarted zdoze
FOCUS = RUN + '/focus'
BGSTAT = '/var/lib/zdoze/bgstat'	# per day and app: what it used while not in front
NEVER = ('ztphone', 'foot', 'waydroid.', 'Waydroid', 'ztos-', 'ztmetro', '-')
LOG = '/var/lib/zdoze/zdoze.log'
HZ = os.sysconf('SC_CLK_TCK')
SELF = os.getpid()	# "zdoze sample" run from a terminal does not count
BATT = '/run/zbatt/capacity'	# zlowbatt writes it every 10 s


def cg_pids(cg):
	try:
		with open(cg + '/cgroup.procs') as f:
			return [int(x) for x in f.read().split()]
	except (OSError, ValueError):
		return []


def user_pids():
	"""every process in user.slice: terminals and their work live there, the
	kernel threads (most of /proc) do not. Cheaper than reading all of /proc."""
	out, todo = [], [CG_USER]
	while todo:
		cg = todo.pop()
		out += cg_pids(cg)
		try:
			todo += [e.path for e in os.scandir(cg) if e.is_dir(follow_symlinks=False)]
		except OSError:
			pass
	return out


def procs():
	"""{pid: (ppid, comm, cpu ticks incl. reaped children)}"""
	out = {}
	for d in user_pids():
		try:
			with open('/proc/%d/stat' % d) as f:
				st = f.read()
		except OSError:
			continue
		comm = st[st.index('(') + 1:st.rindex(')')]
		f = st[st.rindex(')') + 2:].split()
		# fields after comm: state ppid ... utime(12) stime(13) cutime(14) cstime(15)
		out[d] = (int(f[1]), comm, sum(int(x) for x in f[11:15]))
	return out


def io_of(pid):
	try:
		with open('/proc/%d/io' % pid) as f:
			v = dict(l.split(': ') for l in f.read().splitlines())
		return int(v['rchar']) + int(v['wchar'])
	except (OSError, KeyError, ValueError):
		return 0


def terminals():
	"""{root pid: (root comm, {pid: (comm, ticks, io)} of everything under it)}"""
	ps = procs()
	kids = {}
	for pid, (ppid, _, _) in ps.items():
		kids.setdefault(ppid, []).append(pid)
	out = {}
	for root, (_, comm, _) in ps.items():
		if comm not in ROOTS:
			continue
		# a tmux client inside foot is not a second terminal: its server is
		under, todo = {}, list(kids.get(root, ()))
		while todo:
			p = todo.pop()
			if p in ps and ps[p][1] not in ROOTS and p != SELF:
				under[p] = (ps[p][1], ps[p][2], io_of(p))
				todo.extend(kids.get(p, ()))
		out[root] = (comm, under)
	return out


class Meter:
	"""per terminal: CPU % and I/O kB over the last WINDOW s"""
	def __init__(self):
		self.hist = []	# (time, {root: {pid: (comm, ticks, io)}})

	def take(self):
		t = time.monotonic()
		snap = {r: u for r, (_, u) in terminals().items()}
		self.hist.append((t, snap))
		while len(self.hist) > 2 and t - self.hist[1][0] >= WINDOW:
			self.hist.pop(0)
		t0, old = self.hist[0]
		res = {}
		for root, now in snap.items():
			was = old.get(root, {})
			cpu = io = 0
			top, top_cpu = '', -1
			for pid, (comm, ticks, b) in now.items():
				# a process born in the window counts from zero
				_, t_was, b_was = was.get(pid, (comm, 0, 0))
				dc, db = max(ticks - t_was, 0), max(b - b_was, 0)
				cpu += dc
				io += db
				if dc > top_cpu:
					top, top_cpu = comm, dc
			span = max(t - t0, 1e-3)
			res[root] = (100.0 * cpu / HZ / span, io / 1024.0, top, span)
		return res


def screen_off():
	try:
		with open(DPMS) as f:
			return f.read().strip() == 'Off'
	except OSError:
		return False	# not known: never freeze


def comm_of(pid):
	try:
		with open('/proc/%d/comm' % pid) as f:
			return f.read().strip()
	except OSError:
		return ''


def frozen(cg):
	try:
		with open(cg + '/cgroup.freeze') as f:
			return f.read().strip() == '1'
	except OSError:
		return False


def set_freeze(cg, on):
	try:
		with open(cg + '/cgroup.freeze', 'w') as f:
			f.write('1' if on else '0')
		return True
	except OSError as e:
		say('%s %s: %s' % ('freeze' if on else 'thaw', os.path.basename(cg), e))
		return False


class PaneMeter:
	"""per tmux pane scope: CPU % (cpu.stat, counts exited processes too)
	and I/O kB of its processes over the last WINDOW s"""
	def __init__(self):
		self.hist = []	# (time, {scope: (usec, {pid: io})})

	def take(self, extra=()):
		t = time.monotonic()
		snap = {}
		for cg in glob.glob(PANES) + list(extra):
			if frozen(cg):
				continue	# not measured: it does nothing
			try:
				with open(cg + '/cpu.stat') as f:
					usec = int(f.readline().split()[1])	# usage_usec
			except (OSError, ValueError, IndexError):
				continue
			snap[cg] = (usec, {p: io_of(p) for p in cg_pids(cg)})
		self.hist.append((t, snap))
		while len(self.hist) > 2 and t - self.hist[1][0] >= WINDOW:
			self.hist.pop(0)
		t0, old = self.hist[0]
		res = {}
		for cg, (usec, ios) in snap.items():
			if cg not in old:
				res[cg] = (0.0, 0.0, 0.0)	# new or just thawed: seen from now
				continue
			u0, io0 = old[cg]
			io = sum(max(b - io0.get(p, 0), 0) for p, b in ios.items())
			span = max(t - t0, 1e-3)
			res[cg] = (100.0 * max(usec - u0, 0) / 1e6 / span, io / 1024.0, span)
		return res


def pane_busy(cg, cpu, io, span):
	"""why this pane must not be frozen, or ''"""
	if span < WINDOW / 2:
		return 'just seen'
	if cpu > CPU_PCT or io > IO_KB:
		return '%.0f%% %.0fkB' % (cpu, io)
	if any(comm_of(p) == 'systemd-inhibit' for p in cg_pids(cg)):
		return 'systemd-inhibit'
	return ''


def thaw_all(why):
	for cg in glob.glob(PANES) + glob.glob(CG_USER + '/**/' + APP_CG + '*', recursive=True):
		if frozen(cg) and set_freeze(cg, False):
			say('thaw %s (%s)' % (os.path.basename(cg), why))


def bg_list():
	"""Settings' background apps (background-apps.h): never frozen"""
	try:
		import pwd
		home = pwd.getpwuid(1000).pw_dir
		with open(home + '/.config/ztos/background-apps') as f:
			return {l.strip().rsplit('/', 1)[-1].removesuffix('.desktop') for l in f if l.strip()}
	except (OSError, KeyError):
		return set()


def descendants(root):
	ps = procs()
	kids = {}
	for pid, (ppid, _, _) in ps.items():
		kids.setdefault(ppid, []).append(pid)
	out, todo = [], [root]
	while todo:
		p = todo.pop()
		out.append(p)
		todo.extend(kids.get(p, ()))
	return out


class Apps:
	"""the apps ztMETRO has shown, each in its own cgroup"""
	def __init__(self):
		self.cg = {}	# app_id -> cgroup dir
		self.front = ''
		self.fd = None
		try:
			with open(APPS) as f:
				for l in f:
					cg, app = l.split()
					if os.path.isdir(cg):
						self.cg[app] = cg
		except (OSError, ValueError):
			pass
		self.open_pipe()

	def open_pipe(self):
		try:
			if not stat.S_ISFIFO(os.stat(CTL).st_mode):
				os.unlink(CTL)
				raise FileNotFoundError
		except FileNotFoundError:
			os.mkfifo(CTL)
		os.chmod(CTL, 0o666)	# ztMETRO runs as the user
		self.fd = os.open(CTL, os.O_RDONLY | os.O_NONBLOCK)
		self.keep = os.open(CTL, os.O_WRONLY | os.O_NONBLOCK)	# no EOF when ztMETRO closes it
		self.buf = b''

	def wait(self, s):
		"""wait up to s seconds, taking ztMETRO's lines as they come"""
		r, _, _ = select.select([self.fd], [], [], s)
		if not r:
			return
		try:
			self.buf += os.read(self.fd, 65536)
		except BlockingIOError:
			return
		*lines, self.buf = self.buf.split(b'\n')
		for l in lines:
			f = l.decode(errors='replace').split()
			if len(f) == 3 and f[0] == 'focus' and f[1].isdigit():
				self.focus(int(f[1]), f[2])

	def focus(self, pid, app):
		self.front = app
		try:
			with open(FOCUS + '.new', 'w') as f:
				f.write('%d %s\n' % (pid, app))
			os.replace(FOCUS + '.new', FOCUS)
		except OSError:
			pass
		if pid <= 1 or app.startswith(NEVER):
			return
		cg = self.cg.get(app)
		if cg and os.path.isdir(cg) and pid in cg_pids(cg):
			return
		try:
			with open('/proc/%d/cgroup' % pid) as f:
				cur = f.read().split('::', 1)[1].strip()
		except (OSError, IndexError):
			return
		if os.path.basename(cur).startswith(APP_CG):
			self.cg[app] = '/sys/fs/cgroup' + cur
			self.save()
			return
		if not cur.startswith('/user.slice/') or 'tmux-spawn-' in cur:
			return	# not the user's app, or a terminal's (2a)
		cg = '/sys/fs/cgroup%s/%s%d' % (cur, APP_CG, pid)
		try:
			os.makedirs(cg, exist_ok=True)
			for p in descendants(pid):
				with open(cg + '/cgroup.procs', 'w') as f:
					f.write(str(p))
		except OSError as e:
			say('adopt %s %d: %s' % (app, pid, e))
			return
		self.cg[app] = cg
		self.save()
		say('adopt %s %d -> %s' % (app, pid, cg[len(CG_USER):]))

	def save(self):
		with open(APPS + '.new', 'w') as f:
			for app, cg in self.cg.items():
				f.write('%s %s\n' % (cg, app))
		os.replace(APPS + '.new', APPS)

	def groups(self):
		"""{cgroup: app_id} of the apps that are still there and may sleep"""
		allowed = bg_list()
		out = {}
		for app, cg in list(self.cg.items()):
			if not cg_pids(cg) and not frozen(cg):
				try:
					os.rmdir(cg)	# the app has gone
				except OSError:
					pass
				del self.cg[app]
				continue
			if app not in allowed:
				out[cg] = app
		return out

	def thaw(self, why):
		for app, cg in self.cg.items():
			if frozen(cg) and set_freeze(cg, False):
				say('thaw %s (%s)' % (app, why))


class BgStat:
	"""what each app uses while it is not in front"""
	def __init__(self):
		self.last = {}	# cgroup -> (usec, io)
		self.day, self.tot = time.strftime('%F'), {}	# app -> [on cpu s, on kB, off cpu s, off kB]
		self.saved = time.monotonic()
		try:
			with open(BGSTAT) as f:
				for l in f:
					v = l.split()
					if len(v) == 6 and v[0] == self.day:
						self.tot[v[1]] = [float(x) for x in v[2:]]
		except (OSError, ValueError):
			pass

	def take(self, groups, front, off):
		for cg, app in groups.items():
			try:
				with open(cg + '/cpu.stat') as f:
					usec = int(f.readline().split()[1])
			except (OSError, ValueError, IndexError):
				continue
			io = sum(io_of(p) for p in cg_pids(cg))
			u0, io0 = self.last.get(cg, (usec, io))
			self.last[cg] = (usec, io)
			if app == front and not off:
				continue	# in use
			t = self.tot.setdefault(app, [0.0] * 4)
			k = 2 if off else 0
			t[k] += max(usec - u0, 0) / 1e6
			t[k + 1] += max(io - io0, 0) / 1024.0
		if time.monotonic() - self.saved >= 60 or time.strftime('%F') != self.day:
			self.save()

	def save(self):
		self.saved = time.monotonic()
		try:
			with open(BGSTAT) as f:
				keep = [l for l in f if not l.startswith(self.day + ' ')]
		except OSError:
			keep = []
		keep = keep[-2000:]
		with open(BGSTAT + '.new', 'w') as f:
			f.writelines(keep)
			for app, t in sorted(self.tot.items()):
				f.write('%s %s %.1f %.0f %.1f %.0f\n' % (self.day, app, *t))
		os.replace(BGSTAT + '.new', BGSTAT)
		if time.strftime('%F') != self.day:
			self.day, self.tot = time.strftime('%F'), {}


def bg_print():
	print('day        app                            on: CPU s    kB   off: CPU s    kB')
	try:
		with open(BGSTAT) as f:
			for l in f:
				d, app, a, b, c, e = l.split()
				print('%s %-30s %10s %6s %10s %6s' % (d, app, a, b, c, e))
	except OSError:
		print('(nothing yet)')


def battery():
	try:
		with open(BATT) as f:
			return int(f.read())
	except (OSError, ValueError):
		return 100


def say(line):
	os.makedirs(os.path.dirname(LOG), exist_ok=True)
	if os.path.exists(LOG) and os.path.getsize(LOG) > 1 << 20:
		os.replace(LOG, LOG + '.1')
	with open(LOG, 'a') as f:
		f.write('%s %s\n' % (time.strftime('%F %T'), line))


def verdict(res):
	"""why the phone must stay awake for a terminal, or ''"""
	if os.path.exists(OFF):
		return ''
	why = []
	for root, (cpu, io, top, span) in sorted(res.items()):
		if span < WINDOW / 2:
			continue	# not enough seen yet
		if cpu > CPU_PCT or io > IO_KB:
			why.append('%d %s %.0f%% %.0fkB' % (root, top, cpu, io))
	if why and battery() < BATT_FLOOR:
		return ''
	return ', '.join(why)


def sample(n):
	m, pm = Meter(), PaneMeter()
	for i in range(n):
		res = m.take()
		print(time.strftime('%T'), '  '.join('%d %.1f%% %.0fkB %s/%.0fs' % (r, c, io, top, s)
			for r, (c, io, top, s) in sorted(res.items())), '=>', verdict(res) or 'idle', flush=True)
		for cg, (c, io, s) in sorted(pm.take().items()):
			print('   pane %s %.1f%% %.0fkB/%.0fs => %s' % (os.path.basename(cg)[11:19], c, io, s,
				pane_busy(cg, c, io, s) or 'may freeze'), flush=True)
		if i + 1 < n:
			time.sleep(TICK)


stopped_apps = bgstat = None


def stopped(*_):
	try:
		os.unlink(BUSY)
	except OSError:
		pass
	thaw_all('stop')
	if bgstat:
		bgstat.save()
	if stopped_apps:
		stopped_apps.thaw('stop')
	say('stop')
	sys.exit(0)


def daemon():
	os.makedirs(RUN, exist_ok=True)
	signal.signal(signal.SIGTERM, stopped)
	signal.signal(signal.SIGINT, stopped)
	m, pm, was = Meter(), PaneMeter(), None
	apps = Apps()
	global bgstat
	bgstat = BgStat()
	say('start: WINDOW=%d CPU_PCT=%g IO_KB=%g BATT_FLOOR=%d FREEZE_AFTER=%d' %
		(WINDOW, CPU_PCT, IO_KB, BATT_FLOOR, FREEZE_AFTER))
	thaw_all('start')	# a killed daemon may have left some frozen
	apps.thaw('start')
	off_since = None	# monotonic: does not count the time asleep
	try:
		loop(m, pm, was, off_since, apps)
	finally:
		thaw_all('exit')
		apps.thaw('exit')


def loop(m, pm, was, off_since, apps):
	global stopped_apps
	stopped_apps = apps
	while True:
		off = screen_off()
		if not off:
			if off_since is not None:
				thaw_all('screen on')
				apps.thaw('screen on')
			off_since = None
		elif off_since is None:
			off_since = time.monotonic()
		bgstat.take({cg: app for app, cg in apps.cg.items()}, apps.front, off)
		groups = apps.groups()
		panes = pm.take(groups)
		if off and time.monotonic() - off_since >= FREEZE_AFTER and \
				not os.path.exists(OFF) and not os.path.exists(FREEZE_OFF):
			for cg, (cpu, io, span) in sorted(panes.items()):
				if not pane_busy(cg, cpu, io, span) and set_freeze(cg, True):
					say('freeze %s (%.0f%% %.0fkB/%.0fs: %s)' % (groups.get(cg) or os.path.basename(cg),
						cpu, io, span, ' '.join(sorted({comm_of(p) for p in cg_pids(cg)}))))
		why = verdict(m.take())
		if why:
			with open(BUSY + '.new', 'w') as f:
				f.write(why + '\n')
			os.replace(BUSY + '.new', BUSY)
		elif os.path.exists(BUSY):
			os.unlink(BUSY)
		if bool(why) != bool(was):
			say('busy: ' + why if why else 'idle (battery %d%%)' % battery())
		was = why
		# while something is frozen, see the screen come on within a second
		for _ in range(TICK):
			apps.wait(1)
			if off_since is not None and not screen_off():
				break


if __name__ == '__main__':
	if sys.argv[1:2] == ['sample']:
		sample(int(sys.argv[2]) if len(sys.argv) > 2 else 12)
	elif sys.argv[1:2] == ['bg']:
		bg_print()
	elif sys.argv[1:2] == ['thaw']:
		thaw_all('by hand')
	else:
		if os.getuid() != 0:
			sys.exit('run as root')
		daemon()
