#!/bin/sh
# zlowbatt: power the Galaxy S10 5G off before its battery runs flat. Every
# 10 s it reads the MAX77705 fuel gauge (0x36, as zbatt: 0x06 RepSOC, 0x09
# VCell, 0x0b AvgCurrent); nine reads in a row (90 s) at or below SOC % or MV mV
# while discharging -> systemctl poweroff (the kernel drops PS_HOLD: DT syscon-poweroff).
# Both limits, since the gauge forgets what it learned when the cell runs
# flat and its SOC is off for a cycle or two after.
#
# Usage: zlowbatt [SOC MV] (defaults 3 %, 3400 mV). Reads only.
#
# Added for OmonullOS, October 2026: the phone went flat off the cable,
# hot, on 2026-10-18 and needed S-Boot's slow charge to come back.

SOC=${1:-3} MV=${2:-3400}
BUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* 2>/dev/null | head -n1)
BUS=${BUS##*/i2c-}
[ -n "$BUS" ] || { echo "zlowbatt: no I2C bus for 15d60000.i2c" >&2; exit 1; }

r16() {
	set -- $(i2ctransfer -y "$BUS" w1@0x36 "$1" r2 2>/dev/null)
	[ $# -eq 2 ] && echo $(($1 | ($2 << 8)))
}

# what the status bar shows (ztMETRO reads /run/zbatt when there is no
# power_supply class): charge in % and whether the charger input is valid
# (MAX77705 charger 0x69, DETAILS_00 CHGIN_DTLS bits 6:5 = 3)
status() {
	d0=$(i2cget -y "$BUS" 0x69 0xb3 2>/dev/null)
	mkdir -p /run/zbatt
	echo "$1" > /run/zbatt/capacity.new && mv /run/zbatt/capacity.new /run/zbatt/capacity
	[ -n "$d0" ] && echo $(( (d0 >> 5 & 3) == 3 )) > /run/zbatt/online
}

say() {
	echo "zboot| lowbatt: $*" > /dev/kmsg 2>/dev/null
}

say "power off at <= $SOC % or <= $MV mV while discharging"
low=0
while :; do
	soc=$(r16 0x06) v=$(r16 0x09) c=$(r16 0x0b)
	if [ -n "$soc" ] && [ -n "$v" ] && [ -n "$c" ]; then
		[ "$c" -ge 32768 ] && c=$((c - 65536))
		pct=$((soc >> 8)) mv=$((v * 78125 / 1000000))
		status $pct
		if [ "$c" -lt 0 ] && { [ $pct -le "$SOC" ] || [ $mv -le "$MV" ]; }; then
			low=$((low + 1))
			say "low: $pct %, $mv mV, $((c * 3125 / 10000)) mA ($low/9)"
			if [ $low -ge 9 ]; then
				say "powering off"
				systemctl poweroff
				exit 0
			fi
		else
			low=0
		fi
	fi
	sleep 10	# the charging glyph follows the plug within 10 s
done
