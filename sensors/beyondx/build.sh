#!/bin/bash
# Builds zrn-bbd-bx.ko (the S10 5G's sensor hub transport) against the
# prepared 7.1 tree, on the phone. Gentle on purpose: one job on the small
# cores (a -j8 build overheated the phone, 2026-11-15).
#   PATCH=FILE   recovery image or patch text for mk-patch-h.py
#                (default ~/omonullos-stock/sensorhub/bcm_sensorhub_a1-0.patch)
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
B=${B:-$HOME/omonullos-src/build} K=$B/linux-7.1 M=$B/bbd-bx
PATCH=${PATCH:-$HOME/omonullos-stock/sensorhub/bcm_sensorhub_a1-0.patch}
export PATH=$B/hostbin:$B/toolchain/arm-gnu-toolchain-14.2.rel1-$(uname -m)-aarch64-none-linux-gnu/bin:$PATH
export LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- KBUILD_MODPOST_WARN=1
O=out-beyondx
[ -x "$K/$O/scripts/mod/modpost" ] || O=out-snd
mkdir -p "$M/r_os" "$M/linux"
cp "$ROOT"/sensors/driver/*.c "$ROOT"/sensors/driver/*.h "$M/"
cp "$ROOT"/sensors/driver/linux/*.h "$M/linux/"
rm -f "$M/zrn_bbd_board.c"
cp "$ROOT/sensors/beyondx/Kbuild" "$ROOT/sensors/beyondx/zrn_bbd_board_bx.c" "$M/"
[ -s "$M/r_os/bbd_patch_file_beyond.h" ] || python3 -I "$ROOT/sensors/beyondx/mk-patch-h.py" "$PATCH" "$M/r_os/bbd_patch_file_beyond.h"
taskset -c 0-3 nice -n 19 make -s -j1 -C "$K" O=$O M="$M" modules 2>&1 | grep -E "error|warning|undefined" || true
ls -l "$M/zrn-bbd-bx.ko"
