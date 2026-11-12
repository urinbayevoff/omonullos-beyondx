#!/bin/bash
# Fetch the build inputs scripts/build-boot.sh expects under build/: the
# submodules, linux-7.1 and the ARM toolchain (sha256 checked), the static
# busybox. Safe to run again: what is there is kept.
#
# Added for OmonullOS, October 2026.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$ROOT/build"
cd "$ROOT/build"
log() { echo "[$(date +%H:%M:%S)] $*"; }
log "s9plus submodule"
git -C "$ROOT" submodule update --init third_party/s9plus third_party/uniLoader && log "submodules ok"
log "linux-7.1"
if [ ! -d linux-7.1 ]; then
  wget -q -c https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.1.tar.xz && \
  wget -q -c https://cdn.kernel.org/pub/linux/kernel/v7.x/sha256sums.asc -O linux-sha256sums.asc && \
  grep " linux-7.1.tar.xz" linux-sha256sums.asc | sha256sum -c - && \
  tar xf linux-7.1.tar.xz && log "linux-7.1 ok"
fi
log "ARM toolchain 14.2.rel1"
T=arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu
mkdir -p toolchain
if [ ! -d toolchain/$T ]; then
  wget -q -c https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/$T.tar.xz && \
  wget -q -c https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/$T.tar.xz.sha256asc && \
  sha256sum -c $T.tar.xz.sha256asc && \
  tar xf $T.tar.xz -C toolchain && log "toolchain ok"
fi
log "busybox (static arm64, Ubuntu busybox-static, sha256 from the signed index)"
[ -x busybox-arm64 ] || python3 -I "$ROOT"/third_party/s9plus/scripts/build/fetch_busybox.py busybox-arm64
log "done"
