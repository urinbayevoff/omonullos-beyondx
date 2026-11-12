#!/bin/sh
# build modem/beyondx against out-beyondx and copy zrn-modem.ko (and
# zrn_drb.ko, /dev/drb for the RIL) to the phone
set -e
R=/mnt/novyi-tom/OmonullOS; B=$R/build
export PATH=$B/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-
rm -rf $B/modem-module; cp -r $R/modem/beyondx $B/modem-module
(cd $B/linux-7.1 && make -j$(nproc) O=out-beyondx M=$B/modem-module KBUILD_EXTRA_SYMBOLS=$B/pcie1.symvers modules > $B/modem-build.log 2>&1) || { grep -E "error:|undefined|multiple definition" $B/modem-build.log | head -20; exit 1; }
grep -E "WARNING: modpost|undefined" $B/modem-build.log || true
aarch64-none-linux-gnu-strip --strip-debug -o $B/zrn-modem.ko $B/modem-module/zrn-modem.ko
aarch64-none-linux-gnu-strip --strip-debug -o $B/zrn_drb.ko $B/modem-module/zrn_drb.ko
[ "$1" = --no-copy ] && exit 0
scp -q -i ~/.ssh/omonullos_ed25519 $B/zrn-modem.ko $B/zrn_drb.ko root@172.16.42.1:/usr/local/lib/zboot/
ssh -i ~/.ssh/omonullos_ed25519 root@172.16.42.1 sync
echo built and copied
