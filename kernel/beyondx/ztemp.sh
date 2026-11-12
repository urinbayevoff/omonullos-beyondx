#!/bin/sh
# ztemp: the Galaxy S10 5G's SoC temperatures, read from the Exynos 9820 TMU
# at 0x10090000 with devmem. Read-only: no TMU register is written (the
# thermal trips and their throttling are not set up; nothing limits heat yet,
# so keep loads short until a thermal driver is in).
#
# Added for OmonullOS, October 2026. Tested on the phone (bx13, from /tmp
# over telnet). Installed as /bin/ztemp.
#
# Sources: Samsung's drivers/thermal/samsung/exynos_tmu.c (CONFIG_SOC_EXYNOS9820)
# and the stock tree's tmu nodes (stock/live/beyondx-twrp-boot.dts):
#   TRIMINFO(s) = 0x00 + 4*s: bits 8:0 the code at 25 C, 17:9 at 85 C;
#     TRIMINFO(0) bit 23 = two-point calibration (else one-point)
#   CURRENT_TEMP: 0x84 + 4*(s/2), bits 16*(s%2) + 8:0
#   zones: LITTLE sensor 4, MID 5, BIG 6-10, G3D 1-4 and 9, ISP 8
# two-point: T = (code - c25) * 60 / (c85 - c25) + 25

B=0x10090000

tenths() {	# tenths <sensor>: temperature in 0.1 C
	t=$(devmem $((B + 4 * $1)) 32)
	c25=$((t & 0x1ff))
	c85=$(((t >> 9) & 0x1ff))
	w=$(devmem $((B + 0x84 + 4 * ($1 / 2))) 32)
	code=$(((w >> (16 * ($1 % 2))) & 0x1ff))
	if [ $((($(devmem $B 32) >> 23) & 1)) -eq 1 ] && [ $c85 -gt $c25 ]; then
		echo $(((code - c25) * 600 / (c85 - c25) + 250))
	else
		echo $(((code - c25 + 25) * 10))
	fi
}

fmt() { echo "$(($1 / 10)).$(($1 % 10))"; }

zone() {	# zone <name> <sensor>...: the hottest of its sensors
	name=$1; shift
	max=-1000
	for s; do
		v=$(tenths "$s")
		[ "$v" -gt "$max" ] && max=$v
	done
	printf '%s %s C  ' "$name" "$(fmt "$max")"
}

zone LITTLE 4
zone MID 5
zone BIG 6 7 8 9 10
zone G3D 1 2 3 4 9
zone ISP 8
echo
if [ "$1" = -a ]; then
	for s in 0 1 2 3 4 5 6 7 8 9 10; do
		printf 's%d %s  ' "$s" "$(fmt "$(tenths "$s")")"
	done
	echo
fi
