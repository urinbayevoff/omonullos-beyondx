#!/bin/bash
# The kernel-side files of the beyondx root image (build-beyondx-image.sh
# --extra): every external module built in the same kernel tree as the BOOT
# image, rild-lite and busybox, laid out as in the image.
#
#   scripts/ci-beyondx-extra.sh OUTDIR
#
# Run after scripts/build-boot.sh --device beyondx ... --gpu --decon --wlan
# (it leaves build/linux-7.1/out-beyondx and build/{gpu,decon,wlan}-modules).
# rild-lite needs the NDK: NDK_CC=<aarch64-linux-android31-clang>.
# The CI's first job (.github/workflows/build-image-beyondx.yml) runs it.
#
# Added for OmonullOS, November 2026.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:?OUTDIR}
export B=$ROOT/build
K=$B/linux-7.1
KVER=7.1.0-zboot
TC=$B/toolchain/arm-gnu-toolchain-14.2.rel1-$(uname -m)-aarch64-none-linux-gnu/bin
export PATH=$TC:$PATH
STRIP=aarch64-none-linux-gnu-strip
say() { echo "=== $*"; }
die() { echo "ci-beyondx-extra: $*" >&2; exit 1; }

[ -f "$K/out-beyondx/.config" ] || die "no $K/out-beyondx (run build-boot.sh first)"
for d in gpu-modules decon-modules wlan-modules; do
	[ -d "$B/$d" ] || die "no $B/$d (build-boot.sh needs --gpu --decon --wlan)"
done

say "pcie1 (S5100 channel)"
sh "$ROOT/scripts/bx-pcie1-build.sh" --no-copy
say "modem"
sh "$ROOT/scripts/bx-modem-build.sh" --no-copy
say "sound"
bash "$ROOT/audio/beyondx/build.sh"
say "reboot"
bash "$ROOT/power/reboot/beyondx/build.sh"
say "cpuidle"
bash "$ROOT/power/idle/beyondx/build.sh"
say "system sleep"
bash "$ROOT/power/suspend/beyondx/build.sh"
say "lastlog"
rm -rf "$B/lastlog"; cp -r "$ROOT/power/suspend/lastlog" "$B/lastlog"
make -s -C "$K" O=out-beyondx M="$B/lastlog" ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- modules
say "rild-lite"
sh "$ROOT/scripts/bx-ril-build.sh"

say "tree"
rm -rf "$OUT"
Z=$OUT/usr/local/lib/zboot X=$OUT/usr/lib/modules/$KVER/extra
install -d "$Z/modules" "$Z/tools" "$X/snd" "$OUT/usr/local/bin" "$OUT/var/lib/zmodem/data/zril"
put() { install -m644 "$1" "$2/"; }
for m in "$B"/gpu-modules/*.ko "$B"/decon-modules/*.ko; do put "$m" "$Z/modules"; done
put "$B/zrn-modem.ko" "$Z"; put "$B/zrn_drb.ko" "$Z"; put "$B/pcie-exynos9820-ch1.ko" "$Z"
put "$B/abox-bx/out/snd-zrn-abox-bx.ko" "$Z/tools"
put "$B/sleep-bx/zrn-system-sleep-bx.ko" "$Z/tools"	# zsleepd
for m in "$B"/wlan-modules/*.ko "$B/reboot-bx/zreboot-bx.ko" "$B/idle-bx/zrn-cpuidle-bx.ko" \
		"$B/lastlog/zrn-lastlog.ko"; do
	put "$m" "$X"
done
for m in soundcore snd snd-timer snd-pcm i2c-algo-bit; do put "$B/abox-bx/out/$m.ko" "$X/snd"; done
find "$OUT" -name '*.ko' -exec $STRIP --strip-debug {} +
install -m755 "$B/busybox-arm64" "$OUT/usr/local/bin/busybox-zboot"
install -m755 "$ROOT/build/rild-lite" "$OUT/var/lib/zmodem/data/zril/rild-lite"

# every module must load into this BOOT image's kernel (vermagic only, no MODVERSIONS)
want=$(modinfo -F vermagic "$Z/modules/gpu-sched.ko")
case $want in "$KVER "*) ;; *) die "unexpected vermagic: $want" ;; esac
bad=0
while read -r m; do
	v=$(modinfo -F vermagic "$m")
	[ "$v" = "$want" ] || { echo "vermagic $m: $v" >&2; bad=1; }
done < <(find "$OUT" -name '*.ko')
[ $bad = 0 ] || die "vermagic mismatch"
echo "vermagic: $want"
(cd "$OUT" && find . -type f | sort | xargs sha256sum) > "$OUT.sha256"
cat "$OUT.sha256"
echo "BEYONDX_EXTRA_OK $OUT"
