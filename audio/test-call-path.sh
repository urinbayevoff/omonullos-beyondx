#!/bin/bash
# audio/test-call-path.sh: the path of a call without a call. No radio is touched.
# Runs on the phone as the user with the session's PipeWire. For each route: a
# tone through the normal playback while the driver is in call mode, measured
# at the microphones; then the driver's own numbers (call_diag). Ends with
# ordinary playback and capture, to show a call leaves nothing behind.
#
# Modified for OmonullOS, November 2026: on the Galaxy S10 5G (snd_zrn_abox_bx)
# the part below runs instead, the S9's is unchanged. There is no call to fake
# there: the driver's "call" without a live call brings the voice firmware
# down (VHMPcmOutThread dequeue_vpcmout_msg(0x0), 2026-10-20; only a reboot
# brings the DSP back), there is no "Voice Call" control and, while the
# CS47L93 has no driver, no capture. So:
#   test-call-path.sh            no call: the path's state (call_diag), then a
#                                tone through ordinary playback, seen at WDMA3
#                                (ref: the echo reference the voice firmware
#                                takes in a call) and RDMA7 (spk: the amps')
#   test-call-path.sh watch [S]  during a live call (zcell sets its path up):
#                                call_diag every 0.5 s for S seconds (20), and
#                                which directions moved
if [ -d /sys/module/snd_zrn_abox_bx ]; then
	P=/sys/module/snd_zrn_abox_bx/parameters
	export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
	[ -r $P/call_diag ] || { echo "snd-zrn-abox-bx has no call_diag: older than November 2026"; exit 1; }
	field() { echo " $1" | sed -n "s/.* $2=\([^ ]*\).*/\1/p"; }	# diag key
	short() { for k in call mark nsrc0 nsrc1 rdma4 rx wdma2 tx wdma3 ref rdma7 spk; do printf '%s=%s ' $k "$(field "$1" $k)"; done; echo; }
	case "${1:-bench}" in
	bench)
		d=$(cat $P/call_diag)
		echo "now:   $d"
		[ "$d" = off ] && exit 1
		[ "$(field "$d" call)" = 0 ] || { echo "a call is on: test-call-path.sh watch"; exit 1; }
		T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
		# 1 kHz, 3 s, peak 3000 of 32767 (-21 dBFS: quieter than the system sounds)
		python3 - "$T/tone.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b''.join(struct.pack('<hh', *(2 * [int(3000 * math.sin(2 * math.pi * 1000 * i / 48000))])) for i in range(48000 * 3)))
PY
		pw-play "$T/tone.wav" & pid=$!
		sleep 1.2
		d=$(cat $P/call_diag)
		wait $pid
		echo "tone:  $(short "$d")"
		ref=$(field "$d" ref)
		case $ref in
		-1) echo "WDMA3 points at no buffer the driver knows: the speaker graph is not running" ;;
		0) echo "WDMA3 runs silent: the echo reference would carry nothing in a call" ;;
		*) echo "the echo reference carries the playback (ref $ref; tone 3000 before PipeWire's volume)" ;;
		esac
		sleep 0.5
		echo "after: $(short "$(cat $P/call_diag)")"
		;;
	watch)
		n=$(( ${2:-20} * 2 )); i=0; mrx=-1; mtx=-1; mref=-1
		[ "$(field "$(cat $P/call_diag)" call)" = 1 ] || echo "no call yet (call=0): sampling anyway"
		while [ $i -lt $n ]; do
			d=$(cat $P/call_diag)
			printf '%5s  %s' "$(( i / 2 )).$(( i % 2 * 5 ))" "$(short "$d")"
			v=$(field "$d" rx); [ "${v:--1}" -gt $mrx ] && mrx=$v
			v=$(field "$d" tx); [ "${v:--1}" -gt $mtx ] && mtx=$v
			v=$(field "$d" ref); [ "${v:--1}" -gt $mref ] && mref=$v
			i=$((i + 1)); sleep 0.5
		done
		echo "loudest: rx $mrx (downlink, RDMA4), tx $mtx (uplink, WDMA2), ref $mref (echo reference, WDMA3)"
		echo "(-1: the DMA points nowhere known; with nsrc1=1 the uplink is zcall-uplink's SIFS0 stand-in, not a microphone)"
		;;
	*)
		echo "usage: test-call-path.sh [bench | watch [seconds]]" >&2; exit 2
		;;
	esac
	exit
fi
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
P=/sys/module/snd_zrn_abox/parameters
T=$(mktemp -d); trap 'amixer -q -c0 cset name="Voice Call" Off; rm -rf "$T"' EXIT
python3 - "$T/tone.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b''.join(struct.pack('<hh', *(2 * [int(9000 * math.sin(2 * math.pi * 1000 * i / 48000))])) for i in range(48000 * 3)))
PY
measure() {	# label: mic level with the tone playing
	pw-play "$T/tone.wav" & local pid=$!
	sleep 0.8
	timeout 1.5 pw-record --rate 48000 --channels 1 --format s16 --raw - > "$T/mic.raw" 2>/dev/null
	wait $pid
	python3 - "$1" "$T/mic.raw" <<'PY'
import array, math, sys
a = array.array('h'); a.frombytes(open(sys.argv[2], 'rb').read()[:144000 // 2 * 2])
print('%-10s fast=%s mic rms %6.0f peak %6d (%d samples)' % (sys.argv[1], open('/sys/module/snd_zrn_abox/parameters/fast_rebuilds').read().strip(), math.sqrt(sum(x * x for x in a) / max(len(a), 1)), max(map(abs, a), default=0), len(a)))
PY
}
measure before
for cycle in 1 2; do
	for route in Earpiece Speaker Earpiece; do
		amixer -q -c0 cset name='Voice Call' $route || echo "FAILED $route"
		sleep 0.3
		measure "$route"
		cat $P/call_diag
	done
	amixer -q -c0 cset name='Voice Call' Off
	sleep 0.3
done
cat $P/call_diag
measure after
for f in ipc_retries dsp_restarts codec_errors fast_rebuilds; do echo "$f=$(cat $P/$f)"; done
