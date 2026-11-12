#!/bin/sh
# ztouch: power the Galaxy S10 5G's touchscreen and bind its driver.
#
# Added for OmonullOS, October 2026. The rails (LDO19M 1.8 V, LDO27M 3.0 V)
# are off at boot and have no regulator driver: zacpm switches their enable
# bits only (the owner's «ha», CLAUDE.md rule 5). s6sy761's probe at boot
# found the IC unpowered; it waits for the IC's boot-complete event, so
# nothing may read the event stack in between (bx24/bx25, log.md).
#   ztouch on | off
DEV=1-0048
DRV=/sys/bus/i2c/drivers/s6sy761
case "$1" in
on)
	[ -e "$DRV/$DEV" ] && exit 0
	zacpm touch on || exit 1
	# the IC sometimes needs longer than the probe gives it after power up
	# (19.10 11:55: tx timeout, -110): bind again, then power it down and up
	for try in 1 2 3; do
		echo "$DEV" > "$DRV/bind" 2>/dev/null && exit 0
		echo "ztouch: bind $try failed" >&2
		[ $try = 1 ] && { sleep 0.5; continue; }
		zacpm touch off; sleep 1; zacpm touch on || exit 1; sleep 0.3
	done
	exit 1
	;;
off)
	[ -e "$DRV/$DEV" ] && echo "$DEV" > "$DRV/unbind"
	zacpm touch off
	;;
*)
	echo "usage: ztouch on|off" >&2
	exit 2
	;;
esac
