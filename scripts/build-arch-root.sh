#!/bin/bash
# Arch Linux ARM tarball -> ext4 image labelled ZR_ARCH, for USERDATA.
#
#   scripts/build-arch-root.sh [--device starlte|beyondx] [size, default 6G]
#
# Modified for OmonullOS, October 2026: --device beyondx (the Galaxy S10 5G)
# adds rootfs/overlay-beyondx and the beyondx helpers from kernel/beyondx
# (ztemp, zbatt, zcharge, zthermal, zchg, wdt-pet.sh, with busybox for
# devmem and the i2c tools) and enables zboot-guards.service, which keeps
# running there what initramfs ran. bsdtar or, without it, GNU tar unpacks.
#
# Runs unprivileged: the tarball is unpacked and the image is populated under
# fakeroot (mke2fs -d), so ownership and modes are right without a loop mount.
# What fakeroot cannot carry are extended attributes; the file capabilities of
# a few binaries (newuidmap, newgidmap) come back with their next package
# update. No chroot is used: everything that has to *run* on the target
# (pacman-key, growing the filesystem) is left to a first-boot unit.
#
# Output: out/arch-root.img (sparse). It grows to the whole partition on the
# first boot.
set -euo pipefail
export PATH=$PATH:/usr/sbin:/sbin
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
B=$ROOT/build
TAR=$B/alarm.tar.gz
R=$B/arch-root
STATE=$B/arch-root.fakeroot
DEVICE=starlte
if [ "${1:-}" = --device ]; then
	DEVICE=$2
	shift 2
fi
case "$DEVICE" in starlte|beyondx) ;; *) echo "--device starlte|beyondx" >&2; exit 2 ;; esac
SIZE=${1:-6G}
OVERLAY=$ROOT/rootfs/overlay
KEY=${ZBOOT_SSH_KEY:-$HOME/.ssh/id_ed25519.pub}

[ -s "$TAR" ] || { echo "missing $TAR" >&2; exit 1; }
if [ -s "$TAR.md5" ]; then
	[ "$(md5sum "$TAR" | cut -d' ' -f1)" = "$(cut -d' ' -f1 "$TAR.md5")" ] ||
		{ echo "md5 mismatch on $TAR" >&2; exit 1; }
	echo "tarball md5 ok"
fi

chmod -R u+rwX "$R" 2>/dev/null || true
rm -rf "$R" "$STATE"
mkdir -p "$R" "$ROOT/out"

echo "=== unpack ==="
if command -v bsdtar >/dev/null; then
	fakeroot -s "$STATE" -- bsdtar -xpf "$TAR" -C "$R" --no-xattrs --no-acls --no-fflags
else
	fakeroot -s "$STATE" -- tar -xpzf "$TAR" -C "$R" --no-xattrs --no-acls --numeric-owner \
		--warning=no-unknown-keyword
fi

echo "=== configure ==="
fakeroot -i "$STATE" -s "$STATE" -- bash -euo pipefail -s "$R" "$OVERLAY" "$KEY" "$DEVICE" "$ROOT" <<'EOS'
R=$1 OVERLAY=$2 KEY=$3 DEVICE=$4 ROOT=$5
# the NTFS checkout shows every file as 755 (systemd then warns about
# "executable" units and drop-ins): the overlay files get git's modes
git_modes() {
	git -C "$ROOT" ls-files -s "$1" | while read -r m _ _ f; do
		t=$R/${f#"$1"/}
		case $m in 100644|100755) [ -f "$t" ] && [ ! -L "$t" ] && chmod "${m#100}" "$t" ;; esac
	done
}
# the user helpers (user.sh, ztos_user.py, ztos-user-dropins) are ztOS's, not
# the phone overlay's; the overlay's own templates go over them
install -d "$R/usr/lib/ztos"
cp -a --no-preserve=ownership "$ROOT/os/overlay/usr/lib/ztos/." "$R/usr/lib/ztos/"
install -Dm755 "$ROOT/os/overlay/usr/local/sbin/ztos-user-dropins" "$R/usr/local/sbin/ztos-user-dropins"
cp -a --no-preserve=ownership "$OVERLAY/." "$R/"
git_modes rootfs/overlay
chown -R 0:0 "$R/etc/systemd" "$R/usr/local/sbin" "$R/etc/fstab" "$R/etc/hostname"
chmod 755 "$R"/usr/local/sbin/zboot-*
# sudo ignores a sudoers.d file that is not 0440 root:root
chown 0:0 "$R/etc/sudoers.d/10-wheel"
chmod 440 "$R/etc/sudoers.d/10-wheel"

# the kernel lives in the boot image, never in a package
sed -i 's/^#\?IgnorePkg *=.*/IgnorePkg   = linux-aarch64 linux-aarch64-headers linux-firmware/' "$R/etc/pacman.conf"
grep -q '^IgnorePkg' "$R/etc/pacman.conf"

W=$R/etc/systemd/system/multi-user.target.wants
mkdir -p "$W" "$R/etc/systemd/system/getty.target.wants"
ln -sf /etc/systemd/system/zboot-usb-gadget.service "$W/zboot-usb-gadget.service"
ln -sf /etc/systemd/system/zboot-firstboot.service "$W/zboot-firstboot.service"
ln -sf /usr/lib/systemd/system/serial-getty@.service "$R/etc/systemd/system/getty.target.wants/serial-getty@ttyGS0.service"
# no suspend: the big cores do not come back from it on this kernel
for t in sleep suspend hibernate hybrid-sleep suspend-then-hibernate; do
	ln -sf /dev/null "$R/etc/systemd/system/$t.target"
done
touch "$R/etc/zboot-firstboot"

if [ "$DEVICE" = beyondx ]; then
	cp -a --no-preserve=ownership "$ROOT/rootfs/overlay-beyondx/." "$R/"
	git_modes rootfs/overlay-beyondx
	chown 0:0 "$R/etc/hostname" "$R/etc/systemd/system/zboot-guards.service" \
		"$R/usr/local/sbin/zboot-guards"
	chmod 755 "$R/usr/local/sbin/zboot-guards"
	K=$ROOT/kernel/beyondx
	install -D -m 755 "$ROOT/build/busybox-arm64" "$R/usr/local/bin/busybox-zboot"
	for a in devmem i2cget i2cset i2ctransfer i2cdetect; do
		ln -sf busybox-zboot "$R/usr/local/bin/$a"
	done
	for t in ztemp zbatt zcharge zthermal zchg zlowbatt zchgoff zbattlog zpdoff; do
		install -m 755 "$K/$t.sh" "$R/usr/local/bin/$t"
	done
	install -m 755 "$K/zacpm.py" "$R/usr/local/bin/zacpm"
	install -m 755 "$K/zgov.py" "$R/usr/local/bin/zgov"
	install -m 755 "$K/ztouch.sh" "$R/usr/local/bin/ztouch"
	install -m 755 "$K/zgpu.sh" "$R/usr/local/bin/zgpu"
	# Panfrost and its scheduler (build-boot.sh --gpu), loaded by zgpu
	for m in gpu-sched panfrost; do
		[ ! -f "$ROOT/build/gpu-modules/$m.ko" ] ||
			install -D -m 644 "$ROOT/build/gpu-modules/$m.ko" \
				"$R/usr/local/lib/zboot/modules/$m.ko"
	done
	# DECON scanout (build-boot.sh --decon), loaded by zgpu too
	for m in drm_dma_helper bx-decon; do
		[ ! -f "$ROOT/build/decon-modules/$m.ko" ] ||
			install -D -m 644 "$ROOT/build/decon-modules/$m.ko" \
				"$R/usr/local/lib/zboot/modules/$m.ko"
	done
	# CPU idle (power/idle/beyondx/build.sh) and the hang log
	# (power/suspend/lastlog), both for 7.1.0-zboot
	for m in idle-bx/zrn-cpuidle-bx lastlog/zrn-lastlog; do
		[ ! -f "$ROOT/build/$m.ko" ] ||
			install -D -m 644 "$ROOT/build/$m.ko" \
				"$R/usr/lib/modules/7.1.0-zboot/extra/${m#*/}.ko"
	done
	[ ! -d "$R/usr/lib/modules/7.1.0-zboot" ] || depmod -b "$R" 7.1.0-zboot	# zlastlog's modprobe
	chmod 755 "$R/usr/local/sbin/zrn-cpuidle"
	install -D -m 644 "$K/wdt-pet.sh" "$R/usr/local/lib/zboot/wdt-pet.sh"
	chown -R 0:0 "$R/usr/local/bin" "$R/usr/local/lib/zboot"
	mkdir -p "$R/etc/systemd/system/sysinit.target.wants"
	ln -sf /etc/systemd/system/zboot-guards.service \
		"$R/etc/systemd/system/sysinit.target.wants/zboot-guards.service"
	# no shutdown initramfs: systemd-shutdown would "enter exitrd" (Arch's
	# generated /run/initramfs) and run that ramfs's system-shutdown hooks,
	# not zboot-poweroff, so poweroff ended halted and the watchdog brought
	# the phone back in TWRP (2026-10-18)
	ln -sf /dev/null "$R/etc/systemd/system/mkinitcpio-generate-shutdown-ramfs.service"
	# touch rails, CPU levels and the GPU at every boot: the owner asked, 2026-10-17
	mkdir -p "$R/etc/systemd/system/multi-user.target.wants"
	for u in ztouch zcpu zgpu zlowbatt zclock zbattlog zpdoff; do
		ln -sf /etc/systemd/system/$u.service \
			"$R/etc/systemd/system/multi-user.target.wants/$u.service"
	done
	# the hang log before anything else, idle cores once the session is up
	# (the owner said «ha» for both, 2026-10-18)
	ln -sf /etc/systemd/system/zlastlog.service \
		"$R/etc/systemd/system/sysinit.target.wants/zlastlog.service"
	mkdir -p "$R/etc/systemd/system/graphical.target.wants"
	ln -sf /etc/systemd/system/zrn-cpuidle.service \
		"$R/etc/systemd/system/graphical.target.wants/zrn-cpuidle.service"
	mkdir -p "$R/etc/systemd/system/poweroff.target.wants"
	ln -sf /etc/systemd/system/zchgoff.service \
		"$R/etc/systemd/system/poweroff.target.wants/zchgoff.service"
fi

# the stock image's user, alarm, is the phone's (zboot-firstboot fills in
# the unit drop-ins, greetd and sudoers from this)
IFS=: read -r _ _ uid gid _ home _ < <(grep '^alarm:' "$R/etc/passwd")
install -Dm644 /dev/stdin "$R/etc/ztos/user.conf" <<USERCONF
ZTOS_USER=alarm
ZTOS_UID=$uid
ZTOS_GID=$gid
ZTOS_HOME=$home
USERCONF

if [ -s "$KEY" ]; then
	for home in "$R/root" "$R/home/alarm"; do
		install -d -m 700 "$home/.ssh"
		install -m 600 "$KEY" "$home/.ssh/authorized_keys"
	done
	chown -R "$uid:$gid" "$R/home/alarm/.ssh"
	chown -R 0:0 "$R/root/.ssh"
fi
EOS

echo "=== image ==="
rm -f "$ROOT/out/arch-root.img"
fakeroot -i "$STATE" -- /usr/bin/mke2fs -q -t ext4 -L ZR_ARCH -m 1 -O ^orphan_file -d "$R" "$ROOT/out/arch-root.img" "$SIZE"
/usr/bin/e2fsck -fn "$ROOT/out/arch-root.img" | tail -n 2
ls -ls "$ROOT/out/arch-root.img"
echo "ARCH_ROOT_OK $ROOT/out/arch-root.img"
