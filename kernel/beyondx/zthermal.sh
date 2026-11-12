#!/bin/sh
# zthermal: a thermal guard for the Galaxy S10 5G until a real thermal driver
# (TMU + cpufreq) is in. Nothing else limits heat: four big cores loaded take
# the SoC from 41 to 57 C in six seconds. Reads the TMU once a second (as
# ztemp does, devmem only) and acts on the hottest of its eleven sensors:
#
#   >= SOFT (75 C): every task's affinity to the A55s (cpu0-3), so the A75s
#                   and M4s go idle; undone below CLEAR (60 C)
#   >= HARD (85 C): cpu4-7 offline. One way: Samsung's EL3 does not bring a
#                   core back through PSCI CPU_ON after CPU_OFF (their kernel
#                   powers it up through the PMU first), so they stay off
#                   until the next boot
#   >= CRIT (95 C): reset now through uniLoader's watchdog (WTCNT = 1), the
#                   way back to TWRP already proven; reboot -f if that
#                   watchdog is not running
#
# Usage: zthermal [SOFT HARD CRIT CLEAR] (whole degrees); started from
# /etc/zboot/early unless zboot.thermal=off, levels from zboot.thermal=75,85,95,60.
# Writes nothing but task affinities, cpu4-7's online files, and in the last
# case WTCNT; no PMIC or TMU register.
#
# Added for OmonullOS, October 2026. Tested on the phone (bx15): each level
# triggered on purpose with low test levels, see docs/s10-5g/log.md.

B=0x10090000
WDT=0x10050000
SOFT=${1:-75} HARD=${2:-85} CRIT=${3:-95} CLEAR=${4:-60}

say() {
	echo "zboot| thermal: $*" > /dev/kmsg 2>/dev/null
}

# calibration, per sensor: c25 and c85 codes (see ztemp)
two=$(( ($(devmem $B 32) >> 23) & 1 ))
s=0
while [ $s -le 10 ]; do
	t=$(devmem $((B + 4 * s)) 32)
	eval "c25_$s=$((t & 0x1ff)) c85_$s=$(((t >> 9) & 0x1ff))"
	s=$((s + 1))
done

hottest() {	# whole degrees, the hottest of sensors 0-10
	max=-100
	s=0
	for o in 0x84 0x88 0x8c 0x90 0x94 0x98; do
		w=$(devmem $((B + o)) 32)
		for code in $((w & 0x1ff)) $(((w >> 16) & 0x1ff)); do
			[ $s -le 10 ] || break
			eval "a=\$c25_$s b=\$c85_$s"
			if [ "$two" -eq 1 ] && [ "$b" -gt "$a" ]; then
				v=$(( (code - a) * 60 / (b - a) + 25 ))
			else
				v=$((code - a + 25))
			fi
			[ $v -gt $max ] && max=$v
			s=$((s + 1))
		done
	done
	echo $max
}

affinity() {	# affinity <mask>: every task that allows it
	for t in /proc/[0-9]*/task/[0-9]*; do
		taskset -p "$1" "${t##*/}" >/dev/null 2>&1
	done
}

state=normal
say "on: soft $SOFT, hard $HARD, crit $CRIT, clear $CLEAR C; now $(hottest) C"
while :; do
	T=$(hottest)
	if [ "$T" -ge "$CRIT" ]; then
		say "$T C >= $CRIT: resetting now"
		touch /run/zboot.nopet
		sync
		con=$(devmem $WDT 32)
		if [ $((con & 0x21)) -eq $((0x21)) ]; then
			devmem $((WDT + 8)) 32 1
			sleep 1
		fi
		reboot -f
	elif [ "$T" -ge "$HARD" ] && [ "$state" != hard ]; then
		for c in 4 5 6 7; do
			echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null
		done
		state=hard
		say "$T C >= $HARD: cpu4-7 offline until reboot ($(cat /sys/devices/system/cpu/online) online)"
	elif [ "$T" -ge "$SOFT" ] && [ "$state" = normal ]; then
		affinity 0f
		state=soft
		say "$T C >= $SOFT: tasks kept on cpu0-3"
	elif [ "$T" -lt "$CLEAR" ] && [ "$state" = soft ]; then
		affinity ff
		state=normal
		say "$T C < $CLEAR: all CPUs again"
	fi
	# a soft state keeps new tasks off the big cores too: they inherit it
	# from their parents, but recheck so nothing escapes
	[ "$state" = soft ] && [ $((n = ${n:-0} + 1)) -ge 10 ] && { affinity 0f; n=0; }
	sleep 1
done
