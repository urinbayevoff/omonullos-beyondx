#!/bin/sh
# zgpu: the Galaxy S10 5G's Mali-G76 at boot. Loads gpu-sched and panfrost
# (built by build-boot.sh --gpu, not in the kernel image) and, when there,
# the DECON scanout bx-decon (--decon; simpledrm stays if it fails), puts
# G3D at LEVEL through the APM (zacpm dvfs set g3d: FREQ_REQ, the APM picks
# the voltage from its own table) and then guards its heat, once every 2 s:
# at HOT (75 C) on the hottest G3D sensor (TMU 1-4 and 9, as ztemp) down to
# 260 MHz, back to LEVEL below CLEAR (60 C). zthermal still covers the whole SoC (its 95 C
# reset reads every sensor, G3D included). With the panel off (DSI dpms Off)
# G3D drops to IDLE (156 MHz, its boot level) and comes back when it is on;
# DRAM (MIF) and the internal bus (INT) go with it, from their boot rates
# 2093/534 MHz down to the stock idle 421/100 MHz (~60 mA less, measured on the
# cable 2026-10-18) and back up before the panel is used. With the panel on,
# MIF runs at 1014 MHz, not 2093: ztMETRO stays at 60 Hz (17 ms frames,
# owner's eyes, 2026-10-19); the scanout needs ~1 GB/s of the ~11 GB/s.
#
# Usage: zgpu [LEVEL HOT CLEAR] (MHz, whole degrees); a stock fvmap level up
# to 702 MHz. Writes nothing but the G3D, MIF and INT DVFS requests. zgpu load: the modules
# only (zgpu.service's ExecStartPre, so greetd starts with the render node
# and the DECON card there and wlroots picks them, not llvmpipe on simpledrm).
#
# Added for OmonullOS, October 2026. The owner asked for the GPU at every
# boot (2026-10-17); levels tested by hand first: 702 MHz = 148 fps in
# gputest, 58 C after 20 s (docs/s10-5g/log.md 10:45).
# Modified for OmonullOS, November 2026: calibration read once, no G3D
# reads with the panel off, dpms without cat (~6 % of a core at idle).

LEVEL=${1:-572} HOT=${2:-75} CLEAR=${3:-60}
LOW=260
IDLE=156
B=0x10090000
M=/usr/local/lib/zboot/modules

say() {
	echo "zboot| gpu: $*" > /dev/kmsg 2>/dev/null
}

# per-sensor calibration (c25, c85) does not change: read once
G3D_S="1 2 3 4 9"
calib() {
	two=$((($(devmem $B 32) >> 23) & 1))
	for s in $G3D_S; do
		t=$(devmem $((B + 4 * s)) 32)
		eval "a_$s=$((t & 0x1ff)) b_$s=$(((t >> 9) & 0x1ff))"
	done
}

g3d() {	# whole degrees, the hottest G3D sensor (see ztemp)
	max=-100
	lastw=
	for s in $G3D_S; do
		wi=$((s / 2))
		# sensors 2 and 3 share a CURRENT_TEMP word: one read for both
		[ "$wi" = "$lastw" ] || { w=$(devmem $((B + 0x84 + 4 * wi)) 32); lastw=$wi; }
		eval "a=\$a_$s b=\$b_$s"
		code=$(((w >> (16 * (s % 2))) & 0x1ff))
		if [ "$two" -eq 1 ] && [ "$b" -gt "$a" ]; then
			v=$(((code - a) * 60 / (b - a) + 25))
		else
			v=$((code - a + 25))
		fi
		[ $v -gt $max ] && max=$v
	done
	echo $max
}

screen_off() {	# the DSI panel's dpms, bx-decon only (simpledrm has none)
	for d in /sys/class/drm/card*-DSI-*/dpms; do
		[ -r "$d" ] && read -r v < "$d" && [ "$v" = Off ] && return 0
	done
	return 1
}

set_g3d() {
	zacpm dvfs set g3d "$1" > /dev/null || say "g3d $1 MHz: zacpm failed"
}

MIF_ON=1014	# MHz with the panel on (2093 at boot)

set_bus() {	# MIF INT, MHz (owner's «ha» for MIF/INT DVFS, 2026-10-18)
	zacpm dvfs set mif "$1" > /dev/null || say "mif $1 MHz: zacpm failed"
	zacpm dvfs set int "$2" > /dev/null || say "int $2 MHz: zacpm failed"
}

grep -q '^gpu_sched ' /proc/modules || insmod $M/gpu-sched.ko || exit 1
grep -q '^panfrost ' /proc/modules || insmod $M/panfrost.ko || exit 1
# the owner checked bx-decon on the panel (2026-10-17) and asked for it at boot
if [ -f $M/bx-decon.ko ] && ! grep -q '^bx_decon ' /proc/modules; then
	{ grep -q '^drm_dma_helper ' /proc/modules || insmod $M/drm_dma_helper.ko; } &&
		insmod $M/bx-decon.ko || say "bx-decon did not load: simpledrm stays"
fi
[ "$1" = load ] && exit 0
calib
set_bus $MIF_ON 534
set_g3d "$LEVEL"
say "panfrost loaded, g3d $LEVEL MHz; down to $LOW at $HOT C, back below $CLEAR C; now $(g3d) C"

state=normal
while :; do
	# panel off: G3D sits at IDLE and zthermal watches every sensor, so the
	# temperature is read only for the log line when going idle
	if screen_off && [ ! -e /run/zcall-bus ]; then	# zcell: a call keeps MIF/INT up
		want=idle
		[ $state != idle ] && T=$(g3d)
	elif T=$(g3d); [ "$T" -ge "$HOT" ] || { [ $state = hot ] && [ "$T" -ge "$CLEAR" ]; }; then
		want=hot
	else
		want=normal
	fi
	if [ $want != $state ]; then
		[ $state = idle ] && set_bus $MIF_ON 534
		case $want in
		idle) set_g3d $IDLE; set_bus 421 100 ;;
		hot) set_g3d $LOW ;;
		normal) set_g3d "$LEVEL" ;;
		esac
		say "$T C, $want: g3d $(case $want in idle) echo $IDLE;; hot) echo $LOW;; *) echo "$LEVEL";; esac) MHz"
		state=$want
	fi
	sleep 2
done
