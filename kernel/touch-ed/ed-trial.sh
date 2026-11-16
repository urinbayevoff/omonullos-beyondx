#!/bin/bash
# ear-detect trial: swap s6sy761 -> zrn-s6sy761, auto-rollback unless ${S:-/tmp}/touch-ok appears
KO=/home/alarm/omonullos-src/build/touch-ed/zrn-s6sy761.ko
DEV=1-0048; OLD=/sys/bus/i2c/drivers/s6sy761; NEW=/sys/bus/i2c/drivers/zrn-s6sy761
FLAG=${S:-/tmp}/touch-ok
log() { echo "$(date +%T) $*"; }
rollback() {
	log "ROLLBACK"
	[ -e $NEW/$DEV ] && echo $DEV > $NEW/unbind
	for t in 1 2 3; do
		/usr/local/bin/i2ctransfer -y 1 w1@0x48 0x12 && log "sw reset sent"
		sleep 0.05
		echo $DEV > $OLD/bind 2>/dev/null && { log "built-in bound again (try $t)"; exit 1; }
		sleep 1
	done
	log "built-in bind FAILED: reboot needed"; exit 2
}
grep -q "^zrn_s6sy761 " /proc/modules || insmod $KO || { log "insmod failed"; exit 3; }
log "insmod ok"
echo $DEV > $OLD/unbind && log "built-in unbound"
if ! echo $DEV > $NEW/bind; then log "zrn bind failed"; rollback; fi
log "zrn-s6sy761 bound"
D=/sys/bus/i2c/devices/$DEV
echo 300 > $D/status_log
echo 1 > $D/ear_detect && log "ear_detect=1"
for i in $(seq 1 120); do [ -e $FLAG ] && { log "touch OK, keeping zrn-s6sy761"; exit 0; }; sleep 1; done
rollback
