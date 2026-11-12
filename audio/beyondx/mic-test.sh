#!/bin/sh
# audio/beyondx/mic-test.sh STEP: the CS47L93 microphones, one step at a time
# (run as root). Each step only does what it says; stop at the first bad one.
#
#   install  the new snd-zrn-abox-bx.ko (install-on-phone.sh, with a backup)
#            and zcall-uplink.sh (backup *.bak-DATE); then: reboot
#   state    read only: the codec's pins, UAIF0, its clock (nothing written)
#   probe    reset pulse on gpg1-0 + read the codec's ID over the SPI pins
#            (GPIO writes only; no PMIC, no ABOX). Want: id=6371
#   bench    without a call: UAIF0 bit clock + codec FLL lock + mics on for
#            3 s, the codec's status, then off. Routes are NOT touched.
#   once     the NEXT call only takes its uplink from the microphones
#            (/etc/zcall-once.env ZCALL_MIC=1; zcall-uplink falls back to the
#            SIFS0 stand-in by itself if the codec fails)
#   tone     the NEXT call sends the codec's 1 kHz tone (-20 dB) instead of
#            the microphones (mic_tone=1 + once): does the far end hear a beep?
#   uplink   only zcall-uplink.sh + mic-test.sh into /root/tools (backup first)
#   bridge   the NEXT call: the tone + stock's NSRC0/1 "Bridge" bits
#            (ZCALL_BRIDGE=1, ROUTE_CTRL2 bits 20/21): beep at the far end now?
#   always   every call (ZCALL_MIC=1 into /etc/zcall.env, backup first)
#   bits32   as once, UAIF0/AIF1 at 32-bit slots (driver mic_bits)
#   fmt32    as once, SIFM1 told 32-bit samples (the 2x pitch, log.md 11.11 17:10)
#   rate24   as once, SIFM1 told 24 kHz instead
#
# Added for OmonullOS, November 2026.
set -u
P=/sys/module/snd_zrn_abox_bx/parameters
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
log() { dmesg | grep -E "snd-zrn-abox-bx: (codec|mic|no CS47L93|the codec)" | tail -n "${1:-5}"; }
case "${1:-}" in
install)
	bash "$ROOT/audio/beyondx/install-on-phone.sh" || exit 1
	U=/root/tools/zcall-uplink.sh
	[ -f $U ] && cp -a $U $U.bak-$(date +%m%d-%H%M)
	install -m755 "$ROOT/audio/beyondx/zcall-uplink.sh" $U
	install -m755 "$ROOT/audio/beyondx/mic-test.sh" /root/tools/mic-test.sh
	sync
	echo "zcall-uplink.sh + mic-test.sh installed; now: sudo reboot, then: sudo /root/tools/mic-test.sh state"
	;;
state)
	[ -r $P/codec_state ] || { echo "the running module has no codec_state: install + reboot first"; exit 1; }
	cat $P/codec_state
	cat $P/mic_probe
	;;
probe)
	echo 1 > $P/mic_probe; r=$?
	cat $P/mic_probe; log 3
	[ $r = 0 ] && echo "OK: the codec answers" || echo "NO: the codec did not answer (pins given back; likely LDO18M off)"
	;;
bench)
	[ "$(cat $P/call)" = 0 ] || { echo "a call is running; not now"; exit 1; }
	echo 1 > $P/mic || { log 4; echo "mic=1 failed"; exit 1; }
	sleep 3
	for r in 1881 0300 0101 0519 0321 0329; do
		echo $r > $P/codec_peek; cat $P/codec_peek
	done
	cat $P/codec_state
	echo 0 > $P/mic
	log 4
	echo "(1881 bit 0100 = FLL1 locked)"
	;;
vi)
	# the next call with WDMA4 (vi-sensing, REFEXT4CH's source) running: the VSS uplink's trigger
	[ -f $P/call_vi ] || { echo "the loaded module has no call_vi: run install, then reboot"; exit 1; }
	echo 1 > $P/call_vi && echo 1 > $P/mic_tone && echo ZCALL_MIC=1 >> /etc/zcall-once.env && sync
	echo "next call: codec tone + WDMA4 (call_vi=$(cat $P/call_vi)); after it: echo 0 > $P/mic_tone"
	;;
tone)
	echo 1 > $P/mic_tone && echo ZCALL_MIC=1 >> /etc/zcall-once.env && sync
	echo "next call: the codec's tone instead of the mics (mic_tone=$(cat $P/mic_tone)); after it: echo 0 > $P/mic_tone"
	;;
uplink)
	U=/root/tools/zcall-uplink.sh
	[ -f $U ] && cp -a $U $U.bak-$(date +%m%d-%H%M)
	install -m755 "$ROOT/audio/beyondx/zcall-uplink.sh" $U
	install -m755 "$ROOT/audio/beyondx/mic-test.sh" /root/tools/mic-test.sh
	sync; echo "zcall-uplink.sh + mic-test.sh installed (no reboot needed)"
	;;
bridge)
	grep -q ZCALL_BRIDGE /root/tools/zcall-uplink.sh || { echo "run: mic-test.sh uplink first"; exit 1; }
	echo 1 > $P/mic_tone && printf 'ZCALL_MIC=1\nZCALL_BRIDGE=1\n' >> /etc/zcall-once.env && sync
	echo "next call: tone + bridge bits"; cat /etc/zcall-once.env
	echo "after it: echo 0 > $P/mic_tone"
	;;
bits32)	# the mics with UAIF0/AIF1 at 32-bit slots (driver mic_bits, log.md 11.11 17:25)
	echo 1 > $P/call_vi && echo 32 > $P/mic_bits && echo ZCALL_MIC=1 >> /etc/zcall-once.env && sync
	echo "next call: mic_bits=$(cat $P/mic_bits) call_vi=$(cat $P/call_vi); after it: echo 16 > $P/mic_bits to go back"
	;;
fmt32)	# the mics, SIFM1 told 32-bit samples (the 2x pitch, log.md 11.11 17:10)
	printf 'ZCALL_MIC=1\nZCALL_SIFM1_FMT="32 2"\n' >> /etc/zcall-once.env; sync
	echo "next call only:"; cat /etc/zcall-once.env
	;;
rate24)	# the mics, SIFM1 told 24 kHz (one sample a frame read as stereo pairs)
	printf 'ZCALL_MIC=1\nZCALL_SIFM1_RATE=24000\n' >> /etc/zcall-once.env; sync
	echo "next call only:"; cat /etc/zcall-once.env
	;;
once)
	echo ZCALL_MIC=1 >> /etc/zcall-once.env; sync
	echo "next call only: ZCALL_MIC=1 (/etc/zcall-once.env)"; cat /etc/zcall-once.env
	;;
always)
	E=/etc/zcall.env
	grep -q '^ZCALL_MIC=' $E 2>/dev/null && { echo "already: $(grep '^ZCALL_MIC=' $E)"; exit 0; }
	cp -a $E $E.bak-$(date +%m%d-%H%M) 2>/dev/null
	echo ZCALL_MIC=1 >> $E; sync
	echo "every call: ZCALL_MIC=1 in $E"
	;;
*)
	sed -n '2,19p' "$0"; exit 2
	;;
esac
