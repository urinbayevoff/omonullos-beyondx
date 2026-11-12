# Galaxy S10 5G: keep uniLoader's watchdog from firing while user space runs.
# Sourced by initramfs/init (as /etc/zboot/early) after its dump and deadman
# timers; scripts/build-boot.sh --device beyondx --pet puts it there.
#
# Added for OmonullOS, October 2026. Tested on the phone (bx9: up past 85 s,
# then nopet brought it back to TWRP).
#
# uniLoader arms the cluster-0 watchdog at 0x10050000 for 30 s
# (kernel/uniloader-failsafe.py: WTCON 0xff39, WTDAT = WTCNT = 23805 at
# 26 MHz / 256 / 128) and nothing in the kernel touches it, so it has ended
# every boot so far, through INFORM3 into TWRP. Mainline's s3c2410_wdt does
# not know this SoC's PMU yet, so a shell loop reloads WTCNT from WTDAT
# every 5 s instead. That keeps the failsafe: if user space stops, so does
# the reload, and the phone is in TWRP 30 s later. On purpose, from telnet:
# touch /run/zboot.nopet.

WDT=0x10050000

# The MAX77705's charger has a watchdog of its own, which S-Boot leaves on
# (CNFG_00 bit 4): unkicked, charging stops about a minute into our boot
# (CHG_DTLS 0x0b) while the phone keeps running from USB. Samsung's driver
# kicks it by setting CNFG_06 (0xbd) WDTCLR = 1, which clears itself; that
# and nothing else is written. Owner's «ha» for this write, 2026-10-17; on
# bx9 over telnet charging went on at +115..185 mA with a kick every 20 s.
# Called from the reload loop below every 4th pass (20 s); bx12 charged
# through 400 s of uptime at +116..199 mA, CHG_DTLS 1 all along.
charger_kick() {
	b=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* 2>/dev/null | head -n1)
	b=${b##*/i2c-}
	[ -n "$b" ] || return 0
	c=$(i2cget -y "$b" 0x69 0xbd 2>/dev/null) || return 0
	i2cset -y "$b" 0x69 0xbd $(( (c & ~3) | 1 )) 2>/dev/null
}

wdt_pet() {
	con=$(devmem $WDT 32)
	dat=$(devmem $((WDT + 4)) 32)
	cnt=$(devmem $((WDT + 8)) 32)
	log "wdt: WTCON $con WTDAT $dat WTCNT $cnt"
	# only the watchdog uniLoader set up: enabled (bit 5), reset (bit 0)
	if [ $((con & 0x21)) -ne $((0x21)) ] || [ $((dat)) -eq 0 ]; then
		log "wdt: not the one uniLoader armed; leaving it alone"
		return 1
	fi
	log "wdt: reloading every 5 s; touch /run/zboot.nopet to let it fire"
	(
		n=0
		while [ ! -e /run/zboot.nopet ]; do
			devmem $((WDT + 8)) 32 "$dat"
			n=$((n + 1))
			[ $((n % 4)) -eq 0 ] && charger_kick
			[ $((n % 12)) -eq 0 ] && log "wdt: alive, $n reloads"
			sleep 5
		done
		log "wdt: /run/zboot.nopet, reloads stop; reset in 30 s"
	) &
	WDT_PET_PID=$!	# pre-switch.sh stops this loop before switch_root
}

[ "$(arg zboot.wdt)" = pet ] && wdt_pet

# The thermal guard (kernel/beyondx/zthermal.sh): nothing else limits heat
# yet. zboot.thermal=off leaves it out, zboot.thermal=75,85,95,60 sets its
# soft, hard, crit and clear levels.
th=$(arg zboot.thermal)
if [ "$th" != off ] && [ -x "${ZBIN:-/bin}/zthermal" ]; then
	log "thermal guard: ${th:-default levels}"
	"${ZBIN:-/bin}/zthermal" $(echo "$th" | tr , ' ') &
fi

# The wall-charger rate with its temperature guard (kernel/beyondx/zchg.sh,
# owner's «ha» 2026-10-17); zboot.chg=off leaves it out.
if [ "$(arg zboot.chg)" != off ] && [ -x "${ZBIN:-/bin}/zchg" ]; then
	log "charger: zchg"
	"${ZBIN:-/bin}/zchg" &
fi
