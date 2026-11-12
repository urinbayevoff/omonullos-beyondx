#!/bin/sh
# zsicd-watch: real-life SICD trial, what zgov would do later. Waits for the
# screen to go off (power key), switches CPD of cores 6-7 and SICD on, logs
# the counters and battery current every 10 s, and stops when the display
# guard switches SICD off at screen-on (or after MAX seconds). Run detached
# as root so Claude Code can be closed:
#	sudo systemd-run --unit zwatch sh ~/omonullos/scripts/zsicd-watch.sh [MAX]
# lpm must already be written (zlpm.sh 64, 32, 16, 1, 2 after each boot).
# Log: /var/lib/zdrain/watch-<time>.txt, synced every line.
# Added for OmonullOS, November 2026.
MAX=${1:-900}
P=/sys/module/zrn_cpuidle_bx/parameters
DPMS=/sys/class/drm/card2-DSI-1/dpms
OUT=/var/lib/zdrain/watch-$(date +%Y%m%d-%H%M%S).txt
mkdir -p /var/lib/zdrain
BUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* | head -n1); BUS=${BUS##*/i2c-}
cur() {
	set -- $(i2ctransfer -y "$BUS" w1@0x36 0x0a r2 2>/dev/null)
	[ $# -eq 2 ] || return 1
	v=$(($1 | ($2 << 8))); [ $v -ge 32768 ] && v=$((v - 65536))
	echo $((v * 3125 / 10000))
}
say() { echo "$(date +%T) $*" >> $OUT; sync; }
stop() { echo 0 > $P/sicd; echo 0 > $P/cpd; say "stopped: $(cat $P/attempts)/$(cat $P/entries) sicd, disp_blocked $(cat $P/disp_blocked), max_sleep_us $(cat $P/max_sleep_us)"; }
trap stop EXIT INT TERM
[ -e $P/disp_guard ] && [ "$(cat $P/disp_guard)" = Y ] || { say "no display guard: not starting"; exit 1; }
say "waiting for screen off (lpm $(cat $P/lpm), $(cat $P/attempts)/$(cat $P/entries) so far)"
while [ "$(cat $DPMS)" != Off ]; do sleep 1; done
sleep 3
echo 2 > $P/cpd
echo 0 > $P/tries
if ! echo 1 > $P/sicd; then say "sicd=1 refused"; exit 1; fi
say "screen off: SICD on, disp_base $(cat $P/disp_base)"
t=0
while [ $t -lt "$MAX" ]; do
	sleep 10; t=$((t + 10))
	say "t=$t $(cur)mA $(cat $P/attempts)/$(cat $P/entries) sicd $(cat $P/sicd) max $(cat $P/max_sleep_us) wake $(cat $P/last_wake_stat) dpms $(cat $DPMS)"
	[ "$(cat $P/sicd)" = N ] && { say "guard switched SICD off (screen on)"; break; }
	[ "$(cat $DPMS)" = On ] && { say "screen on"; break; }
done
