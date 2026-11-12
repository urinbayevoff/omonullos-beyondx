#!/bin/sh
# build kernel/beyondx/pcie1 (PCIe channel 1, S5100) against out-beyondx and
# copy pcie-exynos9820-ch1.ko to the phone's /root/tools
set -e
R=/mnt/novyi-tom/OmonullOS; B=$R/build; S=$R/kernel/beyondx/pcie1
export PATH=$B/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-
DTC=$B/linux-7.1/out-beyondx/scripts/dtc/dtc
rm -rf $B/pcie1-module; cp -r $S $B/pcie1-module; cd $B/pcie1-module
mkdir -p include/linux; cp $R/modem/beyondx/zinc/linux/exynos-pci-noti.h include/linux/
$DTC -q -I dts -O dtb -o pcie1-overlay.dtbo pcie1-overlay.dts
python3 - <<'PY'
d = open('pcie1-overlay.dtbo', 'rb').read()
body = ",".join("0x%02x" % b for b in d)
open('pcie1_overlay_blob.c', 'w').write(
    "/* generated from pcie1-overlay.dts by scripts/bx-pcie1-build.sh */\n"
    "#include <linux/types.h>\nconst u8 pcie1_overlay[] __aligned(8) = {%s};\n"
    "const u32 pcie1_overlay_size = sizeof(pcie1_overlay);\n" % body)
PY
(cd $B/linux-7.1 && make -j$(nproc) O=out-beyondx M=$B/pcie1-module modules > $B/pcie1-build.log 2>&1) || { grep -E "error|undefined" $B/pcie1-build.log | head -20; exit 1; }
grep -E "warning|WARNING" $B/pcie1-build.log || true
aarch64-none-linux-gnu-strip --strip-debug -o $B/pcie-exynos9820-ch1.ko $B/pcie1-module/pcie-exynos9820-ch1.ko
cp $B/pcie1-module/Module.symvers $B/pcie1.symvers
[ "$1" = --no-copy ] && exit 0
scp -q -i ~/.ssh/omonullos_ed25519 $B/pcie-exynos9820-ch1.ko root@172.16.42.1:/root/tools/
ssh -i ~/.ssh/omonullos_ed25519 root@172.16.42.1 sync
echo built and copied
