#!/usr/bin/env python3
"""zmon: what the Galaxy S10 5G spends its battery on, logged all day.

  zmon run              the logger (zbattlog.service runs it): every 30 s one
                        JSON line into /var/lib/zmon/<day>.jsonl, and the old
                        zbattlog CSV line into /var/lib/zbattlog/<day>.csv
  zmon report [day]     a day's summary (default today): hours on battery,
                        screen on / off / asleep, mA in each, hour by hour, the
                        processes that took the CPU, and a power profile
                        fitted from the day's samples (mA per screen, CPU
                        core, Wi-Fi/mobile traffic) with each process's share
  zmon now              take one sample now and print it

Each line (one sample, values over the time since the one before):
  t, dt (s), slept (s spent suspended, CLOCK_BOOTTIME - CLOCK_MONOTONIC),
  soc, mv, ma, avg_ma (fuel gauge, - = discharging), qh (its coulomb counter,
  raw), temp (battery, C), chg (charger online), screen, bright (ztMETRO's
  software brightness, config), cpu (busy cores, 0..8), irq/gpu/touch (per s),
  wifi (up), cell (modem tech, data on), rx/tx (kB/s, Wi-Fi and mobile), top
  (the 6 processes with most CPU: name, CPU s), sleep (zsleepd's status line),
  meter (S2MPS19 and S2MPS20 rail currents, mA, only with /etc/zmon-meter.on).

Reads only: the fuel gauge (MAX77705 0x36 through i2ctransfer, as zbattlog),
/proc, /sys, /run status files and ztMETRO's config. The one exception is the
rail meters: with /etc/zmon-meter.on present zmon turns them on when it
starts (they are off after every boot; mux as S-Boot leaves it) and runs
`zacpm meter` and `meter2` once a minute, which also moves their ADC pointers.
That touches S2MPS19 0xbf-0xc2 and S2MPS20 0x49-0x4c only, the registers the
owner allowed for the meters. Without that file nothing is written to any chip.

Added for OmonullOS, November 2026: the drain work needed day-long data, not
only 20-minute A/B runs with the phone left alone.
"""

import glob
import json
import os
import subprocess
import sys
import time

OUT = "/var/lib/zmon"
OLD = "/var/lib/zbattlog"
KEEP_DAYS = 30
OLD_KEEP_DAYS = 10
STEP = 30
METER_FLAG = "/etc/zmon-meter.on"
METER_STEP = 60
# the rails each meter channel reads (S-Boot's own mux): set at every start,
# since the meters come up off (EN 0) after each boot
METER_SET = (["meter", "set", "1", "2", "3", "4", "5", "6", "11", "12"], ["meter2", "set", "1", "2", "3"])
CONF = "/home/alarm/.config/ztmetro/config"
HZ = os.sysconf("SC_CLK_TCK")
FG = 0x36


def i2c_bus():
    b = sorted(glob.glob("/sys/bus/platform/devices/15d60000.i2c/i2c-*"))
    return b[0].rsplit("-", 1)[1] if b else None


def rd16(bus, reg, signed=False):
    try:
        r = subprocess.run(["i2ctransfer", "-y", bus, "w1@0x%02x" % FG, "0x%02x" % reg, "r2"],
                           capture_output=True, text=True, timeout=3)
        lo, hi = (int(x, 16) for x in r.stdout.split())
    except Exception:
        return None
    v = lo | hi << 8
    if signed and v >= 32768:
        v -= 65536
    return v


def gauge(bus):
    if bus is None:
        return {}
    soc, mv, cur, avg = rd16(bus, 0x06), rd16(bus, 0x09), rd16(bus, 0x0a, True), rd16(bus, 0x0b, True)
    qh, temp = rd16(bus, 0x4d), rd16(bus, 0x08, True)
    g = {}
    if soc is not None:
        g["soc"] = round(soc / 256, 1)
    if mv is not None:
        g["mv"] = mv * 78125 // 1000000
    if cur is not None:		# 5 mOhm sense: 0.3125 mA per step (as zbattlog)
        g["ma"] = cur * 3125 // 10000
    if avg is not None:
        g["avg_ma"] = avg * 3125 // 10000
    if qh is not None:
        g["qh"] = qh
    if temp is not None:
        g["temp"] = round(temp / 256, 1)
    return g


def read(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def screen():
    for d in glob.glob("/sys/class/drm/card*-DSI-*/dpms"):
        s = read(d)
        if s:
            return "off" if s == "Off" else "on"
    return "?"


def brightness():
    for line in read(CONF).splitlines():
        k, _, v = line.partition("=")
        if k.strip() == "brightness":
            try:
                return int(v.strip())
            except ValueError:
                pass
    return 100


def irqs():
    tot = gpu = touch = 0
    try:
        with open("/proc/interrupts") as f:
            ncpu = len(f.readline().split())
            for line in f:
                p = line.split()
                s = sum(int(x) for x in p[1:1 + ncpu] if x.isdigit())
                tot += s
                if "panfrost-job" in line:
                    gpu += s
                if "104b0000.i2c" in line:
                    touch += s
    except OSError:
        pass
    return tot, gpu, touch


def cpu_jiffies():
    p = read("/proc/stat").split("\n", 1)[0].split()[1:]
    v = [int(x) for x in p]
    return sum(v), v[3] + v[4]		# all, idle + iowait


def proc_name(pid, comm):
    try:
        with open("/proc/%s/cmdline" % pid, "rb") as f:
            argv = [a.decode(errors="replace") for a in f.read().split(b"\0") if a]
    except OSError:
        return comm
    if not argv:
        return comm		# a kernel thread
    name = os.path.basename(argv[0])
    if name.startswith(("python", "sh", "bash", "node")) and len(argv) > 1:
        for a in argv[1:]:
            if not a.startswith("-"):
                return os.path.basename(a)
    return name


def procs():
    """{pid: (name, utime+stime jiffies)}"""
    out = {}
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % d) as f:
                s = f.read()
        except OSError:
            continue
        r = s.rfind(")")
        comm = s[s.find("(") + 1:r]
        f = s[r + 2:].split()
        out[d] = (comm, int(f[11]) + int(f[12]))
    return out


def netbytes():
    wifi, cell = [0, 0], [0, 0]
    for dev in os.listdir("/sys/class/net"):
        if dev == "lo":
            continue
        rx = int(read("/sys/class/net/%s/statistics/rx_bytes" % dev, "0") or 0)
        tx = int(read("/sys/class/net/%s/statistics/tx_bytes" % dev, "0") or 0)
        t = wifi if dev.startswith("wl") else cell if dev.startswith("rmnet") else None
        if t is not None:
            t[0] += rx
            t[1] += tx
    return wifi, cell


def cellstat():
    kv = dict(l.split("=", 1) for l in read("/run/ztoscorenetworking/status").splitlines() if "=" in l)
    return {"tech": kv.get("tech", ""), "data": kv.get("data_on", "") == "1",
            "call": kv.get("call", "")}


def meter_on():
    """turn both meters on (owner's «ha» 2026-11-19 11:20: at every boot)"""
    ok = True
    for args in METER_SET:
        try:
            r = subprocess.run(["/usr/local/bin/zacpm"] + args, capture_output=True, text=True, timeout=10)
            ok = ok and r.returncode == 0
        except Exception:
            ok = False
    return ok


def meter():
    """S2MPS19 rails as BUCKn/LDOn, the S2MPS20's (sub PMIC) as s:BUCKn"""
    m = {}
    for cmd, pre in (("meter", ""), ("meter2", "s:")):
        try:
            r = subprocess.run(["/usr/local/bin/zacpm", cmd], capture_output=True, text=True, timeout=8)
        except Exception:
            continue
        for line in r.stdout.splitlines():	# "ch0 BUCK1     62 mA (4)"
            p = line.split()
            if len(p) >= 4 and p[0].startswith("ch") and p[3] == "mA":
                m[pre + p[1]] = int(float(p[2]))
    return m or None


def clocks():
    return time.clock_gettime(time.CLOCK_BOOTTIME), time.clock_gettime(time.CLOCK_MONOTONIC)


class Sampler:
    def __init__(self):
        self.bus = i2c_bus()
        self.prev = None
        self.names = {}
        self.last_meter = 0

    def snap(self):
        boot, mono = clocks()
        return dict(boot=boot, mono=mono, irq=irqs(), cpu=cpu_jiffies(), procs=procs(), net=netbytes())

    def sample(self):
        now = self.snap()
        p, self.prev = self.prev, now
        if p is None:
            return None
        dt = now["boot"] - p["boot"]
        awake = max(now["mono"] - p["mono"], 0.001)
        s = {"t": int(time.time()), "dt": round(dt, 1), "slept": round(max(dt - awake, 0), 1)}
        s.update(gauge(self.bus))
        s["chg"] = read("/run/zbatt/online") == "1"
        s["screen"] = screen()
        s["bright"] = brightness()
        all_j, idle_j = now["cpu"][0] - p["cpu"][0], now["cpu"][1] - p["cpu"][1]
        # busy cores over the awake time (jiffies do not count while suspended)
        s["cpu"] = round((all_j - idle_j) / HZ / awake, 2)
        s["irq"], s["gpu"], s["touch"] = (round((a - b) / awake, 1) for a, b in zip(now["irq"], p["irq"]))
        (wr, wt), (cr, ct) = now["net"]
        (pwr, pwt), (pcr, pct) = p["net"]
        s["wifi"] = read("/sys/class/net/wlp1s0/operstate") == "up"
        s["cell"] = cellstat()
        s["rx"] = [round(max(wr - pwr, 0) / 1024 / dt, 1), round(max(cr - pcr, 0) / 1024 / dt, 1)]
        s["tx"] = [round(max(wt - pwt, 0) / 1024 / dt, 1), round(max(ct - pct, 0) / 1024 / dt, 1)]
        use = {}
        for pid, (comm, j) in now["procs"].items():
            old = p["procs"].get(pid)
            d = j - old[1] if old and old[0] == comm else j if old is None and j < HZ * dt else 0
            if d <= 0:
                continue
            if pid not in self.names or self.names[pid][0] != comm:
                self.names[pid] = (comm, proc_name(pid, comm))
            n = self.names[pid][1]
            use[n] = use.get(n, 0) + d
        for pid in list(self.names):
            if pid not in now["procs"]:
                del self.names[pid]
        s["top"] = [[n, round(j / HZ, 2)] for n, j in sorted(use.items(), key=lambda x: -x[1])[:6]]
        st = read("/run/zsleepd.status")
        if st:
            s["sleep"] = st
        if os.path.exists(METER_FLAG) and time.monotonic() - self.last_meter >= METER_STEP:
            self.last_meter = time.monotonic()
            m = meter()
            if m:
                s["meter"] = m
        return s


def prune(d, keep, ext):
    old = sorted(glob.glob(os.path.join(d, "*" + ext)))[:-keep]
    for f in old:
        try:
            os.unlink(f)
        except OSError:
            pass


def run():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(OLD, exist_ok=True)
    sm = Sampler()
    sm.sample()
    meter_ready = not os.path.exists(METER_FLAG) or meter_on()
    day = None
    while True:
        time.sleep(STEP)
        try:
            s = sm.sample()
        except Exception as e:		# never die on one bad read
            print("zmon: %s" % e, file=sys.stderr)
            continue
        if not meter_ready:		# the APM was not answering yet: try again
            meter_ready = meter_on()
        if not s:
            continue
        today = time.strftime("%Y%m%d")
        with open(os.path.join(OUT, today + ".jsonl"), "a") as f:
            f.write(json.dumps(s, separators=(",", ":"), ensure_ascii=False) + "\n")
        if "ma" in s:
            with open(os.path.join(OLD, today + ".csv"), "a") as f:
                f.write("%d,%s,%s,%s,%s,%s\n" % (s["t"], s.get("soc", ""), s.get("mv", ""), s["ma"],
                                                 s.get("avg_ma", ""), s["screen"]))
        if today != day:
            day = today
            prune(OUT, KEEP_DAYS, ".jsonl")
            prune(OLD, OLD_KEEP_DAYS, ".csv")


# ---- report ---------------------------------------------------------------

def solve(rows, ys, ridge=1e-3):
    """least squares with a little ridge: rows of features -> coefficients"""
    n = len(rows[0])
    a = [[sum(r[i] * r[j] for r in rows) + (ridge if i == j and i else 0) for j in range(n)] for i in range(n)]
    b = [sum(r[i] * y for r, y in zip(rows, ys)) for i in range(n)]
    for c in range(n):		# Gauss-Jordan
        piv = max(range(c, n), key=lambda r: abs(a[r][c]))
        if abs(a[piv][c]) < 1e-9:
            return None
        a[c], a[piv], b[c], b[piv] = a[piv], a[c], b[piv], b[c]
        for r in range(n):
            if r != c:
                k = a[r][c] / a[c][c]
                a[r] = [x - k * y for x, y in zip(a[r], a[c])]
                b[r] -= k * b[c]
    return [b[i] / a[i][i] for i in range(n)]


def report(day):
    path = os.path.join(OUT, day + ".jsonl")
    try:
        with open(path) as f:
            S = [json.loads(l) for l in f if l.strip()]
    except OSError:
        print("zmon: no %s" % path)
        return 1
    bat = [s for s in S if "ma" in s and not s.get("chg") and s["ma"] <= 0]
    print("zmon %s: %d samples, %d off the cable" % (day, len(S), len(bat)))
    if not bat:
        return 0

    # time split: asleep part of each interval, the rest is screen on/off awake
    t_on = sum(s["dt"] - s["slept"] for s in bat if s["screen"] == "on")
    t_off = sum(s["dt"] - s["slept"] for s in bat if s["screen"] != "on")
    t_sl = sum(s["slept"] for s in bat)
    # ma is an instant reading taken awake; asleep mA comes from QH below
    on = [-s["ma"] for s in bat if s["screen"] == "on" and s["slept"] < 1]
    off = [-s["ma"] for s in bat if s["screen"] != "on" and s["slept"] < 1]
    mean = lambda v: sum(v) / len(v) if v else 0
    h = lambda x: "%.1f h" % (x / 3600)
    print("  screen on   %-8s %4.0f mA" % (h(t_on), mean(on)))
    print("  screen off  %-8s %4.0f mA  (awake)" % (h(t_off), mean(off)))
    print("  asleep      %-8s" % h(t_sl))
    # the gauge's coulomb counter (QH, 1 mAh per step with the 5 mOhm sense
    # resistor) gives the true mean over an interval, sleep included
    q = {"on": [0, 0], "off": [0, 0], "asleep": [0, 0]}
    for a, b in zip(S, S[1:]):
        if "qh" not in a or "qh" not in b or b.get("chg") or a.get("chg") or b["qh"] > a["qh"]:
            continue
        k = "asleep" if b["slept"] > 0.8 * b["dt"] else "on" if b["screen"] == "on" else "off"
        q[k][0] += a["qh"] - b["qh"]
        q[k][1] += b["dt"]
    print("  coulomb counter: " + ", ".join("%s %.0f mA (%.1f h)" % (k, v[0] / v[1] * 3600, v[1] / 3600)
                                           for k, v in q.items() if v[1] > 600))
    soc = [s["soc"] for s in bat if "soc" in s]
    if len(soc) > 1:
        print("  battery %.1f%% -> %.1f%%" % (soc[0], soc[-1]))

    print("\n  hour  mA   on%  slept%  cpu   top")
    hours = {}
    for s in bat:
        hours.setdefault(time.localtime(s["t"]).tm_hour, []).append(s)
    for hr in sorted(hours):
        v = hours[hr]
        dt = sum(s["dt"] for s in v)
        aw = [-s["ma"] for s in v if s["slept"] < 1]
        use = {}
        for s in v:
            for n, c in s.get("top", []):
                use[n] = use.get(n, 0) + c
        top = ", ".join("%s %.0fs" % x for x in sorted(use.items(), key=lambda x: -x[1])[:3])
        print("  %02d   %4.0f  %3.0f   %3.0f   %4.2f  %s" % (
            hr, mean(aw), 100 * sum(s["dt"] - s["slept"] for s in v if s["screen"] == "on") / dt,
            100 * sum(s["slept"] for s in v) / dt, mean([s["cpu"] for s in v]), top))

    # power profile: awake samples only, mA = base + screen + bright + cpu + net
    aw = [s for s in bat if s["slept"] < 1 and "cpu" in s]
    feats = lambda s: [1, s["screen"] == "on", (s["screen"] == "on") * s.get("bright", 100) / 100,
                       s["cpu"], (sum(s["rx"]) + sum(s["tx"])) / 100]
    names = ["awake floor", "screen on", "screen x bright", "per busy core", "per 100 kB/s"]
    co = solve([feats(s) for s in aw], [-s["ma"] for s in aw]) if len(aw) > 40 else None
    if co:
        print("\n  power profile (fitted from %d awake samples):" % len(aw))
        for n, c in zip(names, co):
            print("    %-16s %5.0f mA" % (n, c))
        use, tot = {}, 0
        for s in aw:
            for n, c in s.get("top", []):
                use[n] = use.get(n, 0) + c
            tot += s["dt"]
        print("\n  processes (CPU s while awake on battery, ~mA from 'per busy core'):")
        for n, c in sorted(use.items(), key=lambda x: -x[1])[:10]:
            print("    %-22s %6.0f s  ~%4.0f mA" % (n, c, co[3] * c / tot))
    m = [s["meter"] for s in bat if "meter" in s]
    if m:
        print("\n  rail meter (mean mA, %d readings):" % len(m))
        for k in sorted({k for x in m for k in x}):
            v = [x[k] for x in m if k in x]
            print("    %-7s %5.0f" % (k, mean(v)))
    return 0


def main(argv):
    if argv[1:2] == ["run"]:
        run()
    elif argv[1:2] == ["report"]:
        return report(argv[2] if len(argv) > 2 else time.strftime("%Y%m%d"))
    elif argv[1:2] == ["now"]:
        sm = Sampler()
        sm.sample()
        time.sleep(3)
        print(json.dumps(sm.sample(), ensure_ascii=False, indent=1))
    else:
        print(__doc__.split("\n\n")[0])
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
