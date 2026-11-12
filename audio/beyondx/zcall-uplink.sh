#!/bin/sh
# zcall-uplink.sh on|off|regs: a stand-in for the call's microphone path while the
# codec (CS47L93) is not up. Stock's route-cp-tx-bridge has UAIF0 -> NSRC0 ->
# WDMA1 -> VPCMIN_DAI0 (the VSS's uplink); here NSRC0 takes SIFS0 (the mixer,
# kept running by zcell's silence) and SIFM0 goes to WDMA, so the VSS gets
# uplink frames; NSRC1 -> WDMA2 (stock's "Voice Call TX Direct", the call
# capture the driver's call=1 starts) takes SIFS0 too. Registers from
# Samsung's abox_soc.h (ABOX 2.0): ROUTE_CTRL1 bits 3:0 / 7:4 = NSRC0 / NSRC1
# (1 SIFS0), SPUM_CTRL0 bits 7 / 11 = SIFM0 / SIFM1 out (0 WDMA); the old
# values are kept in /run for "off". zcell runs it once a call is active.
# The downlink: the VSS writes the decoded call into RDMA4 (the driver's
# call=1), which stock routes SPUS OUT4 = SIFS0 (mixer_paths init), ASRC4
# off; SPUS_CTRL0 stream 4 = bits 24:20 (IN 24, OUT 23:21, ASRC 20, ITV 5
# for ABOX 2.0.1) and its SRAM bank SBANK_RDMA4 size (bits 29:20, 512 bytes
# as abox_cmpnt_adjust_sbank gives a >10 ms period; the start is left).
# "regs" prints what the call's DMAs and routes hold.
# The stream 4 route with 48 kHz call streams crashed the LTE CP one second
# after every answered call (OTA 44, 2026-10-19 15:22/15:26/15:38); without
# them, at stock's 32 kHz (OTA 43), calls ran. So both wait behind
# ZCALL_RDMA4=1 and the rate is ZCALL_RATE (32000 by default).
# OTA 49 showed the route alone crashes the CP (RDMA4 at 32 kHz read by the
# 48 kHz SIFS0 with no rate converter). Stock turns SPUS ASRC4 on when the
# rates differ (asrc_set, sync mode): ZCALL_ASRC=1 does what
# asrc_config_sync does for 32000 -> 48000, 32 bit out, APF coef 1
# (mixer_paths), on the ASRC that SPUS_CTRL5 bits 3:0 give stream 4, then
# sets the stream's ASRC bit (SPUS_CTRL0 bit 20).
# ZCALL_AUD_MHZ: the ABOX core clock during the call. S-Boot leaves it at
# 295 MHz and the VSS plus the media chain miss periods there (rdma-7
# underruns every ~35 ms, the call sounds as if under water); stock asks for
# ABOX_CPU_GEAR_MAX (pm-qos-aud 1180 MHz) when a call stream starts. The
# DVFS request goes through the APM (zacpm, which picks the voltage), then the
# DSP is told the new clock (ABOX_CHANGED_GEAR, as the driver's gear_tell).
# That did not help (OTA 51): the VSS still hit its margin emergency every
# frame. ZCALL_PERIOD sets the call streams' period (driver call_period).
# ZCALL_NSRC_ASRC=1: the uplink's rate converters (SPUM_CTRL0 bit 4 NSRC0,
# bit 8 NSRC1: abox_soc.h FUNC_CHAIN_NSRC_ASRC, base 4, step 4; ztOS turns
# NSRC1's on for its calls) and SIFM0/1 told the input's real rate, 48 kHz.
# Without them WDMA2 takes SIFS0's 48 kHz 32-bit stereo straight: its 0x140-byte
# period (the VSS's) fills every ~0.86 ms instead of 10 ms, and the VSS's
# ISR_WDMAInt sees an interrupt 1.15-2.5 ms after every VIF boundary, so
# ISR_VIFBoundary restarts WDMA2/WDMA3 every frame (log.md 10.11, vsssync.py).
# ZCALL_MIC (default 1 since 2026-11-11 17:27, the mics heard clear): the real
# microphones; ZCALL_MIC=0 in /etc/zcall.env goes back to the SIFS0 stand-in.
# Added for OmonullOS, October 2026.
# Modified for OmonullOS, November 2026: ZCALL_MIC, ZCALL_BRIDGE.
P=/sys/module/snd_zrn_abox_bx/parameters
# A one-call test: /etc/zcall-once.env (ZCALL_RATE=, ZCALL_RDMA4=) is read
# by the next "on" and renamed, so a setting that crashes the CP is gone after
# that call.
E=/etc/zcall-once.env
S=/run/zcall-uplink.saved
A=/run/zcall-uplink.aud
# ZCALL_BRIDGE=1: stock's route-cp-tx-bridge sets "NSRC0 Bridge" and "NSRC1
# Bridge" (ROUTE_CTRL2 0x18c50048 bits 20/21, NSRC_CONNECTION_TYPE); ours
# leaves it 0. The codec's tone reached WDMA2 (tx 3259) and the far end still
# heard nothing (2026-11-11 14:40). The old value is kept in $B for "off".
B=/run/zcall-uplink.route2
# ZCALL_DSPCTL: DSP controls (NAME=VALUE ...) set for the call, ZCALL_DSPCTL_OFF
# after it (kept in $D for "off"). The RDMA7 underrun that fires in a call is
# chconv4ch_sink_update's (0x800ca7ac): one WDMA3 period in, one RDMA7 out.
D=/run/zcall-uplink.dspctl
aud_tell() {
	d=$(( 0x$(zpoke 18c01828 | cut -d' ' -f3) & 0xf ))
	echo "1 0 9 $(( 1179648000 / (d + 1) )) 0 0" > $P/ipc
	echo "aud: $(zacpm dvfs | grep '^aud'), CMU div $d"
}
# the uplink's rate converters, read only: SPUM_CTRL4 (0x310, NSRC0/1's ASRC id
# in bits 7:4 / 11:8) and each such SPUM ASRC (0x4000 + id x 0x100: CTRL, IS/OS
# default and limit, FILTER). Stock programs them from the kernel
# (asrc_config_sync, abox_cmpnt_v20.c) when the call's WDMA opens; we never do.
spum_asrc() {
	c4=$(zpoke 18c50310 | cut -d' ' -f3)
	printf 'spum_ctrl4 %s' $c4
	for n in 0 1; do
		id=$(( (0x$c4 >> (4 + 4 * n)) & 0xf ))
		a=$(( 0x18c54000 + id * 0x100 ))
		printf ' | nsrc%d asrc %d:' $n $id
		for o in 0 10 14 18 1c 2c; do printf ' %s' $(zpoke $(printf %x $((a + 0x$o))) | cut -d' ' -f3); done
	done
	echo
}
regs() {
	for a in 18c50200 18c50210 18c50214 18c50218 18c50220 18c50230 18c50044 18c50048 18c50300 \
			18c51400 18c51404 18c51408 18c5140c 18c51410 18c51414 18c51418 18c5141c 18c51420 \
			18c53200 18c53204 18c53208 18c5320c 18c53210 18c53214 18c53218 18c5321c 18c53220 \
			18c53300 18c53304 18c53308 18c5330c 18c53310 18c53314 18c53318 18c53330 \
			18c51700 18c51704 18c51708 18c5170c 18c51710 18c51714 18c51718 18c51730; do
		zpoke $a
	done | tr '\n' ' '
	echo
}
case "$1" in
regs)
	regs; spum_asrc; exit 0
	;;
on)
	r1=$(zpoke 18c50044 | cut -d' ' -f3)
	m0=$(zpoke 18c50300 | cut -d' ' -f3)
	c0=$(zpoke 18c50200 | cut -d' ' -f3)
	b4=$(zpoke 18c50230 | cut -d' ' -f3)
	[ -e $S ] || echo "$r1 $m0 $c0 $b4" > $S
	# every call's settings: /etc/zcall.env (the downlink RDMA4 + ASRC4 route,
	# the calls that had sound on 2026-10-20 00:21/00:35); the one-call file wins
	[ -r /etc/zcall.env ] && . /etc/zcall.env
	if [ -r $E ]; then
		. $E; mv $E $E.used
		echo "once: ZCALL_RATE=$ZCALL_RATE ZCALL_RDMA4=$ZCALL_RDMA4 ZCALL_ASRC=$ZCALL_ASRC ZCALL_AUD_MHZ=$ZCALL_AUD_MHZ ZCALL_PERIOD=$ZCALL_PERIOD ZCALL_NSRC_ASRC=$ZCALL_NSRC_ASRC ZCALL_TYPE=$ZCALL_TYPE ZCALL_DSPCTL=$ZCALL_DSPCTL ZCALL_MIC=$ZCALL_MIC ZCALL_BRIDGE=$ZCALL_BRIDGE ZCALL_SIFM1_RATE=$ZCALL_SIFM1_RATE ZCALL_SIFM1_FMT=$ZCALL_SIFM1_FMT"
	fi
	echo "before: $(regs)"
	echo ${ZCALL_RATE:-32000} > $P/call_rate
	# the call streams' period in frames: stock 480 (driver default 320)
	[ -w $P/call_period ] && echo ${ZCALL_PERIOD:-320} > $P/call_period
	if [ "$ZCALL_RDMA4" = 1 ]; then
		zpoke 18c50200 $(printf %x $(( (0x$c0 & ~0x1f00000) | (1 << 21) )))
		zpoke 18c50230 $(printf %x $(( (0x$b4 & ~0x3ff00000) | (512 << 16) )))
	fi
	if [ "$ZCALL_RDMA4" = 1 ] && [ "$ZCALL_ASRC" = 1 ]; then
		id=$(( 0x$(zpoke 18c50214 | cut -d' ' -f3) & 0xf ))
		a=$(( 0x18c52000 + id * 0x100 ))
		ac=$(zpoke $(printf %x $a) | cut -d' ' -f3)
		# OS/IS source TICK_SYNC, keep the tick fields, 32 bit out, DCMF/OVSF 8
		zpoke $(printf %x $a) $(printf %x $(( (0x$ac & 0x000fff00) | 0x00900000 | 0xfc )))
		zpoke $(printf %x $((a + 0x10))) 18000		# IS default 98304
		zpoke $(printf %x $((a + 0x14))) 1333		# IS tperiod limit, 5 %
		zpoke $(printf %x $((a + 0x18))) 10000		# OS default 65536
		zpoke $(printf %x $((a + 0x1c))) ccc		# OS tperiod limit
		f=$(zpoke $(printf %x $((a + 0x2c))) | cut -d' ' -f3)
		zpoke $(printf %x $((a + 0x2c))) $(printf %x $(( 0x$f | 0x40 )))	# APF coef 1
		c=$(zpoke 18c50200 | cut -d' ' -f3)
		zpoke 18c50200 $(printf %x $(( 0x$c | (1 << 20) )))
		echo "asrc $id: $(for o in 0 10 14 18 1c 2c; do zpoke $(printf %x $((a + 0x$o))); done | tr '\n' ' ')"
	fi
	if [ -n "$ZCALL_AUD_MHZ" ]; then
		# the APM answers 0 until someone asked it for a rate: S-Boot's 295
		[ -e $A ] || zacpm dvfs | awk '/^aud/{m = int($2 / 1000); print m ? m : 295}' > $A
		zacpm dvfs set aud $ZCALL_AUD_MHZ
		aud_tell
	fi
	# ZCALL_MIC=1: the real microphones (driver mic=1: CS47L93 + UAIF0), NSRC0/1
	# <- UAIF0 (nsrcx_texts 8, stock route-cp-tx-bridge); if the codec does not
	# come up, the SIFS0 stand-in as before
	src=11
	if [ "${ZCALL_MIC:-1}" = 1 ]; then
		# the codec core rail LDO18M (off after every boot; owner's «ha» 2026-11-11)
		# nothing turns it off again, so once a boot (~0.3 s of the answer's delay)
		if [ ! -e /run/zcall-codec-on ]; then
			zacpm codec on >/dev/null 2>&1 && : > /run/zcall-codec-on || echo "mic: zacpm codec on failed"
		fi
		if echo 1 > $P/mic; then src=88; echo "mic: on ($(cat $P/mic_probe))"
		else echo "mic: failed ($(cat $P/mic_probe)), SIFS0 stand-in"; fi
	fi
	zpoke 18c50044 $(printf %x $(( (0x$r1 & ~0xff) | 0x$src )))
	if [ "$ZCALL_BRIDGE" = 1 ]; then
		r2=$(zpoke 18c50048 | cut -d' ' -f3)
		[ -e $B ] || echo $r2 > $B
		zpoke 18c50048 $(printf %x $(( 0x$r2 | 0x300000 )))
		echo "bridge: route2 $r2 -> $(zpoke 18c50048 | cut -d' ' -f3)"
	fi
	if [ "$ZCALL_NSRC_ASRC" = 1 ]; then
		zpoke 18c50300 $(printf %x $(( (0x$m0 & ~0x880) | 0x110 )))
		in0=48000; in1=48000
	else
		zpoke 18c50300 $(printf %x $(( 0x$m0 & ~0x880 )))
		in0=48000; in1=$(cat $P/call_rate)
	fi
	# ZCALL_TYPE: ABOX_SET_TYPE after zcell's VOICE (0); 1 = SPEAKER. The
	# firmware's RDMA7 underrun check (0x800ca2e0, by CHCONV2CH, which never
	# opens here) fires every frame in calls and sets off the VSS margin
	# emergency; VOICE may be what arms it.
	[ -n "$ZCALL_TYPE" ] && echo "1 0 0x60 $ZCALL_TYPE" > $P/ipc
	echo "0xa 0 5 $in0 0" > $P/ipc		# SET_SIFM0_RATE
	echo "0xa 0 0x14 16 2" > $P/ipc		# SET_SIFM0_FORMAT: 16 bit, 2 ch
	# ZCALL_SIFM1_RATE / ZCALL_SIFM1_FMT ("bits ch"): the microphones' tone
	# reached the VSS at twice its pitch, its two channels one 48 kHz sample
	# apart (log.md 11.11 17:10): one sample a frame taken as a stereo frame
	echo "0xa 0 6 ${ZCALL_SIFM1_RATE:-$in1} 0" > $P/ipc	# SET_SIFM1_RATE (without the converter: the call capture's)
	echo "0xa 0 0x15 ${ZCALL_SIFM1_FMT:-$(cat $P/mic_bits 2>/dev/null || echo 16) 2}" > $P/ipc	# SET_SIFM1_FORMAT
	for c in WDMA1_EN=On VPCMIN_DAI0_EN=On VPCMIN_DAI0_A=WDMA1 $ZCALL_DSPCTL; do
		echo $c > $P/dspctl || echo "dspctl $c failed"
	done
	[ -n "$ZCALL_DSPCTL_OFF" ] && echo "$ZCALL_DSPCTL_OFF" > $D
	echo "on: $(spum_asrc)"
	# the same once a second for 20 s (the firmware may rewrite them once the
	# VSS starts WDMA2): /run/zcall-asrc.log, detached from zcell's pipe
	setsid sh -c "for i in \$(seq 20); do date +%T; sh $0 regs | tail -1; sleep 1; done" \
		> /run/zcall-asrc.log 2>&1 < /dev/null &
	;;
off)
	echo "off: $(spum_asrc)"
	for c in VPCMIN_DAI0_EN=Off WDMA1_EN=Off $(cat $D 2>/dev/null); do
		echo $c > $P/dspctl || echo "dspctl $c failed"
	done
	[ "$(cat $P/mic 2>/dev/null)" = 1 ] && echo 0 > $P/mic
	if read r1 m0 c0 b4 < $S; then
		zpoke 18c50044 $r1; zpoke 18c50300 $m0
		[ -n "$b4" ] && { zpoke 18c50200 $c0; zpoke 18c50230 $b4; }
		rm -f $S
	fi
	if read r2 < $B; then
		zpoke 18c50048 $r2; rm -f $B
	fi
	rm -f $D
	if read mhz < $A; then
		zacpm dvfs set aud $mhz
		aud_tell
		rm -f $A
	fi
	;;
*)
	echo "usage: zcall-uplink.sh on|off|regs" >&2; exit 2
	;;
esac
dmesg | grep "DSP control" | tail -4
