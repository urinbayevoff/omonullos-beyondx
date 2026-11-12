#!/bin/sh
# RDMA0 -> SPUS OUT0 -> SIFS0, paced by SIFS0's own counter
aplay -q -D hw:0,0 -f S16_LE -c 2 -r 48000 --buffer-size=16384 --period-size=1024 /dev/zero 2>/tmp/aplay.err &
A=$!
sleep 0.08
c0=$(zpoke 18c50200 | cut -d' ' -f3); c1=$(zpoke 18c50204 | cut -d' ' -f3)
zpoke 18c50200 $(printf %08x $(( (0x$c0 & ~0x1f) | 0x2 ))) >/dev/null
zpoke 18c50204 $(printf %08x $(( (0x$c1 & ~0x1f) | 9 ))) >/dev/null
zpoke 18c50290 000001ff >/dev/null
zpoke 18c51008 91000000 >/dev/null; zpoke 18c5100c 91010000 >/dev/null; zpoke 18c51010 1000 >/dev/null; zpoke 18c51014 91000000 >/dev/null
zpoke 18c51000 00400001 >/dev/null
for i in 1 2 3 4 5; do zpoke 18c51030; sleep 0.04; done
zpoke 18c50200; zpoke 18c50204
wait $A; cat /tmp/aplay.err
zpoke 18c51000 00400000 >/dev/null; zpoke 18c50290 0 >/dev/null
zpoke 18c50200 $c0 >/dev/null; zpoke 18c50204 $c1 >/dev/null
