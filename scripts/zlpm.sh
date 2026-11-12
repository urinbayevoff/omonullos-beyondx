#!/bin/sh
# zlpm: write one group of stock lpm_init through zrn-cpuidle-bx's `lpm`
# (power/idle/beyondx/zrn-sicd-bx.c). Run as root:  zlpm.sh 1|2|4|8|16|32|64
# Only these masks; prints the old>new values of that group.
# Added for OmonullOS, November 2026.
P=/sys/module/zrn_cpuidle_bx/parameters
case "$1" in 1|2|4|8|16|32|64) ;; *) echo "usage: zlpm.sh 1|2|4|8|16|32|64"; exit 1;; esac
[ -e $P/lpm ] || { echo "zlpm: module has no lpm"; exit 1; }
sync
echo "$1" > $P/lpm || exit 1
echo "lpm done: $(cat $P/lpm)"
grep "^$1 " $P/lpm_log
