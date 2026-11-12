#!/bin/bash
# power/reboot/beyondx/build.sh: zreboot-bx.ko for the flashed kernel
# (7.1.0-zboot, out-beyondx), into $B/reboot-bx.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
B=$HOME/omonullos-src/build K=$B/linux-7.1 M=$B/reboot-bx
export PATH=$B/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH
export LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-
mkdir -p "$M"
cp "$ROOT/power/reboot/beyondx/Kbuild" "$ROOT/power/reboot/beyondx/zreboot-bx.c" "$M/"
make -s -C "$K" O=out-beyondx M="$M" modules 2>&1 | grep -E "error|warning|undefined" || true
ls -l "$M/zreboot-bx.ko"
