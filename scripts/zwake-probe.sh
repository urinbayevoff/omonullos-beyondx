#!/bin/sh
# zwake-probe: what wakes the CPUs with the screen off (read-only).
# Run as root with Claude Code closed (it alone wakes the cores ~250/s):
#	sudo sh ~/omonullos/scripts/zwake-probe.sh [SECONDS]
# Turns the screen off through ztMETRO's pipe, counts interrupts per CPU and
# context switches per thread for SECONDS (default 30), turns the screen
# back on. Writes /var/lib/zdrain/wake-<time>.txt (synced) and prints it.
# Nothing is written to hardware. Added for OmonullOS, November 2026.
SECS=${1:-30}
D=$(dirname "$0")
CTL=/run/user/1000/ztmetro.ctl
OUT=/var/lib/zdrain/wake-$(date +%Y%m%d-%H%M%S).txt
mkdir -p /var/lib/zdrain
T=$(mktemp -d)
timeout 2 sh -c "echo 'screen off' > $CTL"
sleep 5
cat /proc/interrupts > $T/i0
P=/sys/module/zrn_cpuidle_bx/parameters
[ -r $P/stats ] && cat $P/stats > $T/s0 2>/dev/null
python3 -I "$D/zwake-ctx.py" "$SECS" > $T/ctx
cat /proc/interrupts > $T/i1
cat /proc/timer_list > $T/tl 2>/dev/null
timeout 2 sh -c "echo 'screen on' > $CTL"
{
	echo "zwake-probe $(date +%T), $SECS s, screen off, procs: $(ls /proc | grep -c '^[0-9]')"
	pgrep -l claude >/dev/null && echo "WARNING: claude is running (adds ~250 wakeups/s)"
	python3 -I - $T/i0 $T/i1 "$SECS" <<'PY'
import sys
def rd(f):
    L=open(f).read().splitlines(); n=len(L[0].split()); d={}
    for l in L[1:]:
        p=l.split(); k=p[0].rstrip(':'); v=[]
        for x in p[1:n+1]:
            if x.isdigit(): v.append(int(x))
            else: break
        d[k]=(v,' '.join(p[1+len(v):]))
    return d,n
a,n=rd(sys.argv[1]); b,_=rd(sys.argv[2]); dt=float(sys.argv[3])
rows=[]
for k in b:
    if k in a and b[k][0] and len(a[k][0])==len(b[k][0]):
        dv=[y-x for x,y in zip(a[k][0],b[k][0])]
        if sum(dv): rows.append((sum(dv),k,dv,b[k][1][:45]))
rows.sort(reverse=True); tot=[0]*n
for s,k,dv,nm in rows:
    for i,x in enumerate(dv): tot[i]+=x
print("irq/s per cpu:", [round(x/dt) for x in tot])
for s,k,dv,nm in rows[:15]:
    print(f"{k:>6} {s/dt:7.1f}/s {[round(x/dt,1) for x in dv]} {nm}")
PY
	echo "--- threads"
	head -25 $T/ctx
	echo "--- timer_list callbacks"
	grep -oE ", [a-z_0-9]+, S:" $T/tl | sort | uniq -c | sort -rn | head -10
} > "$OUT" 2>&1
sync
cat "$OUT"
rm -rf $T
