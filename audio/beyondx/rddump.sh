#!/bin/sh
# ABOX state while a stream runs, RDMA0 enabled from the AP
r() { for a in "$@"; do zpoke $a; done | tr '\n' ' '; echo; }
aplay -q -D hw:0,0 -f S16_LE -c 2 -r 48000 --buffer-size=16384 --period-size=1024 /dev/zero 2>/tmp/aplay.err &
A=$!
sleep 0.08
zpoke 18c51008 91000000 >/dev/null; zpoke 18c5100c 91010000 >/dev/null; zpoke 18c51010 1000 >/dev/null; zpoke 18c51014 91000000 >/dev/null
zpoke 18c51000 00400001 >/dev/null
sleep 0.05
echo "-- sys/route"; r 18c50010 18c50014 18c50040 18c50044
echo "-- spus";      r 18c50200 18c50204 18c50208 18c5020c 18c50210 18c50214 18c50218 18c50220
echo "-- sifs cnt";  r 18c50290 18c50294 18c50298 18c5029c
echo "-- uaif1";     r 18c50510 18c50514 18c50518 18c5051c
echo "-- rdma0";     r 18c51000 18c51004 18c51008 18c5100c 18c51010 18c51014 18c51020 18c51030
echo "-- sysmmu";    r 18e00000 18e00060 18e00070
sleep 0.05; r 18c51030 18c5051c
wait $A; cat /tmp/aplay.err
