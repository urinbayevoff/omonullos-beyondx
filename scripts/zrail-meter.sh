#!/bin/sh
# zrail-meter: every S2MPS19/S2MPS20 rail's current with the screen off.
# Run as root, off the cable. Turns the screen off through ztMETRO's control
# pipe (as the session user), waits SETTLE s, then points the S2MPS19 meter's
# 8 channels at one group of rails after another (zacpm meter set: the
# meter's own ADC registers only) and reads each group after its ~16 s
# average has filled; the fuel gauge current is read alongside. The screen
# comes back on at the end. Output: /var/lib/zdrain/rail-<time>.txt.
# Added for OmonullOS, November 2026.

SETTLE=${SETTLE:-60}
OUT=/var/lib/zdrain
T=$(date +%Y%m%d-%H%M)
TXT=$OUT/rail-$T.txt
CTL=/run/user/1000/ztmetro.ctl
mkdir -p $OUT
BUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* | head -n1)
BUS=${BUS##*/i2c-}

ctl() { [ -p $CTL ] && timeout 2 sh -c "echo '$1' > $CTL"; }
cur() {	# battery mA, + = charging
	set -- $(i2ctransfer -y "$BUS" w1@0x36 0x0a r2 2>/dev/null)
	[ $# -eq 2 ] || return 1
	v=$(($1 | ($2 << 8))); [ $v -ge 32768 ] && v=$((v - 65536))
	echo $((v * 3125 / 10000))
}
trap 'ctl "screen on"' EXIT INT TERM

ctl "screen off"
echo "zrail-meter $T, cpuidle $(cat /sys/devices/system/cpu/cpuidle/current_driver)" > $TXT
sleep "$SETTLE"
for g in "1 2 3 4 5 6 7 8" "9 10 11 12 L3 L4 L18 L19" "L1 L2 L5 L6 L7 L8 L9 L10" \
	 "L11 L12 L13 L14 L15 L16 L17 L20" "L21 L22 L23 L24 L25 L26 L27 L28"; do
	zacpm meter set $g > /dev/null || { echo "set $g failed" >> $TXT; continue; }
	sleep 20
	echo "battery $(cur) mA" >> $TXT
	zacpm meter | grep '^ch' >> $TXT
done
zacpm meter2 set 1 2 3 > /dev/null && sleep 20 && zacpm meter2 | grep '^ch' >> $TXT
echo "battery $(cur) mA, done $(date +%T)" >> $TXT
