#!/bin/sh
# zbattlog: every 30 s, one line of the MAX77705 fuel gauge (0x36, as zbatt)
# and the panel state into /var/lib/zbattlog/<day>.csv, to see what the
# phone draws off the cable with the screen on and off:
#
#	time,soc_pct,mv,ma,avg_ma,screen
#
# ma/avg_ma: + = charging, - = discharging. Reads only. Ten days are kept.
#
# Added for OmonullOS, October 2026: CPU idle at boot (zrn-cpuidle) needed
# a measure of the drain off the cable.

# November 2026: zmon (zmon.py) took over; it writes this same CSV and a
# fuller log in /var/lib/zmon. This loop stays as the fallback without it.
[ -x /usr/local/bin/zmon ] && exec /usr/local/bin/zmon run

D=/var/lib/zbattlog
BUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* 2>/dev/null | head -n1)
BUS=${BUS##*/i2c-}
[ -n "$BUS" ] || { echo "zbattlog: no I2C bus for 15d60000.i2c" >&2; exit 1; }
mkdir -p $D

r16() {
	set -- $(i2ctransfer -y "$BUS" w1@0x36 "$1" r2 2>/dev/null)
	[ $# -eq 2 ] && echo $(($1 | ($2 << 8)))
}

s16() {
	v=$(r16 "$1") || return 1
	[ "$v" -ge 32768 ] && v=$((v - 65536))
	echo "$v"
}

screen() {	# the DSI panel's dpms, bx-decon only (simpledrm has none)
	for d in /sys/class/drm/card*-DSI-*/dpms; do
		[ -r "$d" ] && { [ "$(cat "$d")" = Off ] && echo off || echo on; return; }
	done
	echo ?
}

while :; do
	soc=$(r16 0x06) && mv=$(r16 0x09) && cur=$(s16 0x0a) && avg=$(s16 0x0b) &&
		echo "$(date +%s),$((soc * 10 / 256 / 10)).$((soc * 10 / 256 % 10)),$((mv * 78125 / 1000000)),$((cur * 3125 / 10000)),$((avg * 3125 / 10000)),$(screen)" \
			>> $D/$(date +%Y%m%d).csv
	ls -1 $D/*.csv | head -n -10 | xargs -r rm -f
	sleep 30
done
