#!/bin/bash
# scripts/zsleepd.sh: automatic system sleep (SYS_SLEEP) on the S10 5G (beyondx), as root.
# Installed as /usr/local/bin/zsleepd. The steps of scripts/zsleep-4a.sh STEP=4c
# WIFI_UNBIND=1 HOTPLUG=1, in a loop: once the screen has been off for DELAY s
# (RESLEEP s after a wake that left it off: an SMS), with no call, no sound, no
# USB cable and nothing busy, the phone goes to sleep; the power key, a call or
# an SMS (CP, wake bit 8) wakes it, and so does a charger plugged in (MAX77705's
# INTB, gpa1-6: chg_arm). Never on a charger (see busy). Tested by hand 14.11 (three 4c runs, amps
# set up again by snd-zrn-abox-bx, zcell lights the screen for a call).
# No timer wakes it: alarms and timers do not run while it sleeps (owner: none needed).
# Off switch: /etc/zsleepd.off (looked at every check). Log: /var/lib/zsleep/zsleepd.log,
# state now: /run/zsleepd.status.
set -u
DELAY=${DELAY:-30}
RESLEEP=${RESLEEP:-20}
TICK=5
WIFI_PCI=0000:01:00.0
WIFI_BRIDGE=0000:00:00.0	# its root port
GADGET=/sys/kernel/config/usb_gadget/zboot
# the image keeps it with the other boot modules (updates put it there too)
KO=${KO:-/usr/local/lib/zboot/tools/zrn-system-sleep-bx.ko}
[ -e "$KO" ] || KO=/root/tools/zrn-system-sleep-bx.ko
P=/sys/module/zrn_system_sleep_bx/parameters
IP=/sys/module/zrn_cpuidle_bx/parameters
AMP=/sys/module/snd_zrn_abox_bx/parameters
OFF=/etc/zsleepd.off
LOG=/var/lib/zsleep/zsleepd.log
STATUS=/run/zsleepd.status
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
mkdir -p /var/lib/zsleep

say() {
	[ -f $LOG ] && [ "$(stat -c %s $LOG)" -gt 1048576 ] && mv -f $LOG $LOG.1
	echo "$(date '+%F %T') $*" >> $LOG
}

# ---- once: the backend module and CPU hotplug -------------------------------
if [ -d $P ]; then
	[ "$(cat $P/enable_deep)" = Y ] || { say "module loaded without enable_deep: reboot first"; exit 1; }
else
	SUS=$(awk '$3=="cpu_suspend"{print $1}' /proc/kallsyms)
	RES=$(awk '$3=="cpu_resume"{print $1}' /proc/kallsyms)
	[ -n "$SUS" ] && [ -n "$RES" ] || { say "no cpu_suspend/cpu_resume in kallsyms"; exit 1; }
	insmod "$KO" enable_deep=1 suspend_enter=0x"$SUS" resume=0x"$RES" || { say "insmod $KO failed"; exit 1; }
	say "insmod enable_deep=1"
fi
# zrn-cpuidle.service loads it after multi-user.target, which can come late
# (15.11: network-online waited 100 s, the module came 12 s after the 90 s
# wait; zsleepd.service is not ordered after it, see the unit): wait for it
n=0
while [ ! -e $IP/hotplug ] && [ $n -lt 300 ]; do sleep 2; n=$((n + 2)); done
[ $n -eq 0 ] || say "waited ${n} s for zrn-cpuidle-bx"
echo 0x$(awk '$3=="psci_ops"{print $1}' /proc/kallsyms) > $IP/psci_ops
echo 0x$(awk '$3=="psci_0_2_cpu_off"{print $1}' /proc/kallsyms) > $IP/psci_cpu_off
echo 1 > $IP/hotplug && echo 1 > $IP/hp_cluster || { say "no hotplug/hp_cluster in zrn-cpuidle-bx"; exit 1; }
[ "$(cat $P/dry_run)" = Y ] && [ "$(cat $P/prepare_only)" = Y ] || { say "dry_run/prepare_only off: left over from a broken run, refusing"; exit 1; }
# a charger plugged in wakes it (15.11): the MAX77705's INTB (gpa1-6, EINT bit
# 14; its USB-C attach interrupt is unmasked) held by zgpio as a falling-edge
# EINT, its interrupts read clear first so the line is high. Off with CHG_WAKE=0.
CHG_WAKE=${CHG_WAKE:-1}
ZGPIO=/usr/local/bin/zgpio
I2C=0
EINT_KEY=$(( 0xffffffff & ~(1 << 20) ))		# the power key only (the module's default)
EINT_CHG=$(( 0xffffffff & ~(1 << 20) & ~(1 << 14) ))
chg_pid=""
DPMS=$(ls /sys/class/drm/card*-DSI-1/dpms 2>/dev/null | head -1)
[ -n "$DPMS" ] || { say "no DSI-1 dpms"; exit 1; }

cycles=0; slept=0; fails=0
ASYNC=$(cat /sys/power/pm_async)
say "start: DELAY=$DELAY RESLEEP=$RESLEEP"

screen_off() { [ "$(cat $DPMS)" = Off ]; }

# read clear what the MAX77705 has pending (read-to-clear registers: PMIC
# INTSRC, SYSTEM_INT, CHG_INT, USB-C UIC/CC/PD/VDM; nothing is written), so
# INTB goes high and a plug-in is a new falling edge
max_clear() {
	local r
	i2cget -y $I2C 0x66 0x22 >/dev/null 2>&1; i2cget -y $I2C 0x66 0x24 >/dev/null 2>&1
	i2cget -y $I2C 0x69 0xb0 >/dev/null 2>&1
	for r in 0x02 0x03 0x04 0x05; do i2cget -y $I2C 0x25 $r >/dev/null 2>&1; done
}

# arm the charger's wake: 0 armed, 1 not (left to the power key)
chg_arm() {
	[ "$CHG_WAKE" = 1 ] && [ -x $ZGPIO ] || return 1
	local f=/run/zsleepd.intb l
	max_clear
	: > $f
	$ZGPIO hold 1 6 falling > $f 2>&1 &
	chg_pid=$!
	for l in 1 2 3 4 5 6 7 8 9 10; do grep -q level $f && break; sleep 0.1; done
	if grep -q "level 1" $f; then
		echo $EINT_CHG > $P/eint_wake && return 0
	fi
	say "charger wake not armed: $(head -1 $f)"
	chg_disarm
	return 1
}

chg_disarm() {
	echo $EINT_KEY > $P/eint_wake 2>/dev/null
	[ -n "$chg_pid" ] && kill $chg_pid 2>/dev/null && wait $chg_pid 2>/dev/null
	chg_pid=""
}
wifi_bound() { [ -e /sys/bus/pci/devices/$WIFI_PCI/driver ]; }

# why it must stay awake now; empty: it may sleep
busy() {
	[ -e $OFF ] && { echo "$OFF"; return; }
	screen_off || { echo "screen on"; return; }
	local c; c=$(sed -n 's/^call=//p' /run/ztoscorenetworking/status 2>/dev/null)
	[ -n "$c" ] && [ "$c" != idle ] && { echo "call $c"; return; }
	[ "$(cat $AMP/call 2>/dev/null)" = 0 ] || { echo "call audio"; return; }
	grep -qs RUNNING /proc/asound/card*/pcm*/sub*/status && { echo "sound"; return; }
	local u; u=$(cat /sys/class/udc/*/state 2>/dev/null | head -1)
	[ -n "$u" ] && [ "$u" != "not attached" ] && { echo "usb $u"; return; }
	# a charger: the MAX77705's charger watchdog is kicked only while awake
	# (wdt-pet.sh, every 20 s); asleep it runs out and charging stops (owner
	# 15.11: asleep on the charger, no charge). zlowbatt writes /run/zbatt/online
	# (CHGIN valid) every 10 s.
	[ "$(cat /run/zbatt/online 2>/dev/null)" = 1 ] && { echo "charger"; return; }
	pgrep -x pacman >/dev/null && { echo "pacman"; return; }
	pgrep -f zsleep-4a.sh >/dev/null && { echo "zsleep-4a"; return; }
	systemctl -q is-active zfield-push.service && { echo "zfield push"; return; }
	# a download in the background (Settings > theme): Wi-Fi goes with the sleep
	systemctl -q is-active ztos-media.service zcommons-walls.service 2>/dev/null && { echo "download"; return; }
	systemd-inhibit --list --no-legend --no-pager 2>/dev/null | awk '$NF == "block" && /sleep/' | grep -q . && { echo "inhibitor"; return; }
}

sleep_once() {
	local udc="" t0 t1 rc wake why unbound=0 armed=1 intsrc=""
	echo deep > /sys/power/mem_sleep
	echo 0 > /sys/power/pm_async
	if [ -w $GADGET/UDC ]; then
		udc=$(cat $GADGET/UDC)
		[ -n "$udc" ] && echo "" > $GADGET/UDC
	fi
	/usr/local/bin/ztouch off >/dev/null 2>&1
	# the last look: a key, a call or a sound meanwhile. Wi-Fi goes only after
	# it, last of all: once unbound, giving up loses it (14.11 15:26 and 15:44,
	# the screen lit in that second: FLR + bind "MMIO read failed"; a dry-run
	# suspend did not bring it back and left the 5G modem in D3cold)
	why=$(busy)
	if [ -z "$why" ]; then
		echo $WIFI_PCI > /sys/bus/pci/drivers/brcmfmac/unbind 2>/dev/null; unbound=1
		why=$(busy)
	fi
	if [ -z "$why" ]; then
		chg_arm; armed=$?
		echo 0 > $P/dry_run && echo 0 > $P/prepare_only
		sync
		t0=$(date +%s.%N)
		echo mem > /sys/power/state; rc=$?
		t1=$(date +%s.%N)
		echo 1 > $P/prepare_only; echo 1 > $P/dry_run
		# what woke it: INTSRC set = the MAX77705 (a key leaves it 0)
		intsrc=$(i2cget -y $I2C 0x66 0x22 2>/dev/null)
		chg_disarm
	else
		rc=-1; t0=0; t1=0
	fi
	/usr/local/bin/ztouch on >/dev/null 2>&1
	if [ "$unbound" = 1 ]; then
		# a function-level reset first: the rebound brcmfmac read a stale RX
		# ring and oopsed (12.11 13:40). Not slept, the FLR finds the chip gone
		# (MMIO read failed) but that fails clean; without it, the oops.
		# the chip's own config as it woke (BAR0, command): 0 = it lost power
		local cfg; cfg=$(setpci -s $WIFI_PCI BASE_ADDRESS_0 COMMAND 2>/dev/null | tr '\n' ' ')
		echo 1 > /sys/bus/pci/devices/$WIFI_PCI/reset 2>/dev/null
		echo $WIFI_PCI > /sys/bus/pci/drivers/brcmfmac/bind 2>/dev/null
		if ! wifi_bound; then
			# 15.11 22:21, 16.11 07:48: "MMIO read failed 0xffffffff" after a
			# wake; the reset saved and put back a config the chip had lost.
			# Enumerate it afresh, as at boot.
			say "Wi-Fi bind failed after wake (cfg $cfg): remove + rescan"
			echo 1 > /sys/bus/pci/devices/$WIFI_PCI/remove 2>/dev/null
			sleep 1
			echo 1 > /sys/bus/pci/devices/$WIFI_BRIDGE/rescan 2>/dev/null
			for i in 1 2 3 4 5 6 7 8 9 10; do wifi_bound && break; sleep 1; done
			say "Wi-Fi after rescan: $(wifi_bound && echo bound || echo DEAD)"
			# 17.11 17:14: still DEAD after the rescan. A rescan does not
			# power the chip; zwlan-guard recover cycles WL_REG_ON and
			# retrains the link, which brought it back on 13:11 the same day.
			if ! wifi_bound && [ -x /usr/local/bin/zwlan-guard ]; then
				/usr/local/bin/zwlan-guard recover >/dev/null 2>&1
				say "Wi-Fi after zwlan-guard recover: $(wifi_bound && echo bound || echo DEAD)"
			fi
		fi
	fi
	local wifi=ok; [ -e /sys/bus/pci/devices/$WIFI_PCI/driver ] || wifi=DEAD
	[ -n "$udc" ] && echo "$udc" > $GADGET/UDC
	echo s2idle > /sys/power/mem_sleep
	echo "$ASYNC" > /sys/power/pm_async
	local touch=0; [ -e /sys/bus/i2c/devices/1-0048/driver ] && touch=1	# either driver
	if [ "$rc" = -1 ]; then
		say "not slept: $why (unbound $unbound, touch $touch wifi $wifi)"
		return 2
	fi
	wake=$(cat $P/wake_stat)
	local d; d=$(awk "BEGIN{printf \"%.1f\", $t1 - $t0}")
	cycles=$((cycles + 1))
	if [ "$rc" = 0 ]; then
		slept=$(awk "BEGIN{printf \"%.0f\", $slept + $d}")
		fails=0
		local by=other
		[ "$wake" = 256 ] && by=cp
		[ "$wake" = 4096 ] && { by=key; [ "$armed" = 0 ] && [ -n "$intsrc" ] && [ "$intsrc" != 0x00 ] && by="charger/usb $intsrc"; }
		say "slept ${d} s wake=$wake ($by) chg_wake=$([ "$armed" = 0 ] && echo on || echo off) intsrc=${intsrc:-?} err=$(cat $P/last_error) restore=$(cat $P/last_restore_error) cpus=$(cat /sys/devices/system/cpu/online) touch=$touch wifi=$wifi amps=$(cat $AMP/amp_rescues)/$(cat $AMP/amp_rescue_ret)"
	else
		fails=$((fails + 1))
		say "suspend failed rc=$rc after ${d} s: $(cat /sys/power/suspend_stats/last_failed_dev 2>/dev/null | head -1) $(cat /sys/power/suspend_stats/last_failed_step 2>/dev/null | head -1) (fail $fails)"
	fi
	echo "cycles=$cycles slept_s=$slept fails=$fails last=$(date +%T) wake=$wake" > $STATUS
	return 0
}

off_since=0; woke=0; wifi_said=0
while :; do
	sleep $TICK
	now=$(date +%s)
	if ! screen_off; then
		off_since=0; woke=0
		continue
	fi
	[ "$off_since" = 0 ] && off_since=$now
	wait=$DELAY; [ "$woke" = 1 ] && wait=$RESLEEP
	[ $((now - off_since)) -ge $wait ] || continue
	why=$(busy)
	if [ -n "$why" ]; then
		continue
	fi
	# never with Wi-Fi gone: that suspend hung (14.11 15:2x); awake until a reboot
	if ! wifi_bound; then
		[ "$wifi_said" = 1 ] || say "Wi-Fi has no driver: no sleep until a reboot"
		wifi_said=1
		continue
	fi
	if [ "$fails" -ge 5 ]; then
		say "5 failures in a row: resting 5 min"
		sleep 300; fails=0; continue
	fi
	sleep_once
	# back from sleep (or not slept): count the screen-off time again from now
	off_since=$(date +%s); woke=1
done
