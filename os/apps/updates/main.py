"""Updates: ztOS's software updater. What can be updated, and installing it;
the work is done by the system (ztos-update, as root, started through systemd),
this shows how it goes.

Modified for OmonullOS, November 2026: in Uzbek; omonullOS's own signed
updates first (omonullos-update: version, what is new, install, roll back);
Arch's packages (ztos-update, pacman -Syu) only behind a warning, as a
whole-system upgrade would replace our kernel's initramfs and more."""
import json
import os
import subprocess
import ztos
from PySide6.QtCore import QTimer

app = ztos.App()

# omonullOS (omonullos-update)
O_STATUS = "/run/omonullos-update/status.json"
O_VAR = "/var/lib/omonullos-update"
# Arch packages (upstream ztos-update)
STATUS = "/run/ztos-update/status"
LIST = "/var/lib/ztos/updates"


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def read_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def start(unit):
    """Ask systemd to run one of the updater's services (allowed without a password)."""
    try:
        subprocess.Popen(["systemctl", "start", "--no-block", unit])
        return True
    except OSError:
        return False


def mb(n):
    n = int(n or 0)
    return f"{n / 1048576:.1f} MB" if n >= 1048576 else f"{max(1, n // 1024)} KB"


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


O_TEXT = {
    "checking": "Yangilaniş qidirilmoqda...",
    "uptodate": "omonullOS eng yangi versiyada.",
    "downloading": "Yuklab olinmoqda...",
    "verifying": "Imzo va fayllar tekşirilmoqda...",
    "badsig": "Topilgan yangilaniş imzosi notoğri, örnatilmaydi.",
    "refused": "Topilgan yangilaniş bu telefon uçun emas, örnatilmaydi.",
    "needs-updater": "Bu yangilaniş uçun avval yangilaş dasturining özini yangilaş kerak.",
}


class Home(ztos.Page):
    def __init__(self):
        super().__init__("yangilanişlar")
        self.state = self.text("", size=17)
        self.progress = self.add(ztos.ProgressBar(0, caption=""))
        self.progress.hide()
        self.space(12)

        self.section("omonullOS")
        self.version = self.text("", size=13, color=ztos.GREY)
        self.check_entry = self.entry("tekşiriş", "hozir qarab çiqiş", self.check)
        self.install_entry = self.entry("örnatiş", "", self.install)
        self.reboot_entry = self.entry("telefonni qayta yoqiş", "yangi versiya şunda işlaydi", self.reboot)
        self.rollback_entry = self.entry("orqaga qaytariş", "", self.rollback)
        self.notes_title = self.text("nima yangi", size=15)
        self.notes = self.text("", size=13, color=ztos.GREY)
        self.notes.setWordWrap(True)

        self.space(24)
        self.section("tizim paketlari")
        self.arch_note = self.text("Arch Linux paketlari (pacman). Butun tizimni yangilaydi, bizning "
                                   "yadro va initramfs bilan buzilişi mumkin.", size=13, color=ztos.GREY)
        self.arch_note.setWordWrap(True)
        self.arch_open = self.entry("baribir körsatiş", "ehtiyot böling", self.open_arch)
        self.arch_state = self.text("", size=13)
        self.arch_check = self.entry("paketlarni tekşiriş", "", self.arch_check_now)
        self.arch_install = self.entry("paketlarni yangilaş", "", self.arch_install_now)
        self.packages = self.text("", size=13, color=ztos.GREY)
        self.arch_shown = False

        self.timer = QTimer(self, interval=700)
        self.timer.timeout.connect(self.refresh)

    def shown(self):
        self.refresh()
        self.timer.start()

    # ---- omonullOS
    def check(self):
        if start("omonullos-update-check.service"):
            self.busy(True)

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
                     lambda choice: choice == 0 and subprocess.Popen(["systemctl", "reboot"]))

    # ---- Arch packages (upstream)
    def open_arch(self):
        ztos.message("Tizim paketlari", "pacman -Syu butun tizimni yangilaydi. omonullOSʼning yadrosi va "
                     "initramfsʼi bizniki: bu yangilaniş ularni buzib, telefon yoqilmay qolişi mumkin. "
                     "Nima qilayotganingizni bilsangizgina davom eting.", ["körsatiş", "bekor qiliş"],
                     lambda choice: choice == 0 and self._show_arch())

    def _show_arch(self):
        self.arch_shown = True
        self.refresh()

    def arch_check_now(self):
        if start("ztos-update-check.service"):
            self.busy(True)

    def arch_install_now(self):
        ztos.message("Paketlar yangilansinmi?", "Hamma Arch paketlari yuklab olinib örnatiladi. Bu "
                     "omonullOSʼni buzişi mumkin.", ["yangilaş", "bekor qiliş"],
                     lambda choice: choice == 0 and start("ztos-update-apply.service"))

    # ---- what is shown
    def refresh(self):
        s = read_json(O_STATUS) or {}
        st = s.get("state", "idle")
        have = read_json(os.path.join(O_VAR, "installed.json")) or {}
        avail = read_json(os.path.join(O_VAR, "available.json"))
        to = rollback_to()
        working = st in ("checking", "downloading", "verifying", "installing", "rolling-back")

        hv = have.get("version", "0")
        self.version.setText("Örnatilgan versiya: " + (hv if hv != "0" else "nomaʼlum"))
        self.check_entry.setVisible(not working)
        can_install = bool(avail) and not avail.get("_needs_updater") and not working and st != "done"
        self.install_entry.setVisible(can_install)
        if avail:
            pkg = avail.get("package", {})
            d = avail.get("delta")
            if d and str(d.get("base")) == str(hv):
                pkg = d
            self.install_entry.set_detail(f"{avail.get('version')} · {mb(pkg.get('size'))}")
        need_reboot = s.get("after", "reboot") != "none"
        self.reboot_entry.setVisible(st in ("done", "rolled-back") and need_reboot)
        self.rollback_entry.setVisible(bool(to) and not working)
        self.rollback_entry.set_detail(f"{to} versiyaga" if to else "")
        notes = (avail or have).get("notes", "") if (avail or st == "done") else ""
        self.notes_title.setVisible(bool(notes))
        self.notes.setVisible(bool(notes))
        self.notes.setText(notes)

        self.progress.setVisible(st in ("downloading", "verifying", "installing", "rolling-back"))
        if st in ("installing", "rolling-back") and s.get("n"):
            k, n = int(s.get("k", 0)), max(1, int(s["n"]))
            self.progress.set_value(100 * k / n)
            verb = "Örnatilmoqda" if st == "installing" else "Qaytarilmoqda"
            text = f"{verb}: {k} / {n} — {s.get('name', '')}"
        elif st in ("downloading", "verifying"):
            self.progress.set_value(None)
            text = O_TEXT[st]
        elif st == "available" or (avail and st == "idle"):
            text = f"omonullOS {avail.get('version') if avail else ''} bor."
        elif st == "done":
            text = f"omonullOS {s.get('version', '')} örnatildi." + (" Telefonni qayta yoqing." if need_reboot else "")
            if s.get("kept"):
                text += ("\nSiz özgartirgan fayllarga tegilmadi, yangisi yonida .new: "
                         + ", ".join("/" + k for k in s["kept"]))
        elif st == "failed":
            text = "Bölmadi: " + s.get("msg", "") + "\nBatafsil: /var/log/omonullos-update.log"
        elif st == "rolled-back":
            text = "Oldingi versiyaga qaytildi." + (" Telefonni qayta yoqing." if need_reboot else "")
        elif st in O_TEXT:
            text = O_TEXT[st]
        else:
            text = "Yangi versiya bormi, tekşirib köring."
        self.state.setText(text)

        # Arch packages: only after the warning
        for w in (self.arch_state, self.arch_check, self.arch_install, self.packages):
            w.setVisible(self.arch_shown)
        self.arch_open.setVisible(not self.arch_shown)
        a_working = False
        if self.arch_shown:
            words = read(STATUS).split() or ["idle"]
            a = words[0]
            updates = [line for line in read(LIST).splitlines() if line.strip()]
            a_working = a in ("checking", "downloading", "installing")
            self.arch_check.setVisible(not a_working)
            self.arch_install.setVisible(bool(updates) and not a_working)
            self.arch_install.set_detail(f"{len(updates)} ta kutmoqda")
            if a == "installing" and len(words) >= 3:
                atext = f"Örnatilmoqda {words[1]} / {words[2]}: {words[3] if len(words) > 3 else ''}"
            else:
                atext = {"checking": "Paketlar qidirilmoqda...", "downloading": "Yuklanmoqda...",
                         "uptodate": "Paketlar yangi.", "available": f"{len(updates)} ta paket yangilaniş mumkin.",
                         "done": "Paketlar yangilandi.",
                         "failed": "Bölmadi. Batafsil: /var/log/ztos-update.log"}.get(a, "")
            self.arch_state.setText(atext)
            self.packages.setText("\n".join(updates[:60]) + ("\n..." if len(updates) > 60 else "")
                                  if updates else "")
        self.busy(working or a_working)


app.run(Home())
