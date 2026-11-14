#!/bin/bash
# power/suspend/beyondx/build.sh: zrn-sleep-check-bx.ko and
# zrn-system-sleep-bx.ko for the flashed
# kernel (7.1.0-zboot), into $B/sleep-bx. Same toolchain/tree as
# power/idle/beyondx/build.sh.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
S=$ROOT/power/suspend
B=${B:-$HOME/omonullos-src/build} K=$B/linux-7.1 M=$B/sleep-bx
export PATH=$B/hostbin:$B/toolchain/arm-gnu-toolchain-14.2.rel1-$(uname -m)-aarch64-none-linux-gnu/bin:$PATH
export LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- KBUILD_MODPOST_WARN=1
O=out-beyondx
[ -x "$K/$O/scripts/mod/modpost" ] || O=out-snd
# the tables are checked against the vendor tree where it is (the phone); the
# CI has none and builds the committed sleep-tables-9820.h as it is
if [ -d "$HOME/omonullos-src/ext/vk9820" ]; then
	python3 "$S/generate-tables.py" --soc 9820 --check
else
	echo "no vendor tree here: sleep-tables-9820.h as committed"
fi
mkdir -p "$M"
cp "$S/beyondx/Kbuild" "$S/sequence.c" "$S/sequence.h" "$S/sleep-tables-9820.h" "$M/"
# object name must differ from the module name
cp "$S/beyondx/zrn-sleep-check-bx.c" "$M/zrn-sleep-check-bx-main.c"
cp "$S/beyondx/zrn-system-sleep-bx.c" "$M/zrn-system-sleep-bx-main.c"
make -s -C "$K" O=$O M="$M" modules 2>&1 | grep -E "error|warning|undefined" || true
ls -l "$M/zrn-sleep-check-bx.ko" "$M/zrn-system-sleep-bx.ko"
