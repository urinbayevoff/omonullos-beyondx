#!/bin/sh
# zsicd-quiet: unlimited SICD + CPD with the wakeup probe at the same time,
# with chosen services stopped, so Claude Code can be closed meanwhile.
# Run detached as root, then close Claude within WAIT seconds:
#	sudo systemd-run --unit zquiet sh ~/omonullos/scripts/zsicd-quiet.sh [SECONDS]
#	STOP="zphone-tunnel zlowbatt" ...	services to stop for the run (default
#		zphone-tunnel = cloudflared, the biggest waker); STOP= keeps all
# Services are started again at the end. Results: /var/lib/zdrain/sicd-*.txt
# and wake-*.txt. Same risk as zsicd-trial.sh 0: someone at the phone.
# Added for OmonullOS, November 2026.
SECS=${1:-60}
STOP=${STOP-zphone-tunnel}
D=$(dirname "$(readlink -f "$0")")
sleep "${WAIT:-20}"
for u in $STOP; do systemctl stop "$u"; done
trap 'for u in $STOP; do systemctl start "$u"; done; sync' EXIT INT TERM
sh "$D/zwake-probe.sh" "$SECS" > /dev/null 2>&1 &
CPD=${CPD:-2} sh "$D/zsicd-trial.sh" 0 "$SECS"
wait
