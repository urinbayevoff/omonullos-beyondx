#!/bin/sh
# zsicd-trial: try SICD (zrn-cpuidle-bx.ko's sicd, power/idle/beyondx/
# zrn-sicd-bx.c) a little at a time. Run as root, with the cable in and
# someone at the phone: a hang needs Vol Down + Power for ~7 s.
#
#	zsicd-trial.sh TRIES [SECONDS]
#	IDLE_MASK=0|1|2 zsicd-trial.sh ...	(the module's idle_mask, if it has one)
#	SCREEN_ON_AT=t zsicd-trial.sh 0 ...	turn the screen on at t s (the display
#		guard must switch SICD off by itself: disp_blocked 1, sicd N)
#	CPD=1|2|3 [CPD_TRIES=n] zsicd-trial.sh ...	cluster power down too (mask:
#		1 cores 0-5, 2 cores 6-7); SICD=0 tries CPD alone
#
# Turns the screen off through ztMETRO's control pipe, allows TRIES attempts
# (0 = no limit) for SECONDS (default 20), switches SICD off again, turns
# the screen back on and prints the counters, the battery current before and
# during, and the wake sources. Everything also goes to
# /var/lib/zdrain/sicd-<time>.txt, written and synced before each step.
# Added for OmonullOS, November 2026.

N=${1:?usage: zsicd-trial.sh TRIES [SECONDS]}
SECS=${2:-20}
P=/sys/module/zrn_cpuidle_bx/parameters
CTL=/run/user/1000/ztmetro.ctl
OUT=/var/lib/zdrain/sicd-$(date +%Y%m%d-%H%M%S).txt
mkdir -p /var/lib/zdrain
[ -e $P/sicd ] || { echo "zsicd-trial: the module has no SICD (old zrn-cpuidle-bx.ko?)"; exit 1; }
BUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* | head -n1); BUS=${BUS##*/i2c-}

cur() {	# battery mA, + = charging
	set -- $(i2ctransfer -y "$BUS" w1@0x36 0x0a r2 2>/dev/null)
	[ $# -eq 2 ] || return 1
	v=$(($1 | ($2 << 8))); [ $v -ge 32768 ] && v=$((v - 65536))
	echo $((v * 3125 / 10000))
}
say() { echo "$*" | tee -a $OUT; sync; }
counters() {
	for f in attempts entries early last_error last_sleep_us max_sleep_us last_core \
		 last_wake_stat last_wake2_stat last_sys_ctrl last_grp4_pend last_grp26_pend early_hw \
		 c2_returned c2_last_ret blocked_cpu blocked_short last_block tries \
		 cpd cpd_tries cpd_attempts cpd_entries cpd_early cpd_sibling_up cpd_last_error \
		 sicd_blocked_cpd drcg_at_load disp_guard disp_blocked disp_base disp_win sicd; do
		printf '%s=%s ' $f "$(cat $P/$f 2>/dev/null)"
	done
}
off() {
	echo 0 > $P/sicd; [ -e $P/cpd ] && echo 0 > $P/cpd
	timeout 2 sh -c "echo 'screen on' > $CTL"
}
trap off EXIT INT TERM

[ -n "$IDLE_MASK" ] && [ -e $P/idle_mask ] && echo "$IDLE_MASK" > $P/idle_mask
say "zsicd-trial $(date +%T): sicd ${SICD:-1} tries $N, cpd ${CPD:-0} tries ${CPD_TRIES:-0}, $SECS s, idle_mask $(cat $P/idle_mask 2>/dev/null); at load: $(cat $P/at_load)"
say "before: $(counters)"
timeout 2 sh -c "echo 'screen off' > $CTL"
sleep 5
say "battery before: $(cur) mA"
echo "$N" > $P/tries
if [ -n "$CPD" ]; then
	[ -e $P/cpd ] || { say "zsicd-trial: the module has no cpd"; exit 1; }
	echo "${CPD_TRIES:-0}" > $P/cpd_tries
fi
sync
[ -n "$CPD" ] && echo "$CPD" > $P/cpd
if [ "${SICD:-1}" = 1 ] && ! echo 1 > $P/sicd; then
	say "zsicd-trial: sicd=1 refused (display guard: screen not off, or no display window)"; exit 1
fi
t=0; l=""
while [ $t -lt "$SECS" ]; do
	sleep 2; t=$((t + 2)); c=$(cur); l="$l $c"
	if [ -n "$SCREEN_ON_AT" ] && [ $t -eq "$SCREEN_ON_AT" ]; then
		echo "t=$t screen on (guard test), sicd before: $(cat $P/sicd)" >> $OUT; sync
		timeout 2 sh -c "echo 'screen on' > $CTL"
	fi
	# one line per step, synced: after a hang the file shows how far it got
	echo "t=$t ${c}mA $(cat $P/attempts)/$(cat $P/entries) sicd, cpd $(cat $P/cpd_attempts 2>/dev/null)/$(cat $P/cpd_entries 2>/dev/null) sys_ctrl $(cat $P/last_sys_ctrl) wake $(cat $P/last_wake_stat) sicd $(cat $P/sicd) disp_blocked $(cat $P/disp_blocked 2>/dev/null)" >> $OUT; sync
done
echo 0 > $P/sicd; [ -e $P/cpd ] && echo 0 > $P/cpd
say "battery during:$l mA"
say "after: $(counters)"
[ -e $P/last_idle_ip ] && say "idle-ip: $(cat $P/last_idle_ip)"
off
trap - EXIT
dmesg | tail -5 | tee -a $OUT
