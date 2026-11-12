#!/bin/bash
# audio/beyondx/leadtest.sh: start a silent stream with rt_lead_us=0 and with
# a lead, and save the DSP's log from each start, to see why RDMA7 does not
# start when it comes after WDMA3. Only our module's parameters are written;
# rt_lead_us is left at 0 (the old order) at the end.
# Run as root: sudo bash audio/beyondx/leadtest.sh [LEAD_US]
#
# Added for OmonullOS, November 2026.
set -u
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
P=/sys/module/snd_zrn_abox_bx/parameters
OUT=/home/alarm/leadtest-$(date +%H%M)
LEAD=${1:-15000}
mkdir -p "$OUT"
U=$(id -u alarm)
play() {	# 2 s of silence through PipeWire as alarm
	python3 -I -c "import wave,sys;w=wave.open(sys.argv[1],'wb');w.setnchannels(2);w.setsampwidth(2);w.setframerate(48000);w.writeframes(bytes(48000*4*2));w.close()" "$OUT/sil.wav"
	runuser -u alarm -- env XDG_RUNTIME_DIR=/run/user/$U pw-play "$OUT/sil.wav" &
	sleep 1
	cat $P/call_diag > "$OUT/$1-diag.txt"
	for s in 3000 2000 1000 0; do echo $s > $P/dsplog_skip; cat $P/dsplog; done |
		grep -v "Magic\|returned error" | awk '!seen[$0]++' > "$OUT/$1-dsplog.txt"
	echo 0 > $P/dsplog_skip
	wait
	sleep 1
}
echo 0 > $P/rt_lead_us; play lead0
echo "$LEAD" > $P/rt_lead_us; play lead$LEAD
echo 0 > $P/rt_lead_us
chown -R alarm: "$OUT"
grep -o "rdma7=[0-9a-f]*" "$OUT"/*-diag.txt
echo "saved in $OUT; rt_lead_us is back to 0"
