#!/bin/sh
# Panel DCS test for bx-decon, run on the phone while someone watches the
# panel. Two rounds, about 50 s in all:
#   1. panel_dcs=1: screen off (black frame + DCS display off); 5 s later
#      ztMETRO's last frame is put back on DECON by hand: a panel that took
#      display off stays dark; 10 s later screen on (display on)
#   2. the same with panel_sleep=1 (enter / exit sleep mode as well)
# zdcs-test.sh nosleep: round 1 three times, then panel_dcs back to 0.
# Added for OmonullOS, October 2026.
P=/sys/module/bx_decon/parameters C=/run/user/1000/ztmetro.ctl
G=0x19071000 D=0x19030000
say() { echo "zboot| dcs test: $*" > /dev/kmsg; echo "$(date +%T) $*"; }
round() {
	echo "screen on" > $C; sleep 5
	buf=$(devmem $((G + 0x40)) 32)
	echo 1 > $P/panel_dcs; echo $1 > $P/panel_sleep
	say "round $2: screen off (panel_sleep=$1), ztMETRO buffer $buf"
	echo "screen off" > $C; sleep 5
	devmem $((G + 0x40)) 32 $buf
	devmem $((D + 0x60)) 32 $(( $(devmem $((D + 0x60)) 32) | 0x20 ))
	say "round $2: lock screen frame pushed while off"
	sleep 10
	echo "screen on" > $C
	say "round $2: screen on"
}
if [ "$1" = nosleep ]; then
	# display off/on only, three times: no sleep in / out (2026-10-17: sleep
	# out without the panel's setup left dashes at the bottom of the panel)
	round 0 1; sleep 5; round 0 2; sleep 5; round 0 3
	echo 0 > $P/panel_dcs
else
	round 0 1
	sleep 5
	round 1 2
fi
say "done (panel_dcs=$(cat $P/panel_dcs) panel_sleep=$(cat $P/panel_sleep))"
