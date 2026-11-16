#!/bin/sh
# ztouch: power the Galaxy S10 5G's touchscreen and bind its driver.
#
# Added for OmonullOS, October 2026. The rails (LDO19M 1.8 V, LDO27M 3.0 V)
# are off at boot and have no regulator driver: zacpm switches their enable
# bits only (the owner's «ha», CLAUDE.md rule 5). s6sy761's probe at boot
# found the IC unpowered; it waits for the IC's boot-complete event, so
# nothing may read the event stack in between (bx24/bx25, log.md).
# off remembers which driver had the IC (s6sy761, or zrn-s6sy761 with the
# ear detection, 16.11: after a sleep it came back to the built-in, which
# could not bind while the other held it) and on binds that one again.
# At boot (nothing remembered) on prefers zrn-s6sy761 when its module is
# there (call proximity, PROXIMITY.md; 16.11: until then ed-trial by hand
# after every boot) and falls back to the built-in if it does not bind.
#   ztouch on | off
DEV=1-0048
DRVS=/sys/bus/i2c/drivers
STATE=/run/ztouch.drv
ZRN=zrn-s6sy761
ZRN_KO=/usr/local/lib/zboot/tools/$ZRN.ko
[ -e $ZRN_KO ] || ZRN_KO=/root/tools/$ZRN.ko

bound() { [ -e /sys/bus/i2c/devices/$DEV/driver ]; }

# bind $1 (a driver directory), three tries; 0 when bound
bind_to() {
	for try in 1 2 3; do
		echo "$DEV" > "$1/bind" 2>/dev/null && return 0
		bound && return 0
		echo "ztouch: bind $(basename "$1") $try failed" >&2
		[ $try = 1 ] && { sleep 0.5; continue; }
		# the IC sometimes needs longer than the probe gives it after
		# power up (19.10 11:55: tx timeout, -110): power it down and up
		zacpm touch off; sleep 1; zacpm touch on || return 1; sleep 0.3
	done
	return 1
}

case "$1" in
on)
	bound && exit 0
	WANT=$(cat $STATE 2>/dev/null)
	if [ -z "$WANT" ] && [ -e $ZRN_KO ]; then WANT=$ZRN; fi
	[ -n "$WANT" ] || WANT=s6sy761
	zacpm touch on || exit 1
	if [ "$WANT" = $ZRN ] && [ ! -d $DRVS/$ZRN ]; then
		# loaded only now, with the IC powered: its OF match probes
		# the unbound IC at once
		insmod $ZRN_KO || echo "ztouch: insmod $ZRN_KO failed" >&2
		bound && exit 0
	fi
	[ -d $DRVS/$WANT ] || WANT=s6sy761
	bind_to $DRVS/$WANT && exit 0
	[ $WANT = s6sy761 ] && exit 1
	# zrn-s6sy761 would not bind: the built-in, after a soft reset
	# (ed-trial's rollback)
	echo "ztouch: $WANT failed, back to the built-in s6sy761" >&2
	echo s6sy761 > $STATE
	i2ctransfer -y 1 w1@0x48 0x12 2>/dev/null; sleep 0.05
	bind_to $DRVS/s6sy761
	;;
off)
	if bound; then
		DRV=$(readlink -f /sys/bus/i2c/devices/$DEV/driver)
		basename "$DRV" > $STATE
		echo "$DEV" > "$DRV/unbind"
	fi
	zacpm touch off
	;;
*)
	echo "usage: ztouch on|off" >&2
	exit 2
	;;
esac
