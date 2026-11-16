#!/bin/bash
# Builds zrn-s6sy761.ko (touchscreen with ear detection) against the prepared
# 7.1 tree, on the phone: one job, small cores, low priority.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
B=${B:-$HOME/omonullos-src/build} K=$B/linux-7.1 M=$B/touch-ed
export PATH=$B/hostbin:$B/toolchain/arm-gnu-toolchain-14.2.rel1-$(uname -m)-aarch64-none-linux-gnu/bin:$PATH
export LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- KBUILD_MODPOST_WARN=1
O=out-beyondx
[ -x "$K/$O/scripts/mod/modpost" ] || O=out-snd
mkdir -p "$M"
cp "$ROOT/kernel/touch-ed/Kbuild" "$ROOT/kernel/touch-ed/zrn-s6sy761.c" "$M/"
taskset -c 0-3 nice -n 19 make -s -j1 -C "$K" O=$O M="$M" modules 2>&1 | grep -E "error|warning:" | grep -v "modpost:" || true
ls -l "$M/zrn-s6sy761.ko"
