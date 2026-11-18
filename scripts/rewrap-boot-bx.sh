#!/bin/bash
# Rewrap an S10 5G (beyondx) boot image: keep its kernel, DTB and command
# line byte for byte, rebuild only uniLoader around them.
#
#   scripts/rewrap-boot-bx.sh <boot.img> <name> [--menu] [--fallback boot|recovery]
#     --menu       the boot menu (kernel/beyondx/bootmenu-bx.c): OmonullOS / TWRP
#     --fallback   where uniLoader's failsafe watchdog lands (default recovery,
#                  as build-boot.sh --device beyondx)
#   env: TC=<toolchain bin dir>  (default: build-boot.sh's x86_64 toolchain;
#        on the phone the aarch64-hosted Arm GNU Toolchain 14.2.Rel1, the
#        same compiler CI uses)
#        UNILOADER=<uniLoader tree> (default third_party/uniLoader)
#        GIT_VERSION_TAG, BUILD_DATE  passed to uniLoader's make (its
#        welcome line), to reproduce an image exactly
#
# Added for OmonullOS, November 2026 (dual boot step 2). A boot loader change
# should not change the kernel: the modules on the root filesystem are built
# for the kernel on the phone, and a new kernel build is a second variable in
# one boot. uniLoader links the kernel, the DTB and the ramdisk in as 4 KiB
# aligned sections after its code (arch/aarch64/linker.lds.S); they are found
# again by the arm64 Image magic, the FDT magic and the empty gzip that
# build-boot.sh puts there. Without --menu the output is the input image
# rebuilt, which is how the extraction and the toolchain are checked.
#
# Output: out/<name>.img (+ .sha256). Runs unprivileged, builds nothing heavy.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SRCIMG=${1:?source boot.img}
NAME=${2:?image name}
shift 2
MENU= FALLBACK=recovery
while [ $# -gt 0 ]; do
	case "$1" in
	--menu) MENU=--menu ;;
	--fallback) FALLBACK=$2; shift ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
case "$FALLBACK" in
boot) INFORM3=0x12345670 ;;
recovery) INFORM3=0x12345674 ;;
*) echo "--fallback boot|recovery" >&2; exit 2 ;;
esac
case "$NAME" in *[!A-Za-z0-9._-]*) echo "bad image name" >&2; exit 2 ;; esac
TC=${TC:-$ROOT/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin}
UNILOADER=${UNILOADER:-$ROOT/third_party/uniLoader}
[ -f "$UNILOADER/Makefile" ] || { echo "no uniLoader tree at $UNILOADER" >&2; exit 1; }
[ -x "$TC/aarch64-none-linux-gnu-gcc" ] || { echo "no toolchain in $TC" >&2; exit 1; }
export PATH=$TC:$PATH LC_ALL=C

W=$ROOT/build/rewrap-$NAME
STAGE=$W/uniloader
rm -rf "$W"
mkdir -p "$W" "$ROOT/out"

echo "=== extract $(basename "$SRCIMG") ==="
python3 -I - "$SRCIMG" "$W" <<'PY'
import struct, sys
src, w = sys.argv[1], sys.argv[2]
b = open(src, "rb").read()
FMT = "<8sIIIIIIIIII16s512s32s1024sIQI"
h = struct.unpack_from(FMT, b)
if h[0] != b"ANDROID!" or h[9] != 1:
    sys.exit("not a boot header v1 image")
ksz, rsz, page = h[1], h[3], h[8]
if rsz:
    sys.exit("image has a ramdisk section: not one of ours")
u = b[page:page + ksz]
ks = [o for o in range(4096, len(u), 4096) if u[o + 0x38:o + 0x3c] == b"ARM\x64"]
if len(ks) != 1:
    sys.exit(f"expected one embedded arm64 Image, found {len(ks)}")
k = ks[0]
ds = [o for o in range(k + 4096, len(u), 4096) if u[o:o + 4] == b"\xd0\x0d\xfe\xed"]
if not ds:
    sys.exit("no DTB after the kernel")
d = ds[0]
dsz = struct.unpack(">I", u[d + 4:d + 8])[0]
r = (d + dsz + 4095) // 4096 * 4096
empty = b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03\x03\x00" + b"\0" * 8	# gzip -n of nothing
if u[r:r + 20] != empty:
    sys.exit("no empty gzip ramdisk after the DTB")
# The Image's own length is not stored here (kernel_size is an adrp/add
# pair in uniLoader's code); its section runs to the DTB's 4 KiB boundary.
# The zeros after it are that alignment. An Image that itself ends in zero
# bytes would come out shorter: the rebuild without --menu, compared with
# the source image, is what proves the cut (bxci1: identical).
kern = u[k:d].rstrip(bytes(1))
open(f"{w}/Image", "wb").write(kern)
open(f"{w}/dtb", "wb").write(u[d:d + dsz])
open(f"{w}/ramdisk", "wb").write(empty)
open(f"{w}/cmdline", "w").write(h[12].rstrip(b"\0").decode())
open(f"{w}/uniloader.orig", "wb").write(u)
print(f"kernel {len(kern)} B at {k:#x} ({d - k - len(kern)} B alignment), "
      f"dtb {dsz} B at {d:#x}, cmdline {h[12].rstrip(bytes(1)).decode()!r}")
PY

echo "=== uniLoader ==="
cp -r "$UNILOADER" "$STAGE"
rm -rf "$STAGE/.git"
python3 "$ROOT/kernel/beyondx/uniloader-board.py" "$STAGE" $MENU
python3 "$ROOT/kernel/uniloader-failsafe.py" "$STAGE/board/samsung/board-beyondx.c" "$INFORM3"
mkdir -p "$STAGE/blob"
cp "$W/Image" "$STAGE/blob/Image"
cp "$W/dtb" "$STAGE/blob/dtb"
cp "$W/ramdisk" "$STAGE/blob/ramdisk"
MAKEVARS=()
[ -z "${GIT_VERSION_TAG:-}" ] || MAKEVARS+=("GIT_VERSION_TAG=$GIT_VERSION_TAG")
[ -z "${BUILD_DATE:-}" ] || MAKEVARS+=("BUILD_DATE=$BUILD_DATE")
(
	cd "$STAGE"
	make ARCH=aarch64 CROSS_COMPILE=aarch64-none-linux-gnu- beyondx_defconfig
	make ARCH=aarch64 CROSS_COMPILE=aarch64-none-linux-gnu- -j1 "${MAKEVARS[@]}"
) > "$W/uniloader-build.log" 2>&1 || { tail -n 30 "$W/uniloader-build.log"; exit 1; }
grep -E 'warning:' "$W/uniloader-build.log" | grep -i bootmenu || true
ls -l "$STAGE/uniLoader"

echo "=== boot image ==="
OUT=$ROOT/out/$NAME
rm -f "$OUT.img"
python3 -I "$ROOT/kernel/mkboot-v1.py" --ref "$SRCIMG" --kernel "$STAGE/uniLoader" \
	--cmdline "$(cat "$W/cmdline")" -o "$OUT.img"
(cd "$ROOT/out" && sha256sum "$NAME.img" > "$NAME.img.sha256" && cat "$NAME.img.sha256")
echo "REWRAP_OK $OUT.img from $(basename "$SRCIMG") menu=${MENU:-no} fallback=$FALLBACK"
