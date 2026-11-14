#!/bin/bash
# power/idle/beyondx/build.sh: zrn-cpuidle-bx.ko for the flashed kernel
# (7.1.0-zboot, out-beyondx), into $B/idle-bx.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
B=${B:-$HOME/omonullos-src/build} K=$B/linux-7.1 M=$B/idle-bx
# x86_64-hosted toolchain on the PC, aarch64-hosted on the phone itself
# (November 2026: no PC); the phone's tree is out-snd (audio/beyondx/build.sh
# prepares it: out-beyondx's built-in options, modules_prepare done)
export PATH=$B/hostbin:$B/toolchain/arm-gnu-toolchain-14.2.rel1-$(uname -m)-aarch64-none-linux-gnu/bin:$PATH
export LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- KBUILD_MODPOST_WARN=1
O=out-beyondx
[ -x "$K/$O/scripts/mod/modpost" ] || O=out-snd
mkdir -p "$M"
cp "$ROOT/power/idle/beyondx/Kbuild" "$ROOT/power/idle/beyondx/zrn-idle-bx.c" \
	"$ROOT/power/idle/beyondx/zrn-sicd-bx.c" "$ROOT/power/idle/beyondx/zrn-sicd-bx.h" "$M/"
cp "$ROOT/power/idle/zrn-idle.h" "$B/"	# ../zrn-idle.h from $M
make -s -C "$K" O=$O M="$M" modules 2>&1 | grep -E "error|warning|undefined" || true
ls -l "$M/zrn-cpuidle-bx.ko"
