#!/bin/bash
# scripts/zsleep-4a.sh: system sleep step 4a on the S10 5G (beyondx), as root.
# The first Linux suspend on this phone, dry run: loads zrn-system-sleep-bx
# with the suspend backend (enable_deep=1, dry_run=1), then
# `echo mem > /sys/power/state`. Processes freeze, drivers suspend, CPUs 1-7
# go offline, the backend refuses at enter without touching a register
# (-EAGAIN) and everything resumes. No wake source needed.
# Needs a fresh boot (the module must not be loaded yet; no rmmod).
# WIFI_UNBIND=1: brcmfmac does not answer D3_INFORM on this phone (12:32
# run: "Timeout on response for entering D3 substate", suspend aborted), so
# unbind it from 0000:01:00.0 before the suspend and bind it again after.
# HOTPLUG=1: zrn-cpuidle-bx hotplug=1 hp_cluster=1 first, so CPUs 1-7 come
# back after the suspend (12:45 run hung with them offline and no wake lines).
# pm_async=0 during the run (restored after). CONSOLE_DEBUG=1: console_suspend=N
# too; that forces the VT switch, which takes the seat from ztmetro (13:24).
# STEP=4b: dry_run=0 for this run (prepare_only stays 1): the PMU prepare and the
# early-wakeup restore run in the real suspend finisher, still no SMC; dry_run
# goes back to 1 after.
# STEP=4c: the real SYS_SLEEP (dry_run=0, prepare_only=0): the firmware call.
# Waits up to 60 s for the screen to go off (press power once), then sleeps;
# only the power key wakes it. Both parameters go back to 1 after.
# USB_DETACH=1 (default): the zboot gadget off its UDC for the run (13:59 dwc3
# suspend -EAGAIN with no cable: gadget soft disconnect failed).
# TOUCH=1 (default): s6sy761 does not come back from a suspend (resume -19,
# 13:24), so `ztouch off` before and `ztouch on` after (zacpm rails: owner's «ha»).
# Log: /var/lib/zsleep/4a-<time>.log. A hang: TWRP -> Reboot System.
set -u
WIFI_PCI=0000:01:00.0
WIFI_UNBIND=${WIFI_UNBIND:-0}
HOTPLUG=${HOTPLUG:-0}
TOUCH=${TOUCH:-1}
USB_DETACH=${USB_DETACH:-1}
STEP=${STEP:-4a}
# WL_OFF=1: Wi-Fi chip off through the sleep (sleep-bx wl_off, needs WIFI_UNBIND=1)
WL_OFF=${WL_OFF:-0}
GADGET=/sys/kernel/config/usb_gadget/zboot
CONSOLE_DEBUG=${CONSOLE_DEBUG:-0}
IP=/sys/module/zrn_cpuidle_bx/parameters
KO=/home/alarm/omonullos-src/build/sleep-bx/zrn-system-sleep-bx.ko
P=/sys/module/zrn_system_sleep_bx/parameters
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
mkdir -p /var/lib/zsleep
LOG=/var/lib/zsleep/4a-$(date +%m%d-%H%M%S).log
exec > >(tee "$LOG") 2>&1
say() { echo "[$(date +%T)] $*"; }

if [ -d "$P" ]; then
	# a second run in the same boot is fine as long as enable_deep is on
	if [ "$(cat $P/enable_deep)" != Y ]; then
		say "module already loaded without enable_deep: reboot first"; exit 1
	fi
else
	SUS=$(awk '$3=="cpu_suspend"{print $1}' /proc/kallsyms)
	RES=$(awk '$3=="cpu_resume"{print $1}' /proc/kallsyms)
	[ -n "$SUS" ] && [ -n "$RES" ] || { say "no cpu_suspend/cpu_resume in kallsyms"; exit 1; }
	say "insmod enable_deep=1 suspend_enter=0x$SUS resume=0x$RES"
	sync
	insmod "$KO" enable_deep=1 suspend_enter=0x"$SUS" resume=0x"$RES" || exit 1
fi
if [ "$WL_OFF" = 1 ]; then
	[ -e $P/wl_off ] || { say "loaded module has no wl_off: reboot first"; exit 1; }
	[ "$WIFI_UNBIND" = 1 ] || { say "WL_OFF=1 needs WIFI_UNBIND=1"; exit 1; }
fi
[ -e $P/wl_off ] && echo "$WL_OFF" > $P/wl_off
if [ "$HOTPLUG" = 1 ]; then
	echo 0x$(awk '$3=="psci_ops"{print $1}' /proc/kallsyms) > $IP/psci_ops
	echo 0x$(awk '$3=="psci_0_2_cpu_off"{print $1}' /proc/kallsyms) > $IP/psci_cpu_off
	echo 1 > $IP/hotplug && echo 1 > $IP/hp_cluster || { say "no hotplug/hp_cluster in zrn-cpuidle-bx"; exit 1; }
	say "hotplug=$(cat $IP/hotplug) hp_cluster=$(cat $IP/hp_cluster)"
fi
say "dry_run=$(cat $P/dry_run) prepare_only=$(cat $P/prepare_only)"
[ "$(cat $P/dry_run)" = Y ] || { say "dry_run is off: refusing (left over from a broken run?)"; exit 1; }
[ "$(cat $P/prepare_only)" = Y ] || { say "prepare_only is off: refusing (left over from a broken run?)"; exit 1; }
say "mem_sleep: $(cat /sys/power/mem_sleep)"
grep -q '\[deep\]' /sys/power/mem_sleep || echo deep > /sys/power/mem_sleep
say "mem_sleep now: $(cat /sys/power/mem_sleep)"
say "suspend_stats before: success=$(cat /sys/power/suspend_stats/success) fail=$(cat /sys/power/suspend_stats/fail)"
# serial device suspend and a live console, so a hang shows which driver it was in
CS=/sys/module/printk/parameters/console_suspend
OLD_CS=$(cat $CS 2>/dev/null); OLD_ASYNC=$(cat /sys/power/pm_async 2>/dev/null)
[ "$CONSOLE_DEBUG" = 1 ] && echo N > $CS 2>/dev/null; echo 0 > /sys/power/pm_async 2>/dev/null
say "console_suspend=$(cat $CS 2>/dev/null) pm_async=$(cat /sys/power/pm_async 2>/dev/null)"
if [ "$STEP" = 4c ]; then
	DPMS=$(ls /sys/class/drm/card*-DSI-1/dpms | head -1)
	say "4c: waiting for the screen to go off (press power once)"
	for i in $(seq 60); do [ "$(cat $DPMS)" = Off ] && break; sleep 1; done
	[ "$(cat $DPMS)" = Off ] || { say "screen still on after 60 s: not sleeping"; echo s2idle > /sys/power/mem_sleep; exit 1; }
	say "screen off; sleeping in 3 s, wake with the power key"
	sleep 3
	echo 0 > $P/dry_run && echo 0 > $P/prepare_only || { say "4c parameters refused"; echo 1 > $P/dry_run; echo 1 > $P/prepare_only; exit 1; }
	say "STEP 4c: dry_run=$(cat $P/dry_run) prepare_only=$(cat $P/prepare_only) post_fixups=$(cat $P/post_fixups)"
	sync
fi
if [ "$STEP" = 4b ]; then
	echo 0 > $P/dry_run || { say "dry_run=0 refused"; exit 1; }
	say "STEP 4b: dry_run=$(cat $P/dry_run) prepare_only=$(cat $P/prepare_only) attempts=$(cat $P/attempts)"
fi
echo "zsleep-4a: dry-run suspend start" > /dev/kmsg
sync; sleep 1
say "echo mem > /sys/power/state (expect EAGAIN: Resource temporarily unavailable)"
if [ "$WIFI_UNBIND" = 1 ]; then
	say "unbind brcmfmac from $WIFI_PCI"
	echo $WIFI_PCI > /sys/bus/pci/drivers/brcmfmac/unbind
	sleep 1
fi
UDC=""
if [ "$USB_DETACH" = 1 ] && [ -w $GADGET/UDC ]; then
	UDC=$(cat $GADGET/UDC)
	[ -n "$UDC" ] && { echo "" > $GADGET/UDC; say "usb gadget detached from $UDC rc=$?"; }
fi
if [ "$TOUCH" = 1 ]; then
	say "ztouch off"; /usr/local/bin/ztouch off; say "  rc=$?"
fi
T0=$(date +%s.%N)
echo mem > /sys/power/state; RC=$?
T1=$(date +%s.%N)
say "returned rc=$RC after $(awk "BEGIN{print $T1 - $T0}") s"
# touch first: 4c (12.11 14:32) waited ~100 s for a dead Wi-Fi before it
if [ "$TOUCH" = 1 ]; then
	say "ztouch on"; /usr/local/bin/ztouch on; say "  rc=$? driver: $(ls /sys/bus/i2c/drivers/s6sy761/ | grep -c 1-0048)"
fi
if [ "$WIFI_UNBIND" = 1 ]; then
	# 13:40 (37 s after the 13:39 run): the rebound brcmfmac read a stale RX ring
	# (msgtype 0) and oopsed in its IRQ thread; Wi-Fi dead until reboot. The
	# chip kept its old state across unbind/suspend: function-level reset first.
	say "reset $WIFI_PCI ($(cat /sys/bus/pci/devices/$WIFI_PCI/reset_method))"
	echo 1 > /sys/bus/pci/devices/$WIFI_PCI/reset; say "reset rc=$?"
	say "bind brcmfmac to $WIFI_PCI"
	echo $WIFI_PCI > /sys/bus/pci/drivers/brcmfmac/bind; say "bind rc=$?"
	for i in $(seq 30); do
		nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep -q '^wlp1s0:connected$' && break
		sleep 1
	done
	say "wifi after ${i} s: $(nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep wlp1s0)"
fi
[ -n "$UDC" ] && { echo "$UDC" > $GADGET/UDC; say "usb gadget back on $UDC rc=$?"; }
if [ "$STEP" = 4c ]; then
	echo 1 > $P/prepare_only; echo 1 > $P/dry_run
	say "4c: pdn_applied=$(cat $P/pdn_applied) attempts=$(cat $P/attempts) entries=$(cat $P/entries) last_error=$(cat $P/last_error) smc=$(cat $P/last_smc) restore=$(cat $P/last_restore_error) wake=$(cat $P/wake_stat)/$(cat $P/wake_stat2) grp4=$(cat $P/grp4_at_smc) post_lost=$(cat $P/post_lost) wl_off=$(cat $P/wl_off 2>/dev/null) wl_dropped=$(cat $P/wl_dropped 2>/dev/null) back: dry_run $(cat $P/dry_run) prepare_only $(cat $P/prepare_only)"
fi
if [ "$STEP" = 4b ]; then
	echo 1 > $P/dry_run
	say "4b: attempts=$(cat $P/attempts) entries=$(cat $P/entries) last_error=$(cat $P/last_error) restore=$(cat $P/last_restore_error) saved=$(cat $P/saved_rows) g3d=$(cat $P/g3d_status) wake=$(cat $P/wake_stat)/$(cat $P/wake_stat2) dry_run back $(cat $P/dry_run)"
fi
echo "zsleep-4a: dry-run suspend end rc=$RC" > /dev/kmsg
[ -n "$OLD_CS" ] && echo $OLD_CS > $CS 2>/dev/null; [ -n "$OLD_ASYNC" ] && echo $OLD_ASYNC > /sys/power/pm_async
# back to s2idle so nothing else lands in our backend by accident
echo s2idle > /sys/power/mem_sleep
say "mem_sleep restored: $(cat /sys/power/mem_sleep)"
for f in success fail failed_freeze failed_prepare failed_suspend failed_suspend_noirq \
	 failed_resume failed_resume_noirq last_failed_dev last_failed_errno last_failed_step; do
	[ -r /sys/power/suspend_stats/$f ] && say "stats $f: $(cat /sys/power/suspend_stats/$f | tr '\n' ' ')"
done
say "online CPUs: $(cat /sys/devices/system/cpu/online)"
[ "$HOTPLUG" = 1 ] && say "hp out=$(cat $IP/hp_out) in=$(cat $IP/hp_in) refused=$(cat $IP/hp_refused) cl2_off=$(cat $IP/hp_cl2_off) cl2_on=$(cat $IP/hp_cl2_on) lvl1=$(cat $IP/hp_cl2_lvl1)"
say "--- dmesg from the start marker (no mif LNK) ---"
dmesg | sed -n '/zsleep-4a: dry-run suspend start/,$p' | grep -v 'mif: LNK' | tail -150
say "--- health ---"
systemctl is-active zril zcell NetworkManager 2>&1 | tr '\n' ' '; echo
ping -c2 -W3 1.1.1.1 2>&1 | tail -1
cat /sys/class/drm/card*-DSI-1/dpms 2>/dev/null
say "log: $LOG"
