#!/bin/sh
# zchgoff: let the Galaxy S10 5G charge while it is powered off. S-Boot leaves
# the MAX77705 charger's watchdog on (CHG_CNFG_00 0xb7 WDTEN, bit 4); in our
# system wdt-pet kicks it, but once the phone is off nobody does and charging
# stops about a minute later (2026-10-18: 18.0 -> 18.4 % in 20 min off the
# charger). With a cable in, on the way to power off, this puts S-Boot's own
# limits back (the CNFG_09/02 values zchg saved, 500 / 450 mA: slow, and no
# software watches the temperature while off) and clears WDTEN, so the charger
# keeps going on its own. Nothing is written without a cable, or when the
# charger's registers are locked (CNFG_06 CHGPROT not 11). S-Boot sets the
# watchdog again at the next boot.
#
# Usage: zchgoff [--dry]. zchgoff.service runs it on poweroff only.
# Added for OmonullOS, October 2026; owner's «ha» for the WDTEN write,
# 2026-10-18 (the limit writes are zchg's, «ha» 2026-10-17).

DRY=; [ "$1" = --dry ] && DRY=1
SAVED=/run/zboot.chg-sboot
BUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* 2>/dev/null | head -n1)
BUS=${BUS##*/i2c-}

say() {
	echo "zboot| chgoff: $*" > /dev/kmsg 2>/dev/null
	echo "zchgoff: $*"
}

r8() {
	v=$(i2cget -y "$BUS" 0x69 "$1" 2>/dev/null) && echo $((v)) || echo -1
}

w8() {	# w8 REG VALUE: write and read back
	[ -n "$DRY" ] && { say "would write $(printf '0x%02x = 0x%02x' "$1" "$2")"; return 0; }
	i2cset -y "$BUS" 0x69 "$1" "$2" 2>/dev/null
	[ "$(r8 "$1")" -eq "$2" ] || { say "$(printf 0x%02x "$1") did not take"; return 1; }
}

[ -n "$BUS" ] || { say "no I2C bus for 15d60000.i2c"; exit 0; }
d0=$(r8 0xb3) c6=$(r8 0xbd) c0=$(r8 0xb7)
[ "$d0" -ge 0 ] && [ "$c6" -ge 0 ] && [ "$c0" -ge 0 ] || { say "charger does not answer"; exit 0; }
[ $(((d0 >> 5) & 3)) -eq 3 ] || { say "no cable: nothing to do"; exit 0; }
[ $(((c6 >> 2) & 3)) -eq 3 ] || { say "CHGPROT locked (CNFG_06 $(printf 0x%02x "$c6")): left alone"; exit 0; }
if [ -s "$SAVED" ]; then
	read -r s9 s2 < "$SAVED"
	w8 0xc0 $(( ($(r8 0xc0) & ~0x7f) | (s9 & 0x7f) )) &&
		w8 0xb9 $(( ($(r8 0xb9) & ~0x3f) | (s2 & 0x3f) )) ||
		{ say "S-Boot limits not restored: watchdog left on"; exit 0; }
else
	say "S-Boot limits unknown ($SAVED): watchdog left on"
	exit 0
fi
[ $((c0 & 0x10)) -eq 0 ] && { say "watchdog already off"; exit 0; }
w8 0xb7 $((c0 & ~0x10)) && say "cable in: S-Boot limits, charger watchdog off (CNFG_00 $(printf 0x%02x "$c0") -> $(printf 0x%02x $((c0 & ~0x10))))"
