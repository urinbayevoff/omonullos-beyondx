#!/bin/sh
# zspk-calibrate: measure the bottom speaker's resistance (RDC) once and keep
# it, so the speaker protection runs with this phone's own value.
#
#   sudo zspk-calibrate [AMBIENT_C]      (default 23)
#
# The way stock's cirrus-cal does it, and as tested by hand on 2026-11-14
# (docs/s10-5g/SPK-PROT.md): the protection on (amp_dsp=1), 12 s of silence
# to the speaker, and while it plays dsp_calibrate=AMBIENT (the driver itself
# runs the calibration at 0 dB with the DSP inputs muted and puts the
# playback tuning back). Only silence is played. A good result (status 1,
# checksum ok, R within the driver's 5600..14000) goes to
# /var/lib/zaudio/spk.conf as module options, which zaudio-load.sh reads at
# boot when /etc/modprobe.d has none: from then on the protection is on by
# default, at -3 dB (dsp_vol_boost 168, the level tested on 2026-11-14).
#
# Added for OmonullOS, November 2026.
set -u
P=/sys/module/snd_zrn_abox_bx/parameters
OUT=${ZSPK_OUT:-/var/lib/zaudio/spk.conf}
AMBIENT=${1:-23}
die() { echo "zspk-calibrate: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root (sudo)"
[ -d "$P" ] || die "the sound driver is not loaded"
[ "$(cat "$P/call")" = 0 ] || die "a call is on"
case "$AMBIENT" in ''|*[!0-9]*) die "ambient must be a number of degrees" ;; esac

U=alarm UIDN=1000
[ -r /etc/ztos/user.conf ] && . /etc/ztos/user.conf && U=${ZTOS_USER:-alarm} UIDN=${ZTOS_UID:-1000}
W=$(mktemp /run/zspk.XXXXXX)
trap 'rm -f "$W"' EXIT
python3 - "$W" <<'EOF'
import sys, wave
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(b"\0" * 4 * 48000 * 12)
EOF
chmod 644 "$W"

[ "$(cat "$P/amp_dsp")" = 1 ] || echo 1 > "$P/amp_dsp"
runuser -u "$U" -- env XDG_RUNTIME_DIR=/run/user/$UIDN pw-play --target zrn-speakers "$W" &
pid=$!
ok=0 i=0
# up to 10 s for the protection to load (~5 s) and the amp to come on
while [ $i -lt 20 ]; do
	sleep 0.5
	i=$((i + 1))
	if { echo "$AMBIENT" > "$P/dsp_calibrate"; } 2>/dev/null; then
		ok=1
		break
	fi
done
wait $pid
[ $ok = 1 ] || die "the speaker protection did not come up (dsp_state: $(cat "$P/dsp_state"))"
res=$(cat "$P/dsp_cal_result")
echo "$res"
r=$(printf '%s\n' "$res" | sed -n 's/.* status 1 r \([0-9]*\) .*checksum [0-9]* (ok).* kept .*/\1/p')
[ -n "$r" ] || die "not a good measurement (nothing saved)"
[ "$r" -ge 5600 ] && [ "$r" -le 14000 ] || die "R $r out of range (nothing saved)"
ohm=$(printf '%s\n' "$res" | sed -n 's/.*(\([0-9.]* ohm\)).*/\1/p')
mkdir -p "$(dirname "$OUT")"
printf '# the bottom speaker, measured by zspk-calibrate on %s: %s, ambient %s C\noptions snd_zrn_abox_bx dsp_cal_r=%s amp_dsp=1 dsp_run_reinit=1 dsp_vol_boost=168\n' \
	"$(date '+%Y-%m-%d %H:%M')" "$ohm" "$AMBIENT" "$r" > "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
echo "saved: R $r ($ohm) -> $OUT; the protection is on from the next boot"
