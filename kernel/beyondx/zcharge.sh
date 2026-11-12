#!/bin/sh
# zcharge: log what the Galaxy S10 5G's MAX77705 sees of the charger plugged
# in, for testing a wall or fast charger away from the PC (no telnet then).
# Read-only: status registers of the USB-C/MUIC side (I2C 0x25) and the
# charger (0x69), the fuel gauge (0x36); no interrupt register (reading
# those clears them) and nothing written to any chip.
#
# Usage: zcharge [SECONDS [INTERVAL]] (default 240 s, every 3 s: about 9 KB,
# within pmsg's 16 KB). One line per sample to /tmp/zcharge.log and to
# /dev/pmsg0, which survives a reset:
# TWRP shows it in /sys/fs/pstore/pmsg-ramoops-0. If the USB gadget is not
# configured again by the end (the phone is still away from the PC), it
# touches /run/zboot.nopet, so the watchdog brings the phone back to TWRP
# 30 s later, where the log can be read.
#
# Added for OmonullOS, October 2026. Tested on the phone (bx15, from /tmp
# over telnet, on the PC's USB: USB, VBUS 4.5-5.5 V, default Rp, 500 mA).
#
# Sources: Samsung's include/linux/muic/max77705-muic.h, include/linux/ccic/
# max77705.h, include/linux/mfd/max77705-private.h:
#   0x25:0x06 USBC_STATUS1: VBADC bits 7:4 (VBUS: 0 < 3.8 V, 1 3.8-4.5,
#     2 4.5-5.5, 3 5.5-6.5, ... 7 9.5-10.5, 0xa > 12.5 V), UIADC 2:0
#   0x25:0x08 BC_STATUS: VBUSDET bit 7, PRCHGTYP 5:3, CHGTYP 1:0 (0 none,
#     1 USB, 2 CDP, 3 dedicated charger)
#   0x25:0x0a CC_STATUS0: CCPinStat 7:6, CCIStat 5:4 (Rp current: 1 default
#     USB, 2 1.5 A, 3 3 A), CCStat 2:0 (1 sink)
#   0x25:0x0d PD_STATUS1: PSRDY bit 4 (a PD contract), FCT_ID 3:0
#   charger 0x69: 0xb3 DETAILS_00, 0xb4 DETAILS_01, 0xb9 CNFG_02 (charge
#     current), 0xc0 CNFG_09 (input limit); gauge 0x36: 0x09 VCell,
#     0x0a Current (as zbatt)

DUR=${1:-240}
INT=${2:-3}
BUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* 2>/dev/null | head -n1)
BUS=${BUS##*/i2c-}
[ -n "$BUS" ] || { echo "zcharge: no I2C bus for 15d60000.i2c (needs --muic)" >&2; exit 1; }

r() { v=$(i2cget -y "$BUS" "$1" "$2" 2>/dev/null); echo $((${v:-0})); }
g16() {
	set -- $(i2ctransfer -y "$BUS" w1@0x36 "$1" r2 2>/dev/null)
	[ $# -eq 2 ] && echo $(( ($2 << 8) | $1 )) || echo 0
}

out() {
	echo "$*" >> /tmp/zcharge.log
	echo "zc $*" > /dev/pmsg0 2>/dev/null
}

udc() { cat /sys/class/udc/*/state 2>/dev/null | head -n1; }

out "start $(cut -d' ' -f1 /proc/uptime) for ${DUR}s; t typ vbus cc pd ilim cc_mA mV mA usb"
end=$(( $(cut -d' ' -f1 /proc/uptime | cut -d. -f1) + DUR ))
while [ "$(cut -d' ' -f1 /proc/uptime | cut -d. -f1)" -lt "$end" ]; do
	bc=$(r 0x25 0x08)
	s1=$(r 0x25 0x06)
	cc=$(r 0x25 0x0a)
	pd=$(r 0x25 0x0d)
	case $((bc & 3)) in 0) typ=none ;; 1) typ=USB ;; 2) typ=CDP ;; 3) typ=DCP ;; esac
	[ $((bc >> 7)) -eq 1 ] || typ="$typ/novbus"
	case $(( (cc >> 4) & 3 )) in 1) rp=def ;; 2) rp=1.5A ;; 3) rp=3A ;; *) rp=- ;; esac
	ilim=$(( $(r 0x69 0xc0) & 0x7f ))
	[ $ilim -le 3 ] && ilim=100 || ilim=$(( (ilim + 1) * 25 ))
	chg=$(( $(r 0x69 0xb9) & 0x3f ))
	[ $chg -le 2 ] && chg=100 || chg=$((chg * 50))
	cur=$(g16 0x0a)
	[ "$cur" -ge 32768 ] && cur=$((cur - 65536))
	out "$(cut -d' ' -f1 /proc/uptime) $typ prch$(( (bc >> 3) & 7 )) vbadc$((s1 >> 4)) rp=$rp ccst$((cc & 7)) psrdy$(( (pd >> 4) & 1 )) ilim$ilim cc$chg $(( $(g16 0x09) * 78125 / 1000000 ))mV $((cur * 3125 / 10000))mA $(udc)"
	sleep "$INT"
done
if [ "$(udc)" = configured ]; then
	out "end $(cut -d' ' -f1 /proc/uptime): USB back"
else
	out "end $(cut -d' ' -f1 /proc/uptime): no USB host, back to TWRP in 30 s"
	touch /run/zboot.nopet
fi
