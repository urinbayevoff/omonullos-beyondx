#!/bin/sh
# Build rild-lite (modem/ril/rild-lite.c) for the phone's bionic with the NDK
# (r27c, API 31, ~/omonullos-src/ndk) and, with --push, copy it into zmodem's
# Android root on the phone (/var/lib/zmodem/data/zril, seen as /data/zril).
# Added for OmonullOS, October 2026.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${NDK_CC:-$HOME/omonullos-src/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang}
OUT=$ROOT/build/rild-lite
"$CC" -O2 -Wall -o "$OUT" "$ROOT/modem/ril/rild-lite.c"
echo "built $OUT"
if [ "$1" = --push ]; then
	K="-i $HOME/.ssh/omonullos_ed25519"
	scp -q $K "$OUT" root@172.16.42.1:/var/lib/zmodem/data/zril/rild-lite
	ssh $K root@172.16.42.1 'chmod 755 /var/lib/zmodem/data/zril/rild-lite; sync'
	echo "pushed"
fi
