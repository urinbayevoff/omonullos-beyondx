#!/usr/bin/env python3
"""zgov: a small CPU frequency governor for the Galaxy S10 5G, in user space.

  zgov                 run: every PERIOD s, each cluster to the lowest fvmap
                       level that covers its busiest core's load
  zgov once            one pass, print what it would ask for and why

There is no cpufreq driver yet, so zcpu.service used to put the clusters at
fixed 1950 / 2210 / 2340 MHz for good: at the voltage those levels take, the
phone ran warm and drained its battery doing nothing (2026-10-18: off the
cable it got hot and went flat). zgov asks the APM (zacpm's DVFS: FREQ_REQ to
an fvmap level, the APM picks the voltage) for what the load needs, like
schedutil: next = 1.25 * current * busy, up at once, down only after DOWN_HOLD
quiet passes. Floors are the stock DT cpufreq min-freq (442 / 507 / 520 MHz),
caps the old zcpu levels. zthermal and zgpu keep guarding heat.

Input boost: a touch (any event from a touchscreen, an evdev device with
ABS_MT_POSITION_X) puts every cluster at its cap at once and keeps it there
for BOOST s after the last event; then the load decides again (down after
DOWN_HOLD as always). Load alone left the big clusters at 650/520 MHz through
a flick in Nullgram: a frame's work comes in bursts, too late at that clock,
yet under 80 % of a 250 ms pass (2026-11-03: glide time lost ~2470 ms under
zgov, ~1110 ms with the clusters fixed at the caps). A process can ask the
same through the datagram socket BOOST_SOCK (anyone may send; the payload, if
a number, is the boost in ms, up to BOOST_MAX): for touches that do not come
through evdev (ztmetro.ctl's) or work a touch does not start.

Screen off: the touchscreens are inhibited (the input core closes them:
s6sy761 SENSE_OFF) and their events boost nothing. In a pocket the dark glass
kept reporting touches and every one put the clusters at their caps
(2026-11-11: ~510 mA screen off against ~400 with the glass inhibited). The
screen is the DSI connector's DPMS, read each pass; at start and on the way
out the touchscreens are opened again, so a zgov that dies leaves no dead glass.

Deep idle with the screen off (only while SICD_SWITCH exists): SICD_DELAY s
after the screen goes off, zrn-cpuidle-bx gets its stock lpm_init groups (once
per boot), CPD of cores 6-7 and SICD with no try limit; at screen on (and on
the way out) cpd and sicd go back off. The module's display guard switches
SICD off by itself the moment DECON scans out a new buffer, before zgov sees
the DPMS change (2026-11-12 09:38: SICD across a screen-on hung the phone).
Without the guard zgov leaves SICD alone. 15 min of this by hand
(scripts/zsicd-watch.sh, 2026-11-12 10:23): no hang, 6869 entries, ~345 mA.

Added for OmonullOS, October 2026. Writes nothing but CPU DVFS requests
(the owner's «ha» for those, 2026-10-17; the input boost, 2026-11-03), the
touchscreens' input inhibited flag (2026-11-11) and, behind SICD_SWITCH,
zrn-cpuidle-bx's lpm/cpd/tries/sicd parameters (2026-11-12).
"""

import importlib.machinery
import importlib.util
import os
import select
import signal
import socket
import sys
import time

PERIOD = 0.25
DOWN_HOLD = 4		# quiet passes before a step down (1 s)
JUMP = 0.90		# a core this busy takes the cluster straight to its cap
BOOST = 1.0		# s at the caps after the last touch
BOOST_MAX = 5.0		# s, the most a BOOST_SOCK datagram can ask
BOOST_SOCK = "/run/zgov.boost"
ABS_MT_POSITION_X = 0x35
SICD_SWITCH = "/etc/zgov-sicd"	# deep idle with the screen off only while this exists
SICD_DELAY = 3.0	# s of screen off before SICD
IDLE_P = "/sys/module/zrn_cpuidle_bx/parameters/"
LPM_GROUPS = (64, 32, 16, 1, 2)	# stock lpm_init, one group per write (= 115)
CLUSTERS = {		# domain: (cpus, floor MHz, cap MHz)
    "cpucl0": ((0, 1, 2, 3), 442, 1950),
    "cpucl1": ((4, 5), 520, 2210),
    "cpucl2": ((6, 7), 520, 2340),
}


def load_zacpm():
    for path in ("/usr/local/bin/zacpm", __file__.rsplit("/", 1)[0] + "/zacpm.py"):
        try:
            loader = importlib.machinery.SourceFileLoader("zacpm", path)
            spec = importlib.util.spec_from_loader("zacpm", loader)
            mod = importlib.util.module_from_spec(spec)
            loader.exec_module(mod)
            return mod
        except FileNotFoundError:
            continue
    raise SystemExit("zgov: zacpm not found")


def cpu_times():
    """{cpu: (busy, total)} jiffies from /proc/stat."""
    out = {}
    with open("/proc/stat") as f:
        for line in f:
            if not line.startswith("cpu") or line[3] == " ":
                continue
            p = line.split()
            v = [int(x) for x in p[1:9]]
            idle = v[3] + v[4]
            out[int(p[0][3:])] = (sum(v) - idle, sum(v))
    return out


def say(msg):
    try:
        with open("/dev/kmsg", "w") as k:
            k.write("zboot| gov: %s\n" % msg)
    except OSError:
        pass


def touchscreens():
    """Open fds of the evdev touchscreens (ABS_MT_POSITION_X in their abs bits)."""
    fds = []
    base = "/sys/class/input"
    for ev in sorted(os.listdir(base)) if os.path.isdir(base) else ():
        if not ev.startswith("event"):
            continue
        try:
            with open("%s/%s/device/capabilities/abs" % (base, ev)) as f:
                words = f.read().split()	# hex longs, the highest first
            bits = 0
            for w in words:
                bits = (bits << 64) | int(w, 16)
            if bits >> ABS_MT_POSITION_X & 1:
                fds.append(os.open("/dev/input/" + ev, os.O_RDONLY | os.O_NONBLOCK))
        except (OSError, ValueError):
            continue
    return fds


def screen_on():
    """The DSI panel's DPMS state from sysfs; on when it cannot be read."""
    base = "/sys/class/drm"
    try:
        for d in os.listdir(base):
            if "-DSI-" in d:
                with open("%s/%s/dpms" % (base, d)) as f:
                    return f.read().strip() != "Off"
    except OSError:
        pass
    return True


def touch_inhibit(fds, off):
    """Inhibit (sense off) or reopen each touchscreen behind these evdev fds."""
    for fd in fds:
        try:
            ev = os.path.basename(os.readlink("/proc/self/fd/%d" % fd))
            with open("/sys/class/input/%s/device/inhibited" % ev, "w") as f:
                f.write("1" if off else "0")
        except OSError as e:
            say("touch inhibit %d: %s" % (off, e))


def boost_socket():
    try:
        os.unlink(BOOST_SOCK)
    except FileNotFoundError:
        pass
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        s.bind(BOOST_SOCK)
        os.chmod(BOOST_SOCK, 0o666)
        s.setblocking(False)
        return s
    except OSError as e:
        say("%s: %s (no boost by socket)" % (BOOST_SOCK, e))
        return None


def idle_param(name, value=None):
    """Read (value None) or write one zrn-cpuidle-bx parameter; None on error."""
    try:
        if value is None:
            with open(IDLE_P + name) as f:
                return f.read().strip()
        with open(IDLE_P + name, "w") as f:
            f.write(str(value))
        return True
    except OSError as e:
        if value is not None:
            say("idle %s=%s: %s" % (name, value, e))
        return None


def deep_idle(on):
    """CPD of cores 6-7 + SICD on (screen off) or off. True if it is now on."""
    if not on:
        if idle_param("sicd") == "Y" or idle_param("cpd") not in (None, "0"):
            idle_param("sicd", 0)
            idle_param("cpd", 0)
            say("deep idle off: sicd %s/%s, disp_blocked %s" % (
                idle_param("attempts"), idle_param("entries"), idle_param("disp_blocked")))
        return False
    if not os.path.exists(SICD_SWITCH) or idle_param("disp_guard") != "Y":
        return False
    try:
        lpm = int(idle_param("lpm") or 0)
    except ValueError:
        lpm = 0
    for g in LPM_GROUPS:
        if not lpm & g and not idle_param("lpm", g):
            return False
    os.sync()	# a hang here should not cost the logs
    if not (idle_param("cpd", 2) and idle_param("tries", 0) and idle_param("sicd", 1)):
        idle_param("cpd", 0)
        return False
    say("deep idle on: lpm %s, cpd 2, sicd %s" % (idle_param("lpm"), idle_param("sicd")))
    return True


class Cluster:
    def __init__(self, name, cpus, floor, cap, table):
        self.name, self.cpus = name, cpus
        self.levels = sorted(r // 1000 for r, _ in table if floor <= r // 1000 <= cap)
        if not self.levels:
            raise SystemExit("zgov: %s has no fvmap level in %d..%d MHz" % (name, floor, cap))
        self.cur, self.quiet = None, 0

    def pick(self, busy):
        """The level for this load (busy 0..1 of the busiest core)."""
        cur = self.cur or self.levels[-1]
        if busy >= JUMP:
            return self.levels[-1]
        want = 1.25 * cur * busy
        for mhz in self.levels:
            if mhz >= want:
                return mhz
        return self.levels[-1]


def main(argv):
    once = argv[1:] == ["once"]
    if argv[1:] and not once:
        print(__doc__.split("\n\n")[0])
        return 2
    z = load_zacpm()
    fv = z.fvmap(z.Acpm())
    apm = z.Acpm(z.DVFS_CHANNEL)
    cl = [Cluster(n, c, lo, hi, fv[n][1]) for n, (c, lo, hi) in CLUSTERS.items()]

    def request(c, mhz):
        r = apm.call([z.DVFS_DOMAINS.index(c.name), mhz * 1000, z.FREQ_REQ, 0])
        if r is None:
            say("%s %d MHz: no answer from the APM" % (c.name, mhz))
            return False
        c.cur = mhz
        return True

    prev = cpu_times()
    touch, sock, boost_until = [], None, 0.0
    lit = None		# the screen as last seen: None until the first pass
    dark_since = None	# screen off since (monotonic), until deep idle goes on
    if not once:
        for c in cl:	# start low; load brings it up within one pass
            request(c, c.levels[0])
        touch, sock = touchscreens(), boost_socket()
        touch_inhibit(touch, False)	# a zgov before us may have died with them off
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
        say("running: %s; at the caps for %.1f s after a touch (%d touchscreen(s)%s)" % (
            ", ".join("%s %d..%d MHz" % (c.name, c.levels[0], c.levels[-1]) for c in cl),
            BOOST, len(touch), ", " + BOOST_SOCK if sock else ""))
    t = time.monotonic()
    next_pass, rescan = t + PERIOD, t + 10
    while True:
        fds = touch + ([sock] if sock else [])
        wait = max(0.0, next_pass - time.monotonic())
        if fds:
            ready = select.select(fds, [], [], wait)[0]
        else:
            time.sleep(wait)
            ready = []
        t = time.monotonic()
        for r in ready:
            want = BOOST
            if r is sock:
                try:
                    data = sock.recv(64).strip()
                    if data:
                        want = min(max(float(data) / 1000, 0.0), BOOST_MAX)
                except OSError:
                    continue
                except ValueError:
                    pass
            else:
                try:
                    while os.read(r, 4096):	# events: their content does not matter
                        pass
                    raise OSError("gone")	# EOF
                except BlockingIOError:
                    pass
                except OSError:
                    touch.remove(r)
                    os.close(r)
                    continue
                if lit is False:
                    continue	# the dark glass (and its release on inhibit)
            boost_until = max(boost_until, t + want)
        boosted = t < boost_until
        if ready and boosted:	# at once, not at the next pass
            for c in cl:
                if c.cur != c.levels[-1]:
                    c.quiet = 0
                    request(c, c.levels[-1])
        if not touch and not once and t >= rescan:	# the touch driver came up after us
            touch, rescan = touchscreens(), t + 10
            if touch:
                say("%d touchscreen(s) now" % len(touch))
                lit = None	# set them as the screen is at the next pass
        if t < next_pass:
            continue
        next_pass += PERIOD
        if next_pass <= t:	# fell behind (an APM call stalled): start the passes anew
            next_pass = t + PERIOD
        lit_now = screen_on() if not once else lit
        if lit_now != lit:
            lit = lit_now
            touch_inhibit(touch, not lit)
            dark_since = None if lit else t
            if lit and not once:
                deep_idle(False)
        if dark_since is not None and t - dark_since >= SICD_DELAY:
            dark_since = None
            deep_idle(True)
        now = cpu_times()
        for c in cl:
            busy = 0.0
            for cpu in c.cpus:
                if cpu in now and cpu in prev:
                    db, dt = now[cpu][0] - prev[cpu][0], now[cpu][1] - prev[cpu][1]
                    if dt > 0:
                        busy = max(busy, db / dt)
            mhz = c.levels[-1] if boosted else c.pick(busy)
            if once:
                print("%s busy %3.0f%% -> %d MHz (levels %s)" % (
                    c.name, 100 * busy, mhz, " ".join(map(str, c.levels))))
                continue
            if c.cur is None or mhz > c.cur:
                c.quiet = 0
                request(c, mhz)
            elif mhz < c.cur:
                c.quiet += 1
                if c.quiet >= DOWN_HOLD:
                    c.quiet = 0
                    request(c, mhz)
            else:
                c.quiet = 0
        if once:
            return 0
        prev = now


def run(argv):
    try:
        return main(argv)
    finally:	# whatever ends us, the glass works again
        if argv[1:] != ["once"]:
            touch_inhibit(touchscreens(), False)
            deep_idle(False)


if __name__ == "__main__":
    sys.exit(run(sys.argv))
