#!/bin/bash
# scripts/zhotplug-test.sh: CPU hotplug on the S10 5G through zrn-cpuidle-bx's
# `hotplug` (sleep step 4a-0), as root. Turns hotplug on, then takes each
# given CPU offline and back, one at a time, and logs the counters and the
# kernel log. Default order: 3 (little), 5 (A75), 4, 7 (M4; 6 stays).
# A core that does not come back stays offline until reboot (harmless).
# CLUSTER=1: after that, the M4 pair as a whole (hp_cluster=1): 6 then 7
# offline (7 takes the cluster off with the CPD hint and CPU_OFF level 1,
# expect lvl1=1 state=16842752 = 0x01010000), then 7 and 6 back.
# Log: /var/lib/zsleep/hp-<time>.log. A hang: TWRP -> Reboot System.
set -u
P=/sys/module/zrn_cpuidle_bx/parameters
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
[ -w $P/hotplug ] || { echo "zrn-cpuidle-bx has no hotplug (old module?)"; exit 1; }
mkdir -p /var/lib/zsleep
LOG=/var/lib/zsleep/hp-$(date +%m%d-%H%M%S).log
exec > >(tee "$LOG") 2>&1
say() { echo "[$(date +%T.%N | cut -c1-12)] $*"; sync; }
cnt() { echo "out=$(cat $P/hp_out) in=$(cat $P/hp_in) refused=$(cat $P/hp_refused) mismatch=$(cat $P/hp_mismatch) bid_lost=$(cat $P/bid_lost) cl2_off=$(cat $P/hp_cl2_off 2>/dev/null) cl2_on=$(cat $P/hp_cl2_on 2>/dev/null) lvl1=$(cat $P/hp_cl2_lvl1 2>/dev/null) state=$(cat $P/hp_cl2_state 2>/dev/null)"; }

echo "zhotplug-test: start" > /dev/kmsg
echo 1 > $P/hotplug || { say "hotplug on failed"; exit 1; }
say "hotplug=$(cat $P/hotplug) online=$(cat /sys/devices/system/cpu/online) $(cnt)"
for c in ${@:-3 5 4 7}; do
	f=/sys/devices/system/cpu/cpu$c/online
	say "cpu$c offline"
	echo 0 > $f; say "  rc=$? online=$(cat /sys/devices/system/cpu/online) $(cnt)"
	sleep 2
	say "cpu$c online"
	echo 1 > $f; rc=$?
	say "  rc=$rc online=$(cat /sys/devices/system/cpu/online) $(cnt)"
	[ "$(cat $f)" = 1 ] || { say "cpu$c did not come back: stopping"; break; }
	sleep 1
done
if [ "${CLUSTER:-0}" = 1 ]; then
	echo 0x$(awk '$3=="psci_ops"{print $1}' /proc/kallsyms) > $P/psci_ops
	echo 0x$(awk '$3=="psci_0_2_cpu_off"{print $1}' /proc/kallsyms) > $P/psci_cpu_off
	echo 1 > $P/hp_cluster || { say "hp_cluster refused (old module? see dmesg)"; exit 1; }
	say "cluster test: hp_cluster=$(cat $P/hp_cluster)"
	for c in 6 7; do
		say "cpu$c offline"; echo 0 > /sys/devices/system/cpu/cpu$c/online
		say "  rc=$? online=$(cat /sys/devices/system/cpu/online) $(cnt)"
		sleep 1
	done
	sleep 2
	for c in 7 6; do
		say "cpu$c online"; echo 1 > /sys/devices/system/cpu/cpu$c/online
		say "  rc=$? online=$(cat /sys/devices/system/cpu/online) $(cnt)"
		sleep 1
	done
	echo 0 > $P/hp_cluster
fi
say "--- dmesg from the start marker ---"
dmesg | sed -n '/zhotplug-test: start/,$p' | grep -v 'mif: LNK' | tail -60
say "log: $LOG"
