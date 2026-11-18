#!/bin/sh
# zspk-calibrate: measure the speakers' resistance (RDC) once and keep it, so
# the speaker protection runs with this phone's own values.
#
#   sudo zspk-calibrate [--bot|--rcv|--both] [AMBIENT_C]   (default --bot, 23 C)
#
#   --bot   the bottom speaker (amp 0x40), mono protection
#   --rcv   the earpiece (amp 0x41); needs both amps through the protection
#           (amp_mask 3, amp_dsp 3), set here for the measurement
#   --both  the two in one go, then stereo protection for media
#
# The way stock's cirrus-cal does it, and as tested by hand on 2026-11-14
# (bottom) and 2026-11-15 (earpiece) (docs/s10-5g/SPK-PROT.md): the
# protection on, silence to the speakers, and while it plays
# dsp_calibrate[_rcv]=AMBIENT (the driver itself runs the calibration at 0 dB
# with the DSP inputs muted and puts the playback tuning back). Only silence
# is played. A good result (status 1, checksum ok, R within the driver's
# 5600..14000) goes to /var/lib/zaudio/spk.conf as module options, which
# zaudio-load.sh reads at boot when /etc/modprobe.d has none: from then on the
# protection is on by default, at -3 dB (dsp_vol_boost 168, tested on
# 2026-11-14 mono and 2026-11-15 stereo). A value kept from an earlier run for
# the other speaker stays; with both values known the conf is stereo.
#
# Added for OmonullOS, November 2026.
set -u
P=/sys/module/snd_zrn_abox_bx/parameters
OUT=${ZSPK_OUT:-/var/lib/zaudio/spk.conf}
FW=/usr/lib/firmware
MODE=bot
case "${1:-}" in
--bot | --rcv | --both) MODE=${1#--}; shift ;;
-h | --help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac
AMBIENT=${1:-23}
die() { echo "zspk-calibrate: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo)"
[ -d "$P" ] || die "the sound driver is not loaded"
[ "$(cat "$P/call")" = 0 ] || die "a call is on"
case "$AMBIENT" in '' | *[!0-9]*) die "ambient must be a number of degrees" ;; esac
[ "$AMBIENT" -le 60 ] || die "ambient must be 0..60"
case $MODE in
bot) AMPS=bot ;;
rcv) AMPS=rcv ;;
both) AMPS="bot rcv" ;;
esac
for a in $AMPS; do
	[ -r "$FW/cs35l40-$a-dsp1-spk-prot-calib.bin" ] ||
		die "$FW/cs35l40-$a-dsp1-spk-prot-calib.bin is missing (zvendor-firmware copies it from VENDOR)"
done

U=alarm UIDN=1000
[ -r /etc/ztos/user.conf ] && . /etc/ztos/user.conf && U=${ZTOS_USER:-alarm} UIDN=${ZTOS_UID:-1000}
W=$(mktemp /run/zspk.XXXXXX)
pid=""
OLD_DSP=$(cat "$P/amp_dsp") OLD_MASK=$(cat "$P/amp_mask")
# amp_dsp is a mask (1 the bottom speaker, 2 the earpiece): --bot adds 1
SET_DSP=$((OLD_DSP | 1)) SET_MASK=$OLD_MASK
[ "$MODE" = bot ] || SET_DSP=3 SET_MASK=3
done_ok=0
finish() {
	[ -n "$pid" ] && kill "$pid" 2>/dev/null
	rm -f "$W"
	# a failed run puts the routing back as it was; a good one keeps the new
	# protection on (the same as the conf it saved)
	if [ $done_ok = 0 ]; then
		[ "$(cat "$P/amp_mask")" = "$OLD_MASK" ] || echo "$OLD_MASK" > "$P/amp_mask"
		[ "$(cat "$P/amp_dsp")" = "$OLD_DSP" ] || echo "$OLD_DSP" > "$P/amp_dsp"
	fi
}
trap finish EXIT
trap 'exit 1' INT TERM
python3 - "$W" <<'EOF'
import sys, wave
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(b"\0" * 4 * 48000 * 20)
EOF
chmod 644 "$W"

[ "$(cat "$P/amp_mask")" = "$SET_MASK" ] || echo "$SET_MASK" > "$P/amp_mask"
[ "$(cat "$P/amp_dsp")" = "$SET_DSP" ] || echo "$SET_DSP" > "$P/amp_dsp"
runuser -u "$U" -- env XDG_RUNTIME_DIR=/run/user/$UIDN pw-play --target zrn-speakers "$W" 2>/dev/null &
pid=$!

# measure one amp: prints its R, or fails
measure() {
	knob=dsp_calibrate
	[ "$1" = rcv ] && knob=dsp_calibrate_rcv
	ok=0 i=0
	# up to 10 s for the protection to load (~5 s) and the amp to come on
	while [ $i -lt 20 ]; do
		kill -0 "$pid" 2>/dev/null || break
		if { echo "$AMBIENT" > "$P/$knob"; } 2>/dev/null; then
			ok=1
			break
		fi
		sleep 0.5
		i=$((i + 1))
	done
	[ $ok = 1 ] || { echo "zspk-calibrate: $1: the protection did not come up (dsp_state: $(cat "$P/dsp_state"))" >&2; return 1; }
	res=$(cat "$P/dsp_cal_result")
	echo "$res" >&2
	r=$(printf '%s\n' "$res" | sed -n 's/.* status 1 r \([0-9]*\) .*checksum [0-9]* (ok).* kept .*/\1/p')
	[ -n "$r" ] || { echo "zspk-calibrate: $1: not a good measurement" >&2; return 1; }
	[ "$r" -ge 5600 ] && [ "$r" -le 14000 ] || { echo "zspk-calibrate: $1: R $r out of range" >&2; return 1; }
	echo "$r $(printf '%s\n' "$res" | sed -n 's/.*(\([0-9.]*\) ohm).*/\1/p')"
}

R_BOT="" R_RCV="" OHM_BOT="" OHM_RCV=""
for a in $AMPS; do
	m=$(measure "$a") || die "nothing saved"
	case $a in
	bot) R_BOT=${m% *} OHM_BOT=${m#* } ;;
	rcv) R_RCV=${m% *} OHM_RCV=${m#* } ;;
	esac
done
kill "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
pid=""

# keep the other speaker's value from an earlier run
old_opt() { sed -n "s/.*[ :]$1=\([0-9]*\).*/\1/p" "$OUT" 2>/dev/null | tail -1; }
old_note() {
	n=$(sed -n "s/^# .*$1 \([0-9.]* ohm, [^;]*\).*/\1/p" "$OUT" 2>/dev/null | tail -1)
	echo "${n:-an earlier run}"
}
now=$(date '+%Y-%m-%d %H:%M')
NOTE_BOT="" NOTE_RCV=""
[ -n "$R_BOT" ] && NOTE_BOT="$OHM_BOT ohm, $now, ambient $AMBIENT C"
[ -n "$R_RCV" ] && NOTE_RCV="$OHM_RCV ohm, $now, ambient $AMBIENT C"
[ -n "$R_BOT" ] || { R_BOT=$(old_opt dsp_cal_r); NOTE_BOT=$(old_note bottom); }
[ -n "$R_RCV" ] || { R_RCV=$(old_opt dsp_cal_r_rcv); NOTE_RCV=$(old_note earpiece); }

mkdir -p "$(dirname "$OUT")"
{
	echo "# measured by zspk-calibrate:${R_BOT:+ bottom $NOTE_BOT;}${R_RCV:+ earpiece $NOTE_RCV;}"
	if [ -n "$R_BOT" ] && [ -n "$R_RCV" ]; then
		echo "options snd_zrn_abox_bx dsp_cal_r=$R_BOT dsp_cal_r_rcv=$R_RCV amp_dsp=3 amp_mask=3 dsp_run_reinit=1 dsp_vol_boost=168"
	elif [ -n "$R_BOT" ]; then
		echo "options snd_zrn_abox_bx dsp_cal_r=$R_BOT amp_dsp=1 dsp_run_reinit=1 dsp_vol_boost=168"
	else
		# the earpiece alone: kept for a later --bot run, the protection stays as it is
		echo "# earpiece only so far (run --bot too for the protection at boot): dsp_cal_r_rcv=$R_RCV"
	fi
} > "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
# live too, so the protection runs with the new values right away
[ -n "$R_BOT" ] && echo "$R_BOT" > "$P/dsp_cal_r"
[ -n "$R_RCV" ] && echo "$R_RCV" > "$P/dsp_cal_r_rcv"
done_ok=1
[ "$MODE" = rcv ] && [ -z "$R_BOT" ] && done_ok=0	# back to the old routing
echo "saved -> $OUT:"
grep -v '^#' "$OUT" || grep 'dsp_cal_r_rcv' "$OUT"
if grep -qs '^[[:space:]]*options[[:space:]]\+snd[-_]zrn[-_]abox[-_]bx' /etc/modprobe.d/snd-zrn-abox-bx.conf; then
	echo "note: /etc/modprobe.d/snd-zrn-abox-bx.conf has its own options and wins at boot;"
	echo "      put the new dsp_cal_r / dsp_cal_r_rcv there by hand (or remove that file)"
fi
