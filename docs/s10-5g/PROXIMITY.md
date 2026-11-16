# Proximity / sensor hub (S10 5G) — research 2026-11-15

Goal: proximity sensor for calls (screen off at the ear). On the S10 5G the
proximity, light, accel, gyro and mag sensors sit behind the **sensor hub**,
a Broadcom **BCM4775 B0** (AsicVersion 0x004775B0; the DT still calls it
`ssp,BCM4773`) that is also the GNSS chip. Nothing of it runs on OmonullOS yet:
no SPI bus, no driver, no firmware loaded.

Everything below was read-only (dtb/dtbo/recovery partitions copied with
`cat`, vendor mounted ro). Copies: `~/omonullos-stock/sensorhub/` on the phone
(not in git: proprietary firmware + Samsung DT).

## Hardware (stock dtbo index 8, fragment@0..2, @95, @124)

| What | Stock |
|---|---|
| Bus | `spi_17` = `spi@10970000` (`samsung,exynos-spi`, fifo 64, SPI 426 = GIC 0x1aa, swap-mode), USI17 mode reg SYSREG_PERIC1 `0x1082102c` (`exynos-usi-v2`) |
| Clocks | stock 0x239 `gate_spi_clk`, 0x249 `ipclk_spi` (CMU_PERIC1) — we have no PERIC1 clock driver |
| SPI pins | gph0-2/gph0-1/gph0-0 func 2 (bus), CS gph0-3 as GPIO (`cs-gpio`), 25 MHz, CPOL=1 CPHA=1 |
| ssp-host-req | gpa1-7 (EINT, pull-down) — MCU → AP request |
| ssp-shub-int | gpa4-0 (EINT) |
| ssp-batch-wake-irq | gpa0-5 (input) |
| ssp-mcu-req | gpg4-1 (out) — AP → MCU |
| ssp-mcu-resp | gpg4-2 (in) |
| ssp-gps-pwr-en | gpg0-2 (out) — chip power/standby (`/sys/class/sec/gps/GPS_PWR_EN`) |
| Supply | `vdd_mcu_1p8` = S2MPS19 **LDO21M**, `regulator-always-on` (on at handover) |
| Board data | ssp-ap-type 3, acc/mag position 4, mag-type 1, mag-array, thermistor tables (fragment@124) |

Pin banks today: our pinctrl exposes only the ALIVE banks (gpa0–gpa4, gpq0).
gph0, gpg0, gpg4 have no GPIO driver → step 1 needs them.

## Software (stock Android 11)

- Kernel: `drivers/sensorhub/brcm/` — `ssp` (sensor protocol, input/IIO
  devices), `bbd` (Broadcom Bridge Driver: `/dev/bbd_control`, `/dev/bbd_patch`,
  `/dev/bbd_sensor`, sysfs `bbd.0/{BBD,ESW,ssi_xfer,…}`) and the SSI SPI
  transport `/dev/ttyBCM` (`bcm_gps_spi`-style: framing over SPI with
  mcu-req/mcu-resp/host-req handshake).
- Firmware: **inside the kernel**, served to user space by `/dev/bbd_patch`.
  Found as text in the TWRP kernel on `recovery` (4.14.113, built from Samsung
  source): three `bcm_sensorhub_a1` patches, FormatVersion 0x00010003,
  AsicVersion 0x004775B0, PatchAddress 0x00800161, BlobLength 445094 / 423374
  / 418740. Which one is beyondx's: still to find (driver source picks by model).
- User space: `/vendor/bin/hw/lhd` (Location Hub Daemon, Android 30 aarch64,
  bionic, proprietary) with `/vendor/etc/sensor/lhd.conf`: LheBbdPacket
  `/dev/ttyBCM`, LheBbdControl `/dev/bbd_control`, LheBbdSensor
  `/dev/bbd_sensor`, LhePatch `/dev/bbd_patch`, power via
  `/sys/class/sec/sensorhub/mcu_power` + GPS_PWR_EN, LheClockMHz 100,
  PinMux P17=M1. lhd downloads the patch, boots the hub, and relays the sensor
  stream between ttyBCM and bbd_sensor (where the kernel `ssp` driver lives).
  We already run vendor binaries in the Android chroot (`/run/android`, modem
  cbd/rild), so lhd can run the same way.

## Plan (each step that touches hardware: owner «ha» first)

1. **Pins + bus.** gph0/gpg0/gpg4 need a GPIO path. Options: a pinctrl bank
   for that block in a runtime overlay module (like zrn_board / pcie1), or
   bit-banged SPI in our own module (the codec bring-up already did SPI
   bit-bang). Real controller later: `spi-s3c64xx` + `exynos-usi` (USI17 →
   SPI mode) need the PERIC1 clock gates; read CMU_PERIC1 first.
2. **Transport module** `zrn-ssp-bx`: SSI framing on SPI + the req/resp/host-req
   handshake, exposing `/dev/ttyBCM`, `/dev/bbd_control`, `/dev/bbd_patch`
   (beyondx patch), `/dev/bbd_sensor` and `/sys/class/sec/sensorhub/mcu_power`
   so lhd runs unchanged. Port from Samsung's GPL source (same model, 4.14).
3. **lhd in the Android chroot** (service like zmodem), check the hub boots
   (ESW version, patch accepted).
4. **Sensor side** in user space first: read `/dev/bbd_sensor`, send the SSP
   "enable proximity" message, parse prox events → a small daemon that
   blanks the screen in calls (zcell knows the call state). Later the other
   sensors (light → auto brightness, accel → rotation).

Risks: the hub shares power with GNSS; a wrong pin direction on gpg0-2 /
gpg4-1 could fight the chip (pins are outputs on the AP side only per stock
DT). No PMIC writes needed (LDO21M already always-on).

## 2026-11-15 19:30–19:50 — first bring-up (owner «ha»)

`sensors/beyondx/` (zrn-bbd-bx, 9d34c5b), loaded by hand with `insmod` from
`~/omonullos-src/build/bbd-bx/`; lhd by hand, then `systemctl start
ztoscsf-sensorhub` (the S9's unit, unchanged; /dev/zrn_ssp present so it skips
its modprobe).

- Load: USI16 mode 2, USI_CON out of reset, USI_OPTION 3, CS_REG 1, gph0 CON
  0x2222 (gph0-4..7 untouched, DAT never written), gpg4 PUD pins 1/2 none,
  gpg0-2 out. After the enable pin: MCU_RESP 1, SHUB_INT 0 (the S9 code's
  "did not reset" warning is wrong here: it was 0 before power-up).
- SPI_CLK_CFG reads back 0: USI v2 SPI has no prescaler here, the bus runs
  at the CMU clock / 4 (200 MHz / 4 = 50 MHz unless DIV_CLK_PERIC1_USI16_USI,
  0x10801830, is raised; writing it by hand was refused by auto mode). It
  works anyway: no FC retries, no errors.
- lhd needs `/data/vendor/gps` (its `.lhd.lock`); without it: "LHD Daemon
  Process is already running". Created `/run/android/data/vendor/gps` and
  `/run/android/data/system/gps` (→ zrn-android-root should make them).
- **Patch download 1.1 s, `ESW:READY`, hub ready 3 s after start.**
  `zrn-ssp.py info`: firmware 22072000, sensors accel gyro mag* pressure
  prox light prox_raw orientation step* rot* light_ir int_gyro tilt pickup
  light_cct accel_uncal ... (same numbering as the S9: prox 7, light 9).
- Light (TCS3701 / alg. TCS3407): works, ~135–200 lux indoors. Its record is
  longer than the S9's (zrn-ssp prints `data?`): sensor_list.h says 20/21 bytes.
- **Proximity: on, but pdata is 16368 (14-bit full scale) all the time**,
  hand over the sensor or not, on the desk and in a pocket. The firmware cycles
  "pr 1sec open / PRX_INIT / pr 1sec close baseline 16368". Sent first, as
  stock does at init/enable: 0xB6 ap info {3, 8}, 0xF1 {4,4,4}, 0xEC {0},
  0xF2 thresholds 2400/1600/16368/250 (beyond defaults, AUTOCAL_AMS): taken
  ("threshod 0 = 2400"…), no change. Hub's own config (0x4C DHR info): chip
  0x18, drive 0x08, persist 0x50, pulse 0x82, gain 0x09, ptime 0x01,
  plen 0x82, offset 250; trim (0x40) 0xfa.

Next for proximity (owner present): the sensor sits under the OLED. Suspects:
crosstalk with offset 250 too small (stock: /efs/FactoryApp/prox_cal on the
sec_efs side, not in the modem EFS copy; MSG2SSP_AP_MCU_SET_PROX_CAL 0xCF,
MSG2SSP_AP_SET_PROX_SETTING 0x48); missing panel state messages (stock tells
the hub LCD on/off, brightness, panel type); prox LED supply. Read what stock
sends from Samsung's ssp_* (ivanmeler/android_kernel_samsung_beyondlte) and,
if possible, the factory prox_cal value (read-only).

Not yet: install into /usr/lib/modules (+ ztoscsf-sensorhub picking
zrn-bbd-bx on beyondx), zrn-android-root dirs, ztoscsf-sensord light record
size for the S10.

### 2026-11-15 ~19:52 — prox_cal tries (no EFS read)

Samsung's beyond driver (drivers/sensorhub/brcm, factory/prox_autocal_ams.c):
at every hub sync it reads `/efs/FactoryApp/prox_cal` (3 ints: offset, hi, lo)
and sends `0xCF` MCU_SET_PROX_CAL with the offset as s16; without the file the
offset is 0x0FFF. Gate `check_proximity_adc_calib_hw_ver`: ap_type 3
(beyondx) needs ap_rev >= 4. A hub-side ADC calibration exists as sensor 39
(PROXIMITY_ADC_CALIB, add with 4-byte delay, reports s16 offset + u16 hi/lo).

Tried with the hub up (scratchpad proxcal.py):
- `0xCF` 0x0FFF, then `0xCF` 100: accepted (empty reply) but ignored — on
  enable the hub still logs `write offset 250 (0xfa 0x0)` (its own trim).
- sensor 39 add/remove: no record, no log.
- Enable log: `ap status 0, cover_set 0`, then `1, baseline -1, pdata 16368, ir 0`
  (IR channel 0). Light worked (lux 3-18, room dim).
Still saturated. Next needs the owner: phone face up on a desk, cover/uncover,
and maybe panel info (0x91, struct panel_bl_event_data) / LCD state.

### 2026-11-15 20:20–20:30 — owner at the phone

- After zsleepd's deep sleeps USI16 was reset (`usi mode 0`, `spi ch 0`): hub
  start failed with SPI -110 / ESW:CRASHED. Live fix: `echo 1 >
  /sys/class/sec/sensorhub/reset` (runs hw_spi_setup). Driver fix in the
  next commit (re-setup when USI mode != 2), built, not loaded.
- Cover test (phone face up, palm over the top): **light follows the hand
  (150 → 0 lux), pdata stays 16368 the whole time.** The window and the
  position are right; the IR path is not.
- Samsung's patch choice: beyondx rev >= 4 → `bbd_patch` (r_os
  bbd_patch_file_beyond.h) = the one we load. Fine.
- 0x91 panel info {brightness 255, aor 0} + 0x96 hall 0: no change.
- No separate prox LED regulator in the DT (only vdd_mcu_1p8 = LDO21M).
- Open questions for the owner: screen protector (S10 under-display prox is
  known to break with tempered glass)? did prox work on stock Android with it?

### 2026-11-15 ~20:40 — TCS3701 registers read through the hub (0x4B, read only)

Owner: prox worked on stock Android, no screen protector → software/config.
`0x4B` REGISTER_SETTING read: header data = sensor<<24 | 1<<16 | reg<<8,
reply 5 bytes {sensor, r, reg, value, ok}. With prox on (sensor 7):

    80 ENABLE 00 (between cycles)   82 PTIME 01   88/89 ff ff   8c 00
    90 c6  91 REVID 01  92 ID 18    93 STATUS 00 / 40 / 70  (PSAT set)
    9c 3f  a1/a2 f0 3f (= 0x3ff0 = 16368, the stuck pdata)
    a4 08 (drive)  a5 30  a9 10  aa 09  ac 0c  b0 6a  b1 90  b3 f2  b6 72
    b7 80  b8 02  b9 06  bb 09 (gain)  bc 82 (pulse)  bd 50 (persist)
    be 02  bf d4  c7 fa (offset 250)  ca/cb e7 03 (ASTEP 999)  cc 02
    cd 2a  ce 6a  cf 99  d6 ff  d7 21  d9 64

STATUS bit 6 (PSAT) = the chip itself reports proximity saturation, so the
IR return (crosstalk) is far above what offset 250 can cancel. ap_rev 0/1/4/12
in 0xB6 changes nothing (likely read at hub boot only).
Next (needs owner «ha», writes go to the sensor chip through the hub):
lower LED drive / pulses via 0x4B w or 0x48 PROX_SETTING; or read the factory
/efs/FactoryApp/prox_cal (read only).

### 2026-11-15 ~21:00–21:20 — with the owner (writes OK'd by owner)

- sec_efs copy (/run/android/efs, read only): **no FactoryApp/prox_cal**;
  only Sensorinfo names ("TCS3701", "Palm Proximity Sensor"). So stock also
  sent offset 0x0FFF: the factory file is not the difference.
- 0x4C DHR read (fresh hub): `18 | thresholds 0 0 0 0 | drive 06 persist 50
  pulse 82 gain 09 ptime 01 plen 82 atime 00 | offset fa 00`. Chip regs:
  **drive = 0xB9, pulse = 0xBC, gain = 0xBB, persist = 0xBD, offset = 0xC7**.
- 0x4B write works (reply {7, 0, reg, val, 1}), but the firmware's PRX_INIT
  (every ~0.6–1 s) writes its values back. bc=00 once: one read of 0x00f0.
- Screen off (DPMS Off 23 s): pdata still 16368 → not the OLED's light.
- 0x48 PROX_SETTING {idx 0..7, 0x01}: no change in DHR or registers.
- Holding b9=00 bc=00 by rewriting + only keeping samples where both read
  back low: pdata steps 0 / 4210 / 4092 / 12276 / 16368 / 5108, one value per
  ~1 s, no clear link to the palm. The race with PRX_INIT spoils it.
- Fallback idea for calls: the light channel follows a palm cleanly
  (150 → 0 lux), usable as "near" by relative drop.

## 2026-11-15 22:17 — ANSWER: stock's call proximity is the touch IC's ear detection

The TCS3701 under the OLED is Samsung's "Palm Proximity Sensor" (that is its
name in /efs/FactoryApp/Sensorinfo/Proximity): it only reacts to touch, and
on our hub its pdata sits at 16368. Third-party apps on stock saw that; the
Phone app used an internal sensor (Sensor Inspector author's write-up,
sites.google.com/view/glimpse-notifications/startseite/samsung_s10_issues).
LineageOS's init chowns /sys/class/sec/tsp/virtual_prox and prox_power_off,
and the stock DT touch node has `support_ear_detect_mode`: Samsung sec_ts/y771
writes **0xEA** (SEC_TS_SET_EAR_DETECT_MODE, 1 byte) and the IC sends status
events `eid 1, stype 7 (vendor info), id 0x6A, data_1 = state`.

kernel/touch-ed/zrn-s6sy761.ko (2bba0d9), bound by hand in place of the
built-in s6sy761 (ed-trial.sh: unbind, bind, ear_detect=1, auto-rollback),
with the owner holding the phone to his ear 3–4 times:

    status 1d 6a 00 ... → 0 far     1d 6a 01 → 1 near     1d 6a 02 → 2 touching

~20 clean transitions, touch kept working. **Call proximity works.**
SW_FRONT_PROXIMITY on input "s6sy761 ear detect" = (state != 0).
Next: ear_detect on only during calls (zcell), screen off/on + touch ignore
while near; load zrn-s6sy761 at boot (ztouch binds it instead of the
built-in, or patch the built-in driver in the next kernel).
