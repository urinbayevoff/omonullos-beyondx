#!/bin/sh
# RDMA0 driven from the AP while aplay holds the stream open
aplay -q -D hw:0,0 -f S16_LE -c 2 -r 48000 --buffer-size=16384 --period-size=1024 /dev/zero 2>/tmp/aplay.err &
A=$!
sleep 0.08
zpoke 18c51008 91000000; zpoke 18c5100c 91010000; zpoke 18c51010 1000; zpoke 18c51014 91000000
zpoke 18c51000 00400001
for i in 1 2 3 4 5 6; do echo "ctrl0=$(zpoke 18c51000) st=$(zpoke 18c51030)"; sleep 0.1; done
sleep 1; kill $A 2>/dev/null; wait $A; echo "aplay rc=$?"; cat /tmp/aplay.err
zpoke 18c51000 00400000
