#!/bin/sh
# zwlan-guard: bring the Galaxy S10 5G's Wi-Fi back when the BCM4375 drops
# out while the phone is awake, without a reboot.
#
# Added for OmonullOS, November 2026. 16.11 09:53 (also 06:13, 15.11 21:46):
# every ring index brcmfmac read was 0xffff ("bad w_ptr 65535", the guard of
# patch-brcmfmac-ordering.py), then only "query_dcmd: Timeout" until a
# reboot. Stock Android recovers the same way: a HANG event, the HAL powers
# the module down and up. Here: unbind brcmfmac, remove 0000:01:00.0, the
# root complex powers the module off and on and trains the link again
# (pcie-exynos9820 relink), rescan, brcmfmac probes it afresh.
#   zwlan-guard recover    once, now
#   zwlan-guard watch      follow the kernel log, recover on the signs
# At most $MAX recoveries an hour; off switch /etc/zwlan-guard.off.
DEV=0000:01:00.0
BRIDGE=0000:00:00.0
RELINK=/sys/module/pcie_exynos9820/parameters/relink
LOG=/var/lib/zsleep/zwlan-guard.log
MAX=${MAX:-3}

say() {
	mkdir -p "${LOG%/*}"
	echo "$(date '+%F %T') $*" | tee -a $LOG
}
bound() { [ -e /sys/bus/pci/devices/$DEV/driver ]; }

recover() {
	exec 9>/run/zwlan-guard.lock
	flock -n 9 || { say "recover: another one runs"; return 1; }
	[ -w $RELINK ] || { say "recover: no relink (old pcie-exynos9820)"; return 1; }
	say "recover: start ($(cat $RELINK 2>/dev/null))"
	if bound; then
		timeout 30 sh -c "echo $DEV > /sys/bus/pci/drivers/brcmfmac/unbind" ||
			say "recover: unbind did not finish"
	fi
	[ -e /sys/bus/pci/devices/$DEV ] && echo 1 > /sys/bus/pci/devices/$DEV/remove
	sleep 0.5
	if ! echo 1 > $RELINK; then
		say "recover: relink failed: $(cat $RELINK 2>/dev/null)"
		return 1
	fi
	echo 1 > /sys/bus/pci/devices/$BRIDGE/rescan
	for i in $(seq 1 20); do bound && break; sleep 1; done
	if bound; then
		say "recover: Wi-Fi bound again ($(cat $RELINK 2>/dev/null))"
	else
		say "recover: Wi-Fi still DEAD"
		return 1
	fi
}

watch() {
	# one line per sign; 3 firmware timeouts within 60 s count as one sign
	journalctl -k -f -n 0 -o cat | while read -r line; do
		case $line in
		*"brcmfmac"*"bad w_ptr 65535"*) sign="rings read all ones" ;;
		*"brcmf_msgbuf_query_dcmd: Timeout"*)
			now=$(date +%s)
			[ $((now - ${t0:-0})) -gt 60 ] && { t0=$now; n=0; }
			n=$((n + 1))
			[ $n -ge 3 ] || continue
			sign="firmware silent"; n=0 ;;
		*) continue ;;
		esac
		[ -e /etc/zwlan-guard.off ] && { say "sign ($sign), off switch set"; continue; }
		now=$(date +%s)
		# recoveries in the last hour
		recent=$(awk -v s=$((now - 3600)) '$1 >= s' /run/zwlan-guard.times 2>/dev/null | wc -l)
		if [ "$recent" -ge "$MAX" ]; then
			say "sign ($sign), but $recent recoveries in the last hour: waiting"
			continue
		fi
		echo "$now" >> /run/zwlan-guard.times
		say "sign: $sign"
		sleep 2	# let the driver's own messages settle
		recover
	done
}

case "$1" in
recover) recover ;;
watch) say "watch: start"; watch ;;
*) echo "usage: zwlan-guard recover|watch" >&2; exit 2 ;;
esac
