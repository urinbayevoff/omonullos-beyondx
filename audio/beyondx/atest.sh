#!/bin/sh
# on the phone: load (if needed), play 4 s through vdma, sample DMA regs, DSP log
P=/sys/module/snd_zrn_abox_bx/parameters
[ -d $P ] || { /root/tools/zaudio-load.sh "$@"; sleep 5; }
dmesg -C
(timeout 4 aplay $AOPT -D hw:0,0 -f S16_LE -c 2 -r 48000 /dev/urandom > /tmp/aplay.out 2>&1 &)
sleep 0.2
for k in 1 2; do for a in 18c51000 18c51030 18c51700 18c51730 18c53300 18c53330 18c53400 18c53430 18c50510; do printf "%s=%s " $a $(zpoke $a 2>&1 | tail -1 | awk '{print $NF}'); done; echo; sleep 0.2; done
sleep 4; cat /tmp/aplay.out | tail -1
dmesg | grep -v "mif:\|zboot\|waited" | cut -c1-150 | tail -6
for s in 3000 1500 0; do echo $s > $P/dsplog_skip; cat $P/dsplog; done | grep -v "Magic\|returned error\|abox_ipc_recv$" | cut -c1-170 | awk '!seen[$0]++' | grep -v "^ *$" | tail -${N:-25}
echo 0 > $P/dsplog_skip
