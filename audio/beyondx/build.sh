#!/bin/bash
# audio/beyondx/build.sh: snd-zrn-abox-bx.ko and the sound core it needs, as
# modules for the running kernel (7.1.0-zboot), into $B/abox-bx/out.
#
# The boot image's kernel has no sound core. out-snd is out-beyondx's
# configuration with SOUND/SND/SND_PCM added as modules (as ztOS's
# audio/build.sh does for the S9), nothing built-in changed.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
B=${B:-$HOME/omonullos-src/build} K=$B/linux-7.1 M=$B/abox-bx
# the same ARM GNU 14.2.Rel1 the flashed kernel was built with: x86_64-hosted
# on the PC, aarch64-hosted on the phone itself (no PC, November 2026; the
# phone has no bc, so busybox's applet stands in from $B/hostbin)
export PATH=$B/hostbin:$B/toolchain/arm-gnu-toolchain-14.2.rel1-$(uname -m)-aarch64-none-linux-gnu/bin:$PATH
export LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- KBUILD_MODPOST_WARN=1

if [ ! -f "$K/out-snd/.config" ]; then
	mkdir -p "$K/out-snd" && cp "$K/out-beyondx/.config" "$K/out-snd/.config"
	(cd "$K" && scripts/config --file out-snd/.config -m SOUND -m SND -m SND_DUMMY -e SND_DRIVERS \
		-d SND_SOC -d SND_USB -d SND_HRTIMER -d SND_ALOOP -d SND_OSSEMUL -d SND_SEQUENCER -d SND_PCI \
		-d SND_HDA -d SND_SPI -d SND_ARM -d SND_VIRTIO)
	make -s -C "$K" O=out-snd olddefconfig
	# nothing built-in outside the sound core may change, or the modules do
	# not fit the flashed kernel (no MODVERSIONS: vermagic only)
	if diff <(grep '=y' "$K/out-beyondx/.config" | sort) <(grep '=y' "$K/out-snd/.config" | sort) |
			grep '^[<>]' | grep -v '^> CONFIG_SND_'; then
		echo "out-snd changes built-in options (above)"; exit 1
	fi
	make -s -j"$(nproc)" -C "$K" O=out-snd modules_prepare
	# the flashed kernel's exports; a tree made on the phone has none (no full
	# build there), and modpost then only warns (KBUILD_MODPOST_WARN=1)
	[ ! -f "$K/out-beyondx/Module.symvers" ] || cp "$K/out-beyondx/Module.symvers" "$K/out-snd/Module.symvers"
fi
make -s -j"$(nproc)" -C "$K" O=out-snd sound/soundcore.ko sound/core/snd.ko sound/core/snd-timer.ko \
	sound/core/snd-pcm.ko drivers/i2c/algos/i2c-algo-bit.ko 2>&1 | grep -E "error|warning: [a-z]" || true
mkdir -p "$M"
cp "$ROOT/audio/beyondx/Kbuild" "$M/Kbuild"
cp "$ROOT/audio/beyondx/snd-zrn-abox-bx.c" "$M/abox.c"
cp "$K/sound/soc/codecs/cs35l41-lib.c" "$M/cs35l41-lib.c"
cp "$K/drivers/firmware/cirrus/cs_dsp.c" "$K/drivers/firmware/cirrus/cs_dsp.h" "$M/"
# a tuning (.bin) into a booted core, as stock cirrus-cal.c's cs35l41_load_config
# (dsp_calibrate); cs_dsp keeps cs_dsp_load_coeff static
cat >> "$M/cs_dsp.c" <<'EOF'

int zrn_cs_dsp_load_coeff(struct cs_dsp *dsp, const struct firmware *fw, const char *file);
int zrn_cs_dsp_load_coeff(struct cs_dsp *dsp, const struct firmware *fw, const char *file)
{
	int ret;

	mutex_lock(&dsp->pwr_lock);
	ret = cs_dsp_load_coeff(dsp, fw, file);
	mutex_unlock(&dsp->pwr_lock);
	return ret;
}
EOF
make -s -C "$K" O=out-snd M="$M" modules 2>&1 | grep -E "error|warning|undefined" || true
mkdir -p "$M/out"
cp "$K"/out-snd/sound/soundcore.ko "$K"/out-snd/sound/core/snd{,-timer,-pcm}.ko \
	"$K"/out-snd/drivers/i2c/algos/i2c-algo-bit.ko "$M/snd-zrn-abox-bx.ko" "$M/out/"
ls -l "$M/out"
