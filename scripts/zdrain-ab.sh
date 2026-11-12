#!/bin/sh
# zdrain-ab: who draws the battery with the screen off? Off the cable, screen
# off, run as root (detached, e.g. systemd-run); each phase holds STEP s, the
# fuel gauge (MAX77705 0x36, reads only, as zbattlog) is read every 5 s and
# the first 60 s of a phase are dropped. PLAN lists the phases in order;
# each one makes its change, then measures, and the changes add up:
#
#	base     as it is
#	pollers  background pollers stopped (zlowbatt, zphone-tunnel/cloudflared, zgov)
#	wifi     Wi-Fi radio off
#	radio    the modem's radio off (RIL RADIO_POWER 0: no calls or SMS meanwhile)
#	back     everything back (the baseline again, to see drift)
#
# PLAN="base pollers wifi back" by default.
#
# Everything is put back at the end, also on an error or a signal. Writes
# only /var/lib/zdrain/<time>.{csv,txt}. Added for OmonullOS, November 2026.

STEP=${STEP:-240}
OUT=/var/lib/zdrain
T=$(date +%Y%m%d-%H%M)
mkdir -p $OUT
CSV=$OUT/$T.csv
TXT=$OUT/$T.txt
BUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* | head -n1)
BUS=${BUS##*/i2c-}
PLAN=${PLAN:-base pollers wifi back}
ZRIL=$(dirname "$0")/../modem/ril/zril.py
STOPPED=""
WIFI=$(nmcli radio wifi)
RADIO=""

restore() {
	[ "$WIFI" = enabled ] && nmcli radio wifi on
	[ -n "$RADIO" ] && python3 "$ZRIL" -t 2 "rawi 23 1" > /dev/null
	RADIO=""
	for s in $STOPPED; do systemctl start $s; done
	STOPPED=""
	echo "restored $(date +%T)" >> $TXT
}
trap 'restore; exit 1' INT TERM
trap restore EXIT

cur() {	# mA, + = charging
	set -- $(i2ctransfer -y "$BUS" w1@0x36 0x0a r2 2>/dev/null)
	[ $# -eq 2 ] || return 1
	v=$(($1 | ($2 << 8))); [ $v -ge 32768 ] && v=$((v - 65536))
	echo $((v * 3125 / 10000))
}

screen() {
	for d in /sys/class/drm/card*-DSI-*/dpms; do
		[ -r "$d" ] && { [ "$(cat "$d")" = Off ] && echo off || echo on; return; }
	done
	echo ?
}

irqs() { awk 'NR>1{s=0; for(i=2;i<=9;i++) if($i ~ /^[0-9]+$/) s+=$i; t+=s} END{print t}' /proc/interrupts; }
idle() { awk '/^cpu /{print $5+$6; exit}' /proc/stat; }	# idle+iowait, jiffies

phase() {	# name
	sleep 60
	i0=$(irqs); j0=$(idle); n=0; sum=0; t0=$(date +%s)
	while [ $(( $(date +%s) - t0 )) -lt $((STEP - 60)) ]; do
		c=$(cur) && { echo "$(date +%s),$1,$c,$(screen)" >> $CSV; n=$((n + 1)); sum=$((sum + c)); }
		sleep 5
	done
	dt=$(( $(date +%s) - t0 ))
	i1=$(irqs); j1=$(idle)
	sort -t, -k3 -n $CSV | awk -F, -v p=$1 '$2==p{v[++n]=-$3} END{printf "%s: %d samples, median %d mA", p, n, v[int(n/2)+1]}' >> $TXT
	echo ", mean $(( -sum / (n ? n : 1) )) mA, irq $(( (i1 - i0) / dt ))/s, idle $(( (j1 - j0) * 100 / (dt * 100 * 8) ))%, screen $(screen)" >> $TXT
}

echo "zdrain-ab $T, plan $PLAN, step $STEP s, cpuidle $(cat /sys/devices/system/cpu/cpuidle/current_driver)" > $TXT
for p in $PLAN; do
	case $p in
	pollers)
		for s in zlowbatt zphone-tunnel zcpu; do
			systemctl is-active -q $s && systemctl stop $s && STOPPED="$STOPPED $s"
		done ;;
	wifi) nmcli radio wifi off ;;
	radio) python3 "$ZRIL" -t 2 "rawi 23 0" > /dev/null; RADIO=off ;;
	back) restore ;;
	base) ;;
	*) echo "unknown phase $p" >> $TXT; continue ;;
	esac
	phase $p
done
echo "done $(date +%T)" >> $TXT
