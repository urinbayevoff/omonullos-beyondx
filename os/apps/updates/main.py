"""Updates: ztOS's software updater. What can be updated, and installing it;
the work is done by the system (ztos-update, as root, started through systemd),
this shows how it goes.

Modified for OmonullOS, November 2026: in Uzbek; only omonullOS's own
signed updates (omonullos-update: version, what is new, install, roll back).
Arch's packages (ztos-update, pacman -Syu) are no longer offered here
(16.11, the owner: a user must not break the phone): a whole-system upgrade
would replace our kernel's initramfs and more. ztos-update itself stays.
One look (the owner, 16.11, after trying three): Metro 2026, live tiles on
black (a light one waits for a day mode)."""
import json
import math
import os
import subprocess
import sys
import time
import ztos
from PySide6.QtCore import QRectF, Qt, QTimer, QPointF
from PySide6.QtGui import QColor, QFont, QFontMetricsF, QImage, QRegion, QLinearGradient, QPainter, QPen, QRadialGradient
from PySide6.QtWidgets import QSizePolicy, QWidget

app = ztos.App()

# omonullOS (omonullos-update)
O_STATUS = "/run/omonullos-update/status.json"
O_VAR = "/var/lib/omonullos-update"
# for pictures of the app (ZTOS_SHOT): a JSON with status/installed/available/rollback
FAKE = os.environ.get("ZTOS_UPDATES_FAKE")



def read_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def start(unit):
    """Ask systemd to run one of the updater's services (allowed without a password)."""
    if FAKE:
        return True
    try:
        subprocess.Popen(["systemctl", "start", "--no-block", unit])
        return True
    except OSError:
        return False


def mb(n):
    n = int(n or 0)
    return f"{n / 1048576:.1f} MB".replace(".", ",") if n >= 1048576 else f"{max(1, n // 1024)} KB"


def rollback_to():
    """The version a rollback would bring back, or None."""
    have = read_json(os.path.join(O_VAR, "installed.json")) or {}
    base = os.path.join(O_VAR, "backup")
    try:
        names = os.listdir(base)
    except OSError:
        return None
    for d in names:
        j = read_json(os.path.join(base, d, "journal.json"))
        if j and j.get("done") and str(j.get("to_version")) == str(have.get("version")):
            return str(j.get("from_version"))
    return None


def check_failed_text(s):
    """A check that failed, in plain words (the updater says which kind)."""
    kind = s.get("kind")
    if kind == "offline":
        text = "Internetga ulanib bölmadi. Wi-Fi yoki mobil internetni tekşirib, qayta urinib köring."
    elif kind == "server":
        text = f"GitHub yangilanişlar röyxatini bermadi (xato {s.get('code', '?')}). Birozdan keyin qayta urinib köring."
    else:
        text = "Yangilanişni tekşirib bölmadi: " + s.get("msg", "")
    if int(s.get("fails", 0) or 0) >= 3:
        text += (f"\n{s['fails']} marta ketma-ket bölmadi. Internet bor bölsa ham davom etsa, "
                 "github.com/urinbayevoff/omonullos-beyondx sahifasida yangi versiyani qarang. "
                 "Batafsil: /var/log/omonullos-update.log")
    return text


def model(expect=None):
    """What the screen shows, the same for every look:
    busy, progress (None: a moving sign, a number: 0..100, -1: none), head and
    sub (words), have/avail (versions), size, notes (lines), actions (primary
    first) and rollback (the version it would bring back)."""
    if FAKE:
        f = read_json(FAKE) or {}
        s, have, avail, to = f.get("status", {}), f.get("installed", {}), f.get("available"), f.get("rollback")
    else:
        s = read_json(O_STATUS) or {}
        have = read_json(os.path.join(O_VAR, "installed.json")) or {}
        avail = read_json(os.path.join(O_VAR, "available.json"))
        to = rollback_to()
    if expect:              # asked a moment ago, not begun yet: shown as begun
        s = {**s, "state": expect}
    st = s.get("state", "idle")
    working = st in ("checking", "downloading", "verifying", "installing", "rolling-back")
    hv = have.get("version", "0")
    hv = hv if hv != "0" else "?"
    m = {"busy": working, "progress": -1, "have": hv, "avail": "", "size": "", "notes": [],
         "rollback": to or None, "actions": [], "state": st}     # stays while it works (no tile going and coming)
    if avail:
        pkg = avail.get("package", {})
        d = avail.get("delta")
        if d and str(d.get("base")) == str(have.get("version")):
            pkg = d
        m["avail"] = str(avail.get("version", ""))
        m["size"] = mb(pkg.get("size"))
    need_reboot = s.get("after", "reboot") != "none"
    can_install = bool(avail) and not avail.get("_needs_updater") and not working and st != "done"

    head, sub = "", ""
    if st in ("installing", "rolling-back") and s.get("n"):
        k, n = int(s.get("k", 0)), max(1, int(s["n"]))
        m["progress"] = 100.0 * k / n
        head = ("Örnatilmoqda" if st == "installing" else "Qaytarilmoqda") + f" {k} / {n}"
        sub = s.get("name", "")
    elif st == "checking":
        m["progress"] = None
        head, sub = "Yangilaniş qidirilmoqda...", f"Hozirgi versiya: omonullOS {hv}"
    elif st in ("downloading", "verifying"):
        m["progress"] = None
        head = f"omonullOS {m['avail']} yuklanmoqda" if st == "downloading" else "Imzo tekşirilmoqda..."
        sub = (f"{m['size']} · telefonni işlataveriň" if st == "downloading"
               else "Imzo va har bir fayl tekşirilmoqda")
    elif st == "done":
        head = f"omonullOS {s.get('version', '')} örnatildi"
        sub = "Telefonni qayta yoqing, yangi versiya şunda işlaydi." if need_reboot else "Hammasi tayyor."
        if s.get("kept"):
            sub += ("\nSiz özgartirgan fayllarga tegilmadi, yangisi yonida .new: "
                    + ", ".join("/" + k for k in s["kept"]))
    elif st == "failed" and s.get("what") == "check":
        head, sub = "Tekşirib bölmadi", check_failed_text(s)
    elif st == "failed":
        head, sub = "Bölmadi", s.get("msg", "") + "\nBatafsil: /var/log/omonullos-update.log"
    elif st == "rolled-back":
        head = "Oldingi versiyaga qaytildi"
        sub = ("Yangilanişdan keyin telefon yaxşi yuklanmadi, şuning uçun özi qaytdi." if s.get("auto")
               else ("Telefonni qayta yoqing." if need_reboot else ""))
    elif can_install or st == "available":
        head = f"omonullOS {m['avail']} bor"
        sub = f"Hajmi {m['size']}. Örnatilgandan keyin telefon qayta yoqiladi."
    elif st == "badsig":
        head, sub = "Imzo notoğri", "Topilgan yangilaniş imzosi notoğri, örnatilmaydi."
    elif st == "refused":
        head, sub = "Bu telefon uçun emas", "Topilgan yangilaniş bu telefon uçun emas, örnatilmaydi."
    elif st == "needs-updater":
        head, sub = "Avval yangilaş dasturi", "Bu yangilaniş uçun avval yangilaş dasturining özini yangilaş kerak."
    elif st == "kernel":
        head, sub = "Boşqa yadro uçun", "Topilgan yangilaniş boşqa yadro uçun, örnatilmaydi (avval BOOT yangilanişi kerak)."
    elif st == "uptodate":
        head, sub = "Eng yangi versiya", f"omonullOS {hv} örnatilgan."
    else:
        head, sub = "Yangi versiya bormi?", f"Hozirgi versiya: omonullOS {hv}. Tekşirib köring."
    m["head"], m["sub"] = head, sub

    if st in ("done", "rolled-back") and need_reboot:
        m["actions"].append(("reboot", "qayta yoqiş"))
    if can_install:
        m["actions"].append(("install", "örnatiş"))
    if not working and st not in ("done",):
        m["actions"].append(("check", "qayta urinib köriş" if st == "failed" else "tekşiriş"))
    notes = (avail or have).get("notes", "") if (avail or st == "done") else ""
    m["notes"] = [ln.strip().lstrip("•").strip() for ln in notes.splitlines() if ln.strip()]
    return m


# ---- drawing --------------------------------------------------------------------------

def font(families, size):
    f = QFont()
    f.setFamilies(families)
    f.setPixelSize(max(1, round(size)))
    f.setHintingPreference(QFont.PreferNoHinting)
    return f


LIGHT, SEMILIGHT, REGULAR, SEMIBOLD, SYMBOL = (ztos.FONT_LIGHT, ztos.FONT_SEMILIGHT, ztos.FONT_REGULAR,
                                              ztos.FONT_SEMIBOLD, ztos.FONT_SYMBOLIC)
G_SYNC, G_REFRESH, G_ACCEPT, G_RESTART, G_UNDO, G_LOOK, G_NEXT, G_MENU, G_LIST = (
    "", "", "", "", "", "", "", "", "")


MEASURED = {}       # (text, font, size, width, flags) -> height (see Skin.text)


def mix(a, b, t):
    return QColor(round(a.red() + (b.red() - a.red()) * t), round(a.green() + (b.green() - a.green()) * t),
                  round(a.blue() + (b.blue() - a.blue()) * t), round(a.alpha() + (b.alpha() - a.alpha()) * t))


def alpha(c, a):
    c = QColor(c)
    c.setAlphaF(a)
    return c


# the toolkit's motion (the runtime's numbers, at the speed set in Settings)
_dur = getattr(ztos, "_dur", lambda ms: ms / 0.75)
TILT_IN = getattr(ztos, "TILT_IN_MS", _dur(70)) / 1000
TILT_OUT = getattr(ztos, "TILT_OUT_MS", _dur(160)) / 1000
TILT_DEG, TILT_LIFT = getattr(ztos, "TILT_DEG", 14.0), getattr(ztos, "TILT_LIFT", 40.0)
PRESS_SINK, FOV = getattr(ztos, "PRESS_SINK", 0.045), getattr(ztos, "DEPTH_FOV_DEG", 34.0)
GONE = getattr(ztos, "ROW_OUT_MS", _dur(110)) / 1000        # a piece that goes: fades and sinks
STAGGER = getattr(ztos, "ROW_STAGGER_MS", _dur(33)) / 1000  # new pieces come one after another
COME, MOVE = _dur(240) / 1000, _dur(300) / 1000             # rise into place; glide to a new place
STAGGER_MOST = 8
BAND_ROWS = 1024        # pixel rows per GPU band (see Home.own_picture)


def ease(t):
    """Out, cubic: quick, then settling."""
    t = 0.0 if t <= 0 else 1.0 if t >= 1 else t
    return 1 - (1 - t) ** 3


def piece(p, img, src, dst):
    """Part `src` of a picture (in points) laid at `dst`."""
    d = img.devicePixelRatio()
    p.drawImage(dst, img, QRectF(src.x() * d, src.y() * d, src.width() * d, src.height() * d))


def fade(p, img, src, x, y, a):
    """Like blit, see-through: drawn the scaled way, which Qt blends at half
    the cost of a plain see-through copy (8 vs 17 ms for a big tile)."""
    d = img.devicePixelRatio()
    p.setOpacity(a)
    p.setRenderHint(QPainter.SmoothPixmapTransform, True)
    p.drawImage(QRectF(round(x * d) / d, round(y * d) / d, src.width(), src.height()), img,
                QRectF(src.x() * d, src.y() * d, src.width() * d, src.height() * d))
    p.setRenderHint(QPainter.SmoothPixmapTransform, False)
    p.setOpacity(1.0)


def blit(p, img, src, x, y):
    """Part `src` laid unscaled with its corner at (x, y), snapped to a whole
    pixel: a plain copy (a scaled or half-pixel one is filtered, and that cost
    20-40 ms a frame at the phone's 4 pixels a point)."""
    d = img.devicePixelRatio()
    sx, sy = round(src.x() * d), round(src.y() * d)
    p.drawImage(QPointF(round(x * d) / d, round(y * d) / d), img,
                QRectF(sx, sy, round(src.right() * d) - sx, round(src.bottom() * d) - sy))


class Skin(QWidget):
    """One look. draw() lays everything out from the top; with no painter it
    only measures (the widget's height), with one it paints. Taps go to the
    rectangles draw() registered."""
    name = ""
    bg = QColor(0, 0, 0)
    animated = True

    def __init__(self, home):
        super().__init__()
        self.home = home
        self._motion = True         # its paints are the dots and bars, not a change of the list
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        self.m = model()
        self.p = None
        self.hits = []
        self.pressed = None
        self.press_at = None
        self.t0 = time.monotonic()
        self.acc = ztos.accent()
        # what does not move is drawn once into a picture (16.11: a whole
        # paint every frame took 40-140 ms on a big core, the app lagged);
        # each frame only the moving spots are drawn over it
        self.cache = None
        self.cache_key = None
        self.spots = []
        self.static = False
        self.press_tint = QColor(255, 255, 255, 56)
        # motion (16.11, the owner: a press was not felt, a change jumped):
        # the pieces draw() names glide, come and go when what is shown
        # changes; the thing pressed leans toward the finger as on Start
        self.blocks, self.order = {}, []
        self.ops, self.kept, self.sigs = None, [], {}
        self.h = 640
        self.pictures = []          # (state, picture, spots, signatures, pieces): the last two
        self.change = None
        self.lean = None
        self.frames = QTimer(self, interval=16)
        self.frames.setTimerType(Qt.PreciseTimer)
        self.frames.timeout.connect(self.frame_step)

    # ---- canvas: measure or paint. Each thing drawn is also written down
    # (self.ops, while laying out): what a piece is drawn from, so a new
    # picture can take the pieces that did not change from the last one
    # instead of drawing them again (see picture)
    def op(self, kind, r, *args):
        if self.ops is not None:
            self.ops.append((kind, QRectF(r), args))
        return self.p is not None and not any(k.contains(r) for k in self.kept)

    def text(self, x, y, w, s, fam, size, color, flags=Qt.TextWordWrap, lh=None):
        f = font(fam, size)
        # measured once per text, font and width (laying out the 38 lines of
        # «nima yangi» again at every change took 20-50 ms)
        mk = (s, tuple(fam), size, round(w, 2), str(flags))
        hgt = MEASURED.get(mk)
        if hgt is None:
            if len(MEASURED) > 4000:
                MEASURED.clear()
            hgt = MEASURED[mk] = QFontMetricsF(f).boundingRect(QRectF(0, 0, w, 100000), flags, s).height()
        r = QRectF(x, y, w, hgt)
        where = QRectF(x, y, w, r.height() + 4)
        if s and self.op("t", where, s, tuple(fam), size, color.rgba(), str(flags)):
            self.p.setFont(f)
            self.p.setPen(color)
            self.p.drawText(where, flags, s)
        return r.height() if s else 0.0

    def fill(self, r, color, radius=0.0):
        if not self.op("f", r, color.rgba(), radius):
            return
        self.p.setPen(Qt.NoPen)
        self.p.setBrush(color)
        if radius:
            self.p.drawRoundedRect(r, radius, radius)
        else:
            self.p.drawRect(r)

    def frame(self, r, color, width, radius=0.0):
        if not self.op("r", r, color.rgba(), width, radius):
            return
        self.p.setPen(QPen(color, width))
        self.p.setBrush(Qt.NoBrush)
        h = width / 2
        rr = r.adjusted(h, h, -h, -h)
        if radius:
            self.p.drawRoundedRect(rr, radius, radius)
        else:
            self.p.drawRect(rr)

    def glyph(self, r, g, size, color, angle=0.0):
        if not self.op("g", r, g, size, color.rgba(), angle):
            return
        self.p.save()
        self.p.translate(r.center())
        if angle:
            self.p.rotate(angle)
        self.p.setFont(font(SYMBOL, size))
        self.p.setPen(color)
        self.p.drawText(QRectF(-r.width() / 2, -r.height() / 2, r.width(), r.height()), Qt.AlignCenter, g)
        self.p.restore()

    def signatures(self):
        """Each piece: its size and what is drawn in it, from its own corner.
        Two pieces with one signature look the same."""
        out = {}
        for k, r in self.blocks.items():
            ox, oy = r.x(), r.y()
            out[k] = (round(r.width(), 2), round(r.height(), 2)) + tuple(
                (kind, round(o.x() - ox, 2), round(o.y() - oy, 2), round(o.width(), 2), round(o.height(), 2), args)
                for kind, o, args in self.ops if o.intersects(r))
        return out

    def hit(self, r, key):
        """A tap target (and a piece that moves as one); pressed it leans."""
        self.hits.append((QRectF(r), key))
        self.block(key, r)
        return False

    def block(self, key, r):
        """A piece of the screen that moves as one when what is shown changes."""
        r = QRectF(r)
        if key in self.blocks:
            self.blocks[key] = self.blocks[key].united(r)
        else:
            self.blocks[key] = r
            self.order.append(key)

    def spot(self, kind, r, **kw):
        """Something that moves: drawn every frame over the still picture."""
        if self.p is not None and self.static:
            self.spots.append((kind, QRectF(r), kw))

    def spots_rect(self):
        out = QRectF()
        for _, r, _ in self.spots:
            out = out.united(r.adjusted(-6, -6, 6, 6))
        return out.toAlignedRect()

    def moving(self, kind, r, kw):
        p, t = self.p, self.now()
        if kind == "dots":          # five dots crossing from the left, slow through the middle (WP's)
            t %= 3.4
            for i in range(5):
                u = (t - i * 0.12) / 2.4
                if 0 <= u <= 1:
                    x = 0.5 + 0.5 * (2 * u - 1) ** 3
                    self.fill(QRectF(r.x() + x * r.width() - 2, r.y() + 2, 4, 4), kw["color"])
        elif kind == "ring":        # five dots round a circle, bunching and spreading (W10M's)
            c, rad = r.center(), kw["rad"]
            p.setPen(Qt.NoPen)
            for i in range(5):
                u = ((t - i * 0.16) % 2.0) / 2.0
                a = 360 * (u - math.sin(2 * math.pi * u) / (2 * math.pi) * 0.9) - 90
                p.setBrush(alpha(kw["color"], 1 - i * 0.12))
                p.drawEllipse(QPointF(c.x() + rad * math.cos(math.radians(a)),
                                      c.y() + rad * math.sin(math.radians(a))), 3.2, 3.2)
        elif kind == "spin":        # a sign turning
            self.glyph(r, kw["g"], kw["size"], kw["color"], angle=(t * kw["speed"]) % 360)
        elif kind == "shimmer":     # a gleam running along a bar
            s = (t % 1.6) / 1.6
            sh = QLinearGradient(r.x() + r.width() * (s - 0.3), 0, r.x() + r.width() * s, 0)
            sh.setColorAt(0, QColor(255, 255, 255, 0))
            sh.setColorAt(1, QColor(255, 255, 255, 90))
            p.setPen(Qt.NoPen)
            p.setBrush(sh)
            p.drawRoundedRect(r, 3, 3)

    def now(self):
        return time.monotonic() - self.t0

    # ---- size, paint, taps
    def relayout(self):
        self.cache = None
        self.home.cancel_bands()        # bands of the old picture must not go up as this one's
        self.measure()
        self.setFixedHeight(self.h)

    def measure(self):
        """Lay out self.m: the pieces, the taps, the signatures, the height."""
        self.hits, self.blocks, self.order = [], {}, []
        self.p = None
        self.ops = []
        h = self.draw(self.width() or 360)
        self.sigs = self.signatures()
        self.ops = None
        view = self.home.viewport.height() if self.home.viewport.height() > 0 else 640
        self.h = int(max(h, view))

    def prerender(self, m):
        """The picture of `m` made now, while nothing moves, and kept: the
        change to it then starts at once (16.11: «tekşiriş» drew its picture
        when pressed, 100-150 ms with the button's motion stopped)."""
        saved = (self.m, self.hits, self.blocks, self.order, self.sigs, self.h,
                 self.cache, self.cache_key, self.spots)
        try:
            self.m = m
            self.measure()
            if all(e[0] != self.state() for e in self.pictures):
                self.cache = None
                self.picture()
        finally:
            (self.m, self.hits, self.blocks, self.order, self.sigs, self.h,
             self.cache, self.cache_key, self.spots) = saved

    def resizeEvent(self, event):
        # laid out again for its real width: the page's own resize can come
        # before the list gives this its width (16.11: measured at 100 points
        # wide, a long "nima yangi" made the page 5500 points tall, mostly empty)
        if event.oldSize().width() != event.size().width():
            self.relayout()
        super().resizeEvent(event)

    def state(self):
        """What the picture shows: the same state, the same picture."""
        return (repr(self.m), self.width(), self.h, self.devicePixelRatioF())

    def picture(self):
        """Everything that does not move, drawn once. The last two are kept
        (a state and back again, «qidirilmoqda» and the list: each drawn once,
        100-200 ms at 4 pixels a point, not at every change)."""
        dpr = self.devicePixelRatioF()
        key = self.state()
        if self.cache is not None and self.cache_key == key:
            return self.cache
        for k, img, spots, _, _ in self.pictures:
            if k == key:
                self.cache, self.cache_key, self.spots = img, key, list(spots)
                return img
        img = QImage(max(1, round(self.width() * dpr)), max(1, round(self.h * dpr)),
                     QImage.Format_ARGB32_Premultiplied)
        img.setDevicePixelRatio(dpr)
        q = QPainter(img)
        sigs, blocks = dict(self.sigs), dict(self.blocks)
        # made from the last picture: it is laid down whole (one copy), what
        # moved is copied to its new place, only what changed is drawn (16.11:
        # after «tekşiriş» the 38 lines of «nima yangi» were drawn anew,
        # 110 ms, though only the big tile changed)
        last = self.pictures[-1] if self.pictures else None
        self.kept = []
        full = QRegion(0, 0, self.width(), self.h)
        if last is not None and last[1].devicePixelRatio() == dpr and last[1].width() == img.width():
            _, old, _, old_sigs, old_blocks = last
            q.setCompositionMode(QPainter.CompositionMode_Source)   # copies, not blends: 55 -> few ms
            q.drawImage(QPointF(0, 0), old)
            still, moved = [], []
            for k, r in blocks.items():
                if k in old_sigs and old_sigs[k] == sigs[k]:
                    ro = old_blocks[k]
                    (still if (ro.x(), ro.y()) == (r.x(), r.y()) else moved).append((ro, r))
            stale = full
            for _, r in still:
                stale = stale.subtracted(QRegion(r.toAlignedRect()))
            q.setClipRegion(stale)
            q.fillRect(QRectF(0, 0, self.width(), self.h), self.bg)
            for ro, r in moved:
                blit(q, old, ro, r.x(), r.y())
            q.setCompositionMode(QPainter.CompositionMode_SourceOver)
            self.kept = [r for _, r in still + moved]
            clip = full
            for r in self.kept:
                clip = clip.subtracted(QRegion(r.toAlignedRect()))
            q.setClipRegion(clip)
        else:
            img.fill(self.bg)
        q.setRenderHints(QPainter.Antialiasing | QPainter.TextAntialiasing)
        self.p, self.static, self.hits, self.spots = q, True, [], []
        self.blocks, self.order = {}, []
        self.draw(self.width())
        self.p, self.static, self.kept = None, False, []
        q.end()
        self.cache, self.cache_key = img, key
        self.pictures = [e for e in self.pictures if e[0] != key][-1:] + [(key, img, list(self.spots), sigs, blocks)]
        return self.cache

    # ---- a change: what stays glides to its new place, what goes fades,
    # what is new rises in after, one after another (WP's)
    def change_to(self, m):
        old = self.picture() if self.width() > 0 and self.isVisible() else None
        before, before_sigs = dict(self.blocks), dict(self.sigs)
        self.m = m
        self.relayout()
        if old is not None:
            self.picture()      # the new picture first: the motion's clock starts after it
        self.begin(old, before, before_sigs)

    def begin(self, old, before, before_sigs=None):
        new = [k for k in self.order if k not in before]
        # which staying pieces look the same (they only move): by what they
        # are drawn from, not by their pixels (that took 56-400 ms)
        alike = {k: before_sigs.get(k) == self.sigs.get(k) for k in self.blocks
                 if old is not None and before_sigs and k in before}
        self.change = {"img": old, "blocks": before, "t0": time.monotonic(), "new": new, "same": alike,
                       "dur": max(MOVE, GONE * 0.6 + STAGGER * min(len(new), STAGGER_MOST) + COME)}
        self.frames.start()
        self.update()

    def paint_change(self, p, now):
        c = self.change
        el, old, new, img_o, img_n = now - c["t0"], c["blocks"], self.blocks, c["img"], self.cache
        if img_o is not None:
            for k, r in old.items():
                if k in new:
                    continue
                u = ease(el / GONE)
                if u < 1:           # gone: fades, slipping down a little
                    fade(p, img_o, r, r.x(), r.y() + 12 * u, 1 - u)
        for k in self.order:
            r = new[k]
            if k in old and img_o is not None:
                ro, v = old[k], ease(el / MOVE)
                x, y = ro.x() + (r.x() - ro.x()) * v, ro.y() + (r.y() - ro.y()) * v
                blit(p, img_n, r, x, y)
                if v < 1 and not c["same"].get(k, False):     # it changed: its old look fades off on top
                    fade(p, img_o, ro, x, y, 1 - v)
            else:
                # one after another, the first few: the rest (off the screen,
                # mostly) together, else 35 lines took 1.9 s
                i = min(c["new"].index(k), STAGGER_MOST) if k in c["new"] else 0
                v = ease((el - GONE * 0.6 - STAGGER * i) / COME)
                if v >= 1:
                    blit(p, img_n, r, r.x(), r.y())
                elif v > 0:
                    fade(p, img_n, r, r.x(), r.y() + 36 * (1 - v), v)

    # ---- a press: the thing leans toward the finger and sinks a little
    # (the runtime's tilt: the flat picture laid down in small cells, each
    # where its corners fall seen from in front)
    def lean_amount(self, now):
        L = self.lean
        if L is None:
            return 0.0
        down = 1 - (1 - min(1.0, (now - L["down"]) / TILT_IN)) ** 2
        if L["up"] is None:
            return down
        start = max(L["up"], L["down"] + TILT_IN)       # a quick tap still goes all the way in
        if now < start:
            return down
        t = (now - start) / TILT_OUT
        return 0.0 if t >= 1 else 1 - (1 - (1 - t) ** 2)

    def paint_lean(self, p, img, amount):
        L = self.lean
        r = L["r"]
        w, h = r.width(), r.height()
        eye = (self.home.viewport.height() or 640) / 2 / math.tan(math.radians(FOV / 2))
        most = math.radians(TILT_DEG)
        ay = min(most, math.asin(min(1.0, TILT_LIFT / (w / 2))))
        ax = min(most, math.asin(min(1.0, TILT_LIFT / (h / 2))))
        ry, rx = (L["fx"] - 0.5) * 2 * ay * amount, (L["fy"] - 0.5) * 2 * ax * amount
        z0 = -PRESS_SINK * eye * amount
        sy, cy, sx, cx = math.sin(ry), math.cos(ry), math.sin(rx), math.cos(rx)

        def at(X, Y):
            den = eye - z0 + X * sy + Y * sx
            return r.x() + w / 2 + eye * cy * X / den, r.y() + h / 2 + eye * cx * Y / den
        p.fillRect(r.adjusted(-1, -1, 1, 1), self.bg)
        p.setRenderHint(QPainter.SmoothPixmapTransform, True)
        nx, ny = 10, 3
        for i in range(nx):
            for j in range(ny):
                X0, X1 = (i / nx - 0.5) * w, ((i + 1) / nx - 0.5) * w
                Y0, Y1 = (j / ny - 0.5) * h, ((j + 1) / ny - 0.5) * h
                pts = (at(X0, Y0), at(X1, Y0), at(X0, Y1), at(X1, Y1))
                left, right = min(q[0] for q in pts), max(q[0] for q in pts)
                top, bottom = min(q[1] for q in pts), max(q[1] for q in pts)
                piece(p, img, QRectF(r.x() + X0 + w / 2, r.y() + Y0 + h / 2, X1 - X0, Y1 - Y0),
                      QRectF(left - 0.25, top - 0.25, right - left + 0.5, bottom - top + 0.5))

    def frame_step(self):
        now = time.monotonic()
        busy = False
        if self.change is not None:
            if now - self.change["t0"] < self.change["dur"]:
                busy = True
            else:
                self.change = None
                self.home._changed(explicit=True)   # still: the page's pictures are made anew,
                # at once (not 0.4 s later, when a finger is often already
                # swiping: the list then moved the slow CPU way); but not while
                # a sign turns («qidirilmoqda»): the bands going up stopped it
                # for a moment each, so then they go up when a finger comes
                if not self.spots:
                    QTimer.singleShot(0, self.home._gl_prepare)
                self.home.ahead_timer.start()
            self.update()
        L = self.lean
        if L is not None:
            if L["up"] is not None:
                start = max(L["up"], L["down"] + TILT_IN)
                if now >= start + TILT_OUT:
                    self.lean = None
            busy = busy or self.lean is not None
            self.update(L["r"].toAlignedRect().adjusted(-4, -4, 4, 4))
        if not busy:
            self.frames.stop()

    def paintEvent(self, event):
        img = self.picture()
        dpr = img.devicePixelRatio()
        er = QRectF(event.rect())
        now = time.monotonic()
        p = QPainter(self)
        if self.change is not None:
            p.fillRect(er, self.bg)
            self.paint_change(p, now)
            p.setOpacity(ease((now - self.change["t0"]) / MOVE))    # the moving signs fade in
        else:
            p.drawImage(er, img, QRectF(er.x() * dpr, er.y() * dpr, er.width() * dpr, er.height() * dpr))
        self.p = p
        for kind, r, kw in self.spots:
            if r.intersects(er):
                self.moving(kind, r, kw)
        self.p = None
        p.setOpacity(1.0)
        if self.lean is not None:
            amount = self.lean_amount(now)
            if amount > 0.001:
                self.paint_lean(p, img, amount)
        p.end()

    def mousePressEvent(self, e):
        self.press_at = e.position()
        for r, key in self.hits:
            if r.contains(e.position()):
                self.pressed = key
                self.lean = {"r": QRectF(r), "down": time.monotonic(), "up": None,
                             "fx": (e.position().x() - r.x()) / max(1.0, r.width()),
                             "fy": (e.position().y() - r.y()) / max(1.0, r.height())}
                self.frames.start()
                self.update(r.toAlignedRect().adjusted(-4, -4, 4, 4))
                return

    def mouseMoveEvent(self, e):
        if self.pressed and self.press_at is not None and (e.position() - self.press_at).manhattanLength() > 12:
            self.pressed = None
            if self.lean is not None and self.lean["up"] is None:
                self.lean["up"] = time.monotonic()      # a drag, not a tap: it comes back up

    def mouseReleaseEvent(self, e):
        key, self.pressed = self.pressed, None
        if self.lean is not None and self.lean["up"] is None:
            self.lean["up"] = time.monotonic()
            if key and any(r.contains(e.position()) and k == key for r, k in self.hits):
                QTimer.singleShot(0, lambda: self.home.act(key))    # at once; it rises meanwhile

    # ---- pieces every look uses
    def bottom_block(self, w, y, view_h, block_h):
        """Where a block that sits at the bottom of the screen starts."""
        return max(y, view_h - block_h)


# ---- the look: Metro 2026, live tiles on black (a light one waits for a day mode) ---------

class Tiles(Skin):
    name = "tiles"
    bg = QColor(0, 0, 0)
    M = 16

    def __init__(self, home):
        super().__init__(home)
        self.press_tint = QColor(255, 255, 255, 56)

    def draw(self, w):
        m, M = self.m, self.M
        ink, soft, white = QColor(255, 255, 255), QColor(153, 153, 153), QColor(255, 255, 255)
        y = 14
        y += self.text(M, y, w, "OMONULLOS", SEMIBOLD, 12.5, self.acc, Qt.AlignLeft) + 2
        y += self.text(M - 2, y, w, "yangilanişlar", LIGHT, 42, ink, Qt.AlignLeft) + 12
        self.block("title", QRectF(0, 10, w, y - 12))
        cw = w - 2 * M
        prog = m["progress"]
        if isinstance(prog, float) and prog >= 0:
            # a big number, and squares that light up one by one
            f = font(LIGHT, 86)
            num = f"{prog:.0f}"
            self.text(M - 4, y - 14, cw, num, LIGHT, 86, self.acc, Qt.AlignLeft)
            nw = QFontMetricsF(f).horizontalAdvance(num)
            self.text(M - 4 + nw + 2, y + 50, 60, "%", LIGHT, 28, self.acc, Qt.AlignLeft)
            self.block("num", QRectF(0, y - 14, w, 104))
            y += 92
            h = self.text(M, y, cw, m["head"], SEMILIGHT, 19, ink)
            self.block("head", QRectF(0, y, w, h + 2))
            y += h + 2
            h = self.text(M, y, cw, m["sub"], REGULAR, 13.5, soft)
            if h:
                self.block("sub", QRectF(0, y, w, h + 2))
            y += h + 12
            tile = QRectF(M, y, cw, 74)
            self.block("big", tile)
            self.fill(tile, self.acc)
            n, cols = 39, 13
            gap, sw = 3, (cw - 24 - 12 * 3) / 13
            on = prog / 100 * n
            for k in range(n):
                cx, cy = M + 12 + (k % cols) * (sw + gap), y + 14 + (k // cols) * (14 + gap)
                c = white if k + 1 <= on else (QColor(255, 255, 255, 160) if k < on else QColor(255, 255, 255, 64))
                self.fill(QRectF(cx, cy, sw, 14), c)
            y += 74 + 12
        else:
            # the big tile: the version, or the sign turning while it works
            tile = QRectF(M, y, cw, 132)
            self.block("big", tile)
            self.fill(tile, self.acc)
            if m["busy"]:
                self.spot("spin", QRectF(M + 10, y + 10, 44, 44), g=G_SYNC, size=28, color=white, speed=200)
                self.text(M + 14, y + 62, cw - 28, m["head"], SEMILIGHT, 21, white)
                # (only the turning sign: the running squares under it went, 16.11, the owner)
            else:
                big = m["avail"] if (m["avail"] and m["state"] != "uptodate") else m["have"]
                self.text(M + 12, y + 4, cw - 24, big, LIGHT, 64, white, Qt.AlignLeft)
                self.text(M + 14, y + 80, cw - 28, m["head"], SEMILIGHT, 17, white, Qt.AlignLeft)
                self.text(M + 14, y + 108, cw - 28, "omonullOS", REGULAR, 12.5, white, Qt.AlignLeft)
            y += 132 + 10
            if m["sub"]:
                h = self.text(M, y, cw, m["sub"], REGULAR, 13.5, soft)
                self.block("sub", QRectF(0, y, w, h + 2))
                y += h + 10
        # small tiles: the version on the phone, roll back
        tw = (cw - 8) / 2
        smalls = [("have", m["have"], "hozirgi versiya", QColor(38, 38, 38), white)]
        if m["rollback"]:
            smalls.append(("rollback", G_UNDO, f"orqaga: {m['rollback']}", QColor(38, 38, 38), ink))
        for i, (key, big, label, bgc, fg) in enumerate(smalls):
            r = QRectF(M + (i % 2) * (tw + 8), y + (i // 2) * (tw * 0.62 + 8), tw, tw * 0.62)
            down = self.hit(r, key) if key != "have" else self.block(key, r)
            self.fill(r, mix(bgc, QColor(255, 255, 255), 0.15) if down else bgc)
            if key == "have":
                self.text(r.x() + 10, r.y() + 2, tw - 20, big, LIGHT, 38, fg, Qt.AlignLeft)
            else:
                self.glyph(QRectF(r.x() + 8, r.y() + 8, 32, 32), big, 22, fg)
            self.text(r.x() + 10, r.bottom() - 24, tw - 20, label, REGULAR, 12.5, fg, Qt.AlignLeft)
        y += ((len(smalls) + 1) // 2) * (tw * 0.62 + 8) + 8
        if m["notes"]:
            y += 6
            for i, line in enumerate(m["notes"]):
                y0 = y
                self.fill(QRectF(M, y + 6, 9, 9), self.acc)
                y += self.text(M + 18, y, cw - 18, line, REGULAR, 14, ink) + 8
                self.block(f"note{i}", QRectF(0, y0, w, y - y0))
            y += 6
        # big buttons at the bottom, the main one last (nearest the thumb)
        acts = m["actions"]
        block = len(acts) * 62 + 14
        view = self.home.viewport.height() or 640
        y = self.bottom_block(w, y + 6, view, block)
        for i, (key, label) in enumerate(reversed(acts)):
            main = i == len(acts) - 1
            r = QRectF(M, y, cw, 52)
            down = self.hit(r, key)
            if main:
                self.fill(r, mix(self.acc, QColor(0, 0, 0), 0.25) if down else self.acc)
                self.text(r.x(), r.y() + 13, cw, label, SEMIBOLD, 19, white, Qt.AlignHCenter)
            else:
                self.fill(r, QColor(51, 51, 51) if down else self.bg)
                self.frame(r, ink, 2)
                self.text(r.x(), r.y() + 13, cw, label, SEMIBOLD, 19, ink, Qt.AlignHCenter)
            y += 62
        return y + 6


class Home(ztos.Page):
    def __init__(self):
        super().__init__("", scroll=True, header=False)
        self.list.setContentsMargins(0, 0, 0, 0)
        self.uploading = False  # GPU bands going up (see own_picture)
        self.band_gen = 0
        self.ahead_timer = QTimer(self, singleShot=True, interval=800)
        self.ahead_timer.timeout.connect(self.ahead)
        self.band_sets = []     # (state, keys, spans, top, height): the last two on the GPU
        self.bands = None
        self.skin = None
        self.skin = Tiles(self)
        ztos.set_background(self.skin.bg)
        self.add(self.skin)
        self.skin.relayout()
        self.timer = QTimer(self, interval=700)
        self.timer.timeout.connect(self.refresh)
        self.anim = QTimer(self, interval=40)
        self.anim.timeout.connect(self.tick)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if self.skin is not None:
            self.skin.relayout()

    def _gl_prepare(self, force=False):
        # the page's GPU picture (120-170 ms to make) waits until the look's
        # own motion is over: made in the middle of it, it stopped it dead
        if not force and self.skin is not None and self.skin.change is not None:
            self._gl_idle.start()
            return
        if not force and self.skin is not None and self.skin.spots:
            return      # a sign turning: the picture goes up when a finger comes (see _gl_begin)
        if not self.own_picture(force):
            super()._gl_prepare(force)

    def cancel_bands(self):
        """The look's picture changed: bands of the old one stop going up."""
        self.band_gen += 1
        self.uploading = False

    def own_picture(self, force):
        """The runtime's _gl_prepare, the quick way: the page is only the look,
        and the look keeps its whole picture already, at the screen's pixels.
        So that goes to the GPU as it is, instead of the list drawn again into
        a new picture and a second small one for checks (120-215 ms on the
        phone each time). It goes up in bands of 1024 rows, one per turn of
        the event loop (the whole at once, 1440x7580, took 105 ms with the
        finger waiting), and the bands of the last two states stay on the GPU
        (back to one of them: nothing goes up). No checks needed: the look
        says itself when it changes. False: not this case, the runtime does it."""
        layer = ztos._layer_of(self)
        if layer is None or not self.scrolls or self._max() <= 0 or not self.isVisible():
            return True
        if self._gl_on and not force:
            return True
        if not force and ztos._animating():
            self._gl_idle.start()
            return True
        if not force and not self._gl_dirty and layer.has((id(self), "list")) and self._gl_covers(self._scroll):
            return True
        sk = self.skin
        ratio = self.devicePixelRatioF()
        others = [w for w in self.body.findChildren(QWidget, options=Qt.FindDirectChildrenOnly)
                  if w is not sk and w.isVisible()]
        if sk is not None and sk.change is not None and not force:
            self._gl_idle.start()       # after the look's own motion
            return True
        if force or sk is None or others or sk.y() != 0 \
                or sk.height() != self.body.height() or sk.width() != self.viewport.width():
            self.bands = None           # the runtime's one picture, its quads
            return False
        img = sk.picture()
        if abs(img.devicePixelRatio() - ratio) > 1e-6:
            self.bands = None
            return False
        view, body_h = float(self.viewport.height()), float(self.body.height())
        state = sk.state()
        for st, keys, spans, top, height in self.band_sets:
            if st == state and top <= max(0.0, self._scroll) and \
                    min(self._scroll + view, body_h) <= top + height and not sk.spots:
                self.bands = (keys, spans)  # this state's bands are up already
                self._gl_top, self._gl_h = top, height
                self._print_top, self._print_img = None, None
                self._gl_dirty = False
                return True
        if not layer.has((id(self), "list")):    # the runtime asks for its key: a dot
            dot = QImage(1, 1, QImage.Format_ARGB32_Premultiplied)
            dot.fill(sk.bg)
            layer.upload((id(self), "list"), dot)
        if sk.spots:                    # what moves (a turning sign) goes up as it is now
            img = img.copy()
            q = QPainter(img)
            q.setRenderHints(QPainter.Antialiasing | QPainter.TextAntialiasing)
            sk.p = q
            for kind, r, kw in sk.spots:
                sk.moving(kind, r, kw)
            sk.p = None
            q.end()
        height = float(round(min(layer.max_texture / ratio, body_h)))
        top = float(round(max(0.0, min(self._scroll - (height - view) / 2, body_h - height))))
        y0, rows = round(top * ratio), min(round(height * ratio), img.height() - round(top * ratio))
        parts = [(y0 + r, min(BAND_ROWS, rows - r)) for r in range(0, rows, BAND_ROWS)]
        self.band_gen += 1
        gen = self.band_gen
        keys = [(id(self), "band", gen, i) for i in range(len(parts))]
        self._gl_dirty = True           # until every band is up
        self.uploading = True

        def up(i):
            if gen != self.band_gen or self._gl_on:
                for k in keys[:i]:
                    layer.drop(k)
                if gen == self.band_gen:
                    self.uploading = False
                return                  # changed again, or moving: begun anew later
            py, n = parts[i]
            layer.upload(keys[i], img.copy(0, py, img.width(), n))
            if i + 1 < len(parts):
                QTimer.singleShot(0, lambda: up(i + 1))
                return
            spans = [(py / ratio, n / ratio) for py, n in parts]
            self.band_sets = [e for e in self.band_sets if e[0] != state] + [(state, keys, spans, top, height)]
            while len(self.band_sets) > 2:
                for k in self.band_sets.pop(0)[1]:
                    layer.drop(k)
            self.bands = (keys, spans)
            self._gl_top, self._gl_h = top, height
            self._print_top, self._print_img = None, None
            self._gl_dirty = False
            self.uploading = False
        up(0)
        return True

    def _gl_begin(self):
        # a finger on the list while its GPU picture is out of date: the
        # runtime waits for the list to be still 0.4 s (each move put it off
        # again, so a swipe right after a change, or swipe after swipe, went
        # the slow CPU way to the end: 410 of 561 moves). Here the bands go up
        # at once, between the moves, and the swipe goes onto the GPU as soon
        # as they are there, in the same gesture. While the look's own motion
        # runs, the list stays off the GPU (the picture there would be still).
        if self.skin is not None and self.skin.change is not None:
            return
        if self._gl_dirty and not self._gl_on and not self.uploading:
            self.own_picture(False)
        super()._gl_begin()

    def _gl_quads(self):
        """The runtime's quads for a scroll, from the bands (see own_picture)."""
        bands = getattr(self, "bands", None)
        if not bands:
            return super()._gl_quads()
        from ztos._gl import Quad
        w, view, s = float(self.viewport.width()), float(self.viewport.height()), self._scroll
        quads = [Quad.rect(ztos._bg_key(), ztos._WHOLE, self._gl_ox, self._gl_oy, w, view)]
        for key, (t, h) in zip(*bands):
            c0, c1 = max(t, s), min(t + h, s + view)
            if c1 > c0:
                quads.append(Quad.rect(key, QRectF(0, (c0 - t) / h, 1, (c1 - c0) / h),
                                       self._gl_ox, self._gl_oy + c0 - s, w, c1 - c0))
        return quads

    def shown(self):
        self.refresh()
        self.timer.start()
        self.ahead_timer.start()

    def ahead(self):
        """Still for a moment: the «qidirilmoqda» picture made before
        «tekşiriş» is pressed (see Skin.prerender)."""
        sk = self.skin
        if sk is None or not sk.isVisible() or sk.width() <= 0:
            return
        if sk.change is not None or self._gl_on or self._flinging() or self.uploading or ztos._animating():
            self.ahead_timer.start()    # later, when nothing moves
            return
        if any(k == "check" for k, _ in sk.m["actions"]):
            sk.prerender(model("checking"))

    def tick(self):
        r = self.skin.spots_rect()
        if not r.isEmpty():
            self.skin.update(r)

    def status_stamp(self):
        try:
            return os.stat(FAKE or O_STATUS).st_mtime_ns
        except OSError:
            return None

    def refresh(self):
        # just asked to check: "qidirilmoqda" at once, until the updater writes
        # anything itself (or 4 s pass)
        e = getattr(self, "expect", None)
        if e and (self.status_stamp() != e[1] or time.monotonic() > e[2]):
            e = self.expect = None
        m = model(e[0] if e else None)
        if m != self.skin.m:
            self.skin.change_to(m)
            self._changed(explicit=True)
        moving = m["busy"] or m["progress"] is None
        if moving and not self.anim.isActive():
            self.anim.start()
        elif not moving and self.anim.isActive():
            self.anim.stop()
            self.skin.update()

    # ---- actions
    def act(self, key):
        if key == "check":
            self.expect = ("checking", self.status_stamp(), time.monotonic() + 4)
            start("omonullos-update-check.service")
            self.refresh()
        elif key == "install":
            self.install()
        elif key == "rollback" and not self.skin.m["busy"]:   # not while it works
            self.rollback()
        elif key == "reboot":
            self.reboot()

    def install(self):
        a = read_json(os.path.join(O_VAR, "available.json")) or {}
        ztos.message(f"omonullOS {a.get('version', '')} örnatilsinmi?",
                     "Yuklab olinadi, imzosi va har bir fayl tekşiriladi, almaşadigan eski fayllar zaxiraga "
                     "olinadi. Siz özgartirgan sozlama fayllariga tegilmaydi. Keyin telefonni qayta yoqasiz; "
                     "yoqmasa, «orqaga qaytariş» bor.", ["örnatiş", "bekor qiliş"],
                     lambda choice: choice == 0 and start("omonullos-update-apply.service"))

    def rollback(self):
        to = rollback_to()
        ztos.message(f"{to} versiyaga qaytilsinmi?",
                     "Oxirgi yangilaniş almaştirgan fayllar zaxiradan qaytariladi, u qöşgan fayllar öçiriladi. "
                     "Keyin telefonni qayta yoqasiz.", ["qaytariş", "bekor qiliş"],
                     lambda choice: choice == 0 and start("omonullos-update-rollback.service"))

    def reboot(self):
        ztos.message("Telefon qayta yoqilsinmi?", "Oçiq ilovalar yopiladi.", ["qayta yoqiş", "bekor qiliş"],
                     lambda choice: choice == 0 and not FAKE and subprocess.Popen(["systemctl", "reboot"]))


app.run(Home())
