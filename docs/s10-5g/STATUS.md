# Galaxy S10 5G (SM-G977N, beyondx) — status

As in ztOS's README: **Works**: tried, and does what it should. **Partial**:
some of it works; the notes say what. **Broken**: tried, does not work.
**Not started**: nothing yet. **Untested**: written, not tried on the phone.

| Area | What | Status | Notes |
|---|---|---|---|
| Stage 1 | Facts about the phone | Partial | hardware.md: adb (no root), stock DT and DTBO, LineageOS source; Samsung's own source still downloading |
| Stage 1 | Stock firmware | Works | `G977NKSU6HWD3`, md5 checked; boot, recovery, dt, dtbo unpacked |
| Stage 2 | Bootloader unlock, root | Works | unlocked; TWRP 3.7.0_9-1 in BOOT and RECOVERY, adb root; live DT read |
| Stage 2 | Backup of every partition but userdata | Works | stock/backup-2026-10-16, 37 images, sha256 checked phone vs PC |
| Stage 3 | First mainline boot (simplefb, kernel log on screen) | Works | 2026-10-16: the log scrolls at the top left (8x16 font, too small to read) |
| Stage 3 | Panic → back to recovery | Works | uniLoader arms the cluster-0 watchdog: CLUSTER0_WDTRESET, then INFORM3 → TWRP, 83 s from the flash |
| Stage 4 | Clocks, pinctrl, UFS, USB gadget, PMIC, battery | Partial | 2026-10-16: USB gadget works (bx8: NCM + ACM, telnet shell; PHY+DWC3 modules, MAX77705 MUIC switch set over I2C). Watchdog fed from initramfs (bx9), shell stays up; battery read (zbatt), charges (charger watchdog kicked, bx12); all 7.1 GB of RAM (bx11, --mem); all 8 CPUs (bx13, --smp); temperatures read (ztemp), thermal guard (zthermal, bx15: affinity / big cores off / reset); wall charger at the stock TA rate, 1800/2100 mA, NTC-guarded (zchg, bx18: +1.3 A into the battery); no AFC/PD fast charge yet; battery/charger/USB NTC temperatures read (zbatt, matches Samsung's driver); 2026-10-17: UFS works (bx21/bx22: HS-G3 x2, reads verified by sha256, writes on userdata only, every other partition ro); **Arch Linux ARM boots from userdata** (bx22 --root, 227 GB ext4 ZR_ARCH, SSH over USB, guards carried over by zboot-guards.service); the rest not started |
| Stage 5 | Rootfs, ztMETRO, DECON, Panfrost (Mali-G76), touch | Not started | |
| Stage 6 | Modems (LTE + S5100 5G), SMS, calls, data, audio, Wi-Fi, sensors | Started | Wi-Fi works (bx29, loaded by hand); calls + SMS work; call audio downlink clear (2026-11-10, tested by ear), uplink still a stand-in (no mic) |
| UI | iPhone-like line (`nav_style = line`, opt-in) | Works | 2026-11-10, tested by the owner: swipe up = Start, rest on the way = switcher (flick a card up to close), along the line = apps before/after, left edge = back, right-edge hold 2 s = keyboard, pull Start down = Hyperscan; iOS 26 spring (0.5 s), the line follows the finger, hidden on Start/switcher/panel, fades over any app after 3 s; switcher at 60 fps (perf log) |
| UI | Uzbek (Latin) | Partial | ztsettings, ztphone, ztmessages, ztcontacts render in Uzbek on the PC (`--render`); the shell is only built on the phone, untested |
