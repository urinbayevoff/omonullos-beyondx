#!/bin/bash
# audio/beyondx/install-on-phone.sh: put the snd-zrn-abox-bx.ko that build.sh
# made on the phone (no PC, November 2026) where the boot loads it from, with
# a backup, plus audio/test-call-path.sh into /root/tools. Run as root
# (`sudo bash audio/beyondx/install-on-phone.sh`), then reboot: no rmmod.
#
# Undo: the printed `cp ... .bak-<date>` line back, then reboot.
#
# Added for OmonullOS, November 2026.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
NEW=/home/alarm/omonullos-src/build/abox-bx/out/snd-zrn-abox-bx.ko
[ -f "$NEW" ] || { echo "missing $NEW (run audio/beyondx/build.sh)" >&2; exit 1; }
[ "$(modinfo -F vermagic "$NEW")" = "$(uname -r) SMP preempt mod_unload aarch64" ] ||
	{ echo "vermagic does not fit $(uname -r)" >&2; exit 1; }

# where the boot takes the module from: the paths the load script names
mapfile -t OLD < <(grep -rhoE '/[^ "]*snd-zrn-abox-bx\.ko' /root/tools/zaudio-load.sh /etc/systemd/system/*.service 2>/dev/null | sort -u)
if [ "${#OLD[@]}" != 1 ] || [ ! -f "${OLD[0]}" ]; then
	echo "cannot tell which file the boot loads (found: ${OLD[*]:-none}); nothing changed" >&2
	exit 1
fi
OLD=${OLD[0]}
BAK=$OLD.bak-$(date +%m%d-%H%M)
cp -a "$OLD" "$BAK"
install -m644 "$NEW" "$OLD"
install -m755 "$ROOT/audio/test-call-path.sh" /root/tools/test-call-path.sh
sync
sha256sum "$OLD" "$BAK"
echo "installed; undo: cp -a $BAK $OLD && sync"
echo "now: sudo reboot, then: dmesg | grep -i 'vss mark'"
