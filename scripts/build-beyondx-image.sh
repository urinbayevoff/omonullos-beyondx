#!/bin/bash
# The OmonullOS root filesystem for the Galaxy S10 5G (beyondx), whole:
# Arch Linux ARM, the packages in image/beyondx/packages.txt, ztMETRO and its
# apps built from this tree, the overlays, the beyondx helpers and services,
# and the Commons wallpapers. The result is an ext4 image labelled ZR_ARCH
# for USERDATA, the same layout as the tested phone (docs/s10-5g/IMAGE-BUILD.md).
#
#   sudo scripts/build-beyondx-image.sh [--extra DIR] [--size 8G]
#                                       [--allow-missing] [--out FILE]
#
# --extra DIR   a tree laid out like the image (the CI's first job: kernel
#               modules built in one kernel tree, rild-lite, busybox), copied
#               over the root as is; see EXTRA_NEEDED below for what it holds
# --allow-missing  build even when EXTRA_NEEDED files are absent (a dry run of
#               the rest; such an image has no Wi-Fi, sound or modem)
#
# Runs as root: an aarch64 chroot (natively on an arm64 host, else through
# qemu-user-static's binfmt handler) installs the packages and builds the C
# programs. Nothing is written to any block device: the image is a file made
# with mke2fs -d. Firmware is not in it: zvendor-firmware.service copies it
# from the phone's own VENDOR partition on the first boot.
#
# Added for OmonullOS, November 2026.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=$SRC/build/beyondx-image
R=$WORK/root
TAR=$SRC/build/alarm.tar.gz
TAR_URL=http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz
PKGCACHE=$SRC/build/pkg-cache
KVER=7.1.0-zboot
EXTRA= SIZE=8G STRICT=1 OUT=$SRC/out/omonullos-beyondx-root.img

while [ $# -gt 0 ]; do
	case $1 in
	--extra) EXTRA=$(cd "$2" && pwd); shift 2 ;;
	--size) SIZE=$2; shift 2 ;;
	--out) OUT=$2; shift 2 ;;
	--allow-missing) STRICT=0; shift ;;
	*) sed -n '2,22p' "$0" >&2; exit 2 ;;
	esac
done

# what only the CI's kernel job can make (paths in the image)
EXTRA_NEEDED="
usr/local/bin/busybox-zboot
usr/local/lib/zboot/modules/gpu-sched.ko
usr/local/lib/zboot/modules/panfrost.ko
usr/local/lib/zboot/modules/drm_dma_helper.ko
usr/local/lib/zboot/modules/bx-decon.ko
usr/local/lib/zboot/zrn-modem.ko
usr/local/lib/zboot/zrn_drb.ko
usr/local/lib/zboot/pcie-exynos9820-ch1.ko
usr/local/lib/zboot/tools/snd-zrn-abox-bx.ko
usr/lib/modules/$KVER/extra/pcie-exynos9820.ko
usr/lib/modules/$KVER/extra/cfg80211.ko
usr/lib/modules/$KVER/extra/brcmutil.ko
usr/lib/modules/$KVER/extra/brcmfmac.ko
usr/lib/modules/$KVER/extra/brcmfmac-wcc.ko
usr/lib/modules/$KVER/extra/zreboot-bx.ko
usr/lib/modules/$KVER/extra/zrn-cpuidle-bx.ko
usr/lib/modules/$KVER/extra/zrn-lastlog.ko
usr/lib/modules/$KVER/extra/snd/soundcore.ko
usr/lib/modules/$KVER/extra/snd/snd.ko
usr/lib/modules/$KVER/extra/snd/snd-timer.ko
usr/lib/modules/$KVER/extra/snd/snd-pcm.ko
usr/lib/modules/$KVER/extra/snd/i2c-algo-bit.ko
var/lib/zmodem/data/zril/rild-lite
"

say() { echo "=== $*"; }
die() { echo "build-beyondx-image: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root (sudo)"
for t in bsdtar chroot mke2fs e2fsck python3 curl; do
	command -v $t >/dev/null || die "missing $t"
done
case $(uname -m) in
aarch64|arm64) ;;
*) [ -e /proc/sys/fs/binfmt_misc/qemu-aarch64 ] || die "no qemu-aarch64 binfmt (qemu-user-static)" ;;
esac

# --- what is missing, before an hour of work ---
missing=
for f in $EXTRA_NEEDED; do
	[ -n "$EXTRA" ] && [ -s "$EXTRA/$f" ] && continue
	# the ALSA core modules are also kept built in prebuilts/modules
	case $f in */extra/snd/*) [ -s "$SRC/prebuilts/modules/${f##*/}" ] && continue ;; esac
	missing="$missing $f"
done
if [ -n "$missing" ]; then
	echo "not in --extra:" $missing >&2
	[ $STRICT = 1 ] && die "missing kernel-job files (--allow-missing for a dry run)"
	echo "WARNING: building without them" >&2
fi

# --- unpack ---
if [ ! -s "$TAR" ]; then
	say "downloading Arch Linux ARM"
	mkdir -p "${TAR%/*}"
	curl -L --fail -o "$TAR.part" "$TAR_URL"
	curl -L --fail -o "$TAR.md5" "$TAR_URL.md5"
	mv "$TAR.part" "$TAR"
fi
if [ -s "$TAR.md5" ]; then
	[ "$(md5sum "$TAR" | cut -d' ' -f1)" = "$(cut -d' ' -f1 "$TAR.md5")" ] || die "md5 mismatch on $TAR"
fi

cleanup() {
	for m in var/cache/pacman/pkg tmp proc sys dev; do
		mountpoint -q "$R/$m" 2>/dev/null && umount -lR "$R/$m"
	done
	mountpoint -q "$R" 2>/dev/null && umount -l "$R"
	true
}
trap cleanup EXIT
cleanup
rm -rf "$WORK"
mkdir -p "$R" "$PKGCACHE" "$(dirname "$OUT")"
say "unpacking Arch Linux ARM"
bsdtar -xpf "$TAR" -C "$R" --no-xattrs --no-acls --no-fflags

mount --bind "$R" "$R"
mount --rbind /dev "$R/dev"
mount -t proc proc "$R/proc"
mount -t sysfs sys "$R/sys"
mount -t tmpfs tmpfs "$R/tmp"
mkdir -p "$R/var/cache/pacman/pkg"
mount --bind "$PKGCACHE" "$R/var/cache/pacman/pkg"
rm -f "$R/etc/resolv.conf"; cp -L /etc/resolv.conf "$R/etc/resolv.conf"
ch() { chroot "$R" /usr/bin/env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/bin HOME=/root LANG=C.UTF-8 "$@"; }
P="pacman --disable-sandbox --noconfirm"

# --- packages ---
say "packages"
# the kernel is the BOOT image's, never a package
sed -i 's/^#\?IgnorePkg *=.*/IgnorePkg   = linux-aarch64 linux-aarch64-headers/' "$R/etc/pacman.conf"
grep -q '^IgnorePkg' "$R/etc/pacman.conf"
ch sh -c 'pacman-key --init && pacman-key --populate archlinuxarm' >/dev/null
# before the upgrade, or every hook that touches the initramfs rebuilds it
if ch pacman -Qq linux-aarch64 >/dev/null 2>&1; then
	ch $P -Rdd linux-aarch64 >/dev/null
fi
ch $P -Syu
ch $P -S --needed $(grep -v '^#' "$SRC/image/beyondx/packages.txt")
rm -rf "$R"/usr/lib/modules/*-ARCH "$R"/boot/Image* "$R"/boot/initramfs-linux*

# --- ztMETRO and the small C tools, built in the chroot ---
say "building ztMETRO"
B=$R/usr/src/omonullos
rm -rf "$B"; mkdir -p "$B"
cp -a "$SRC/ztmetro" "$B/ztmetro"
rm -rf "$B/ztmetro/ref"
ch sh -c 'cd /usr/src/omonullos/ztmetro && make clean >/dev/null 2>&1; make -j"$(nproc)" all && make check-arch'
Z=$B/ztmetro
for b in ztmetro apps/ztapps settings/ztsettings settings/ztos-passwd auth/ztos-polkit-agent \
		sounds/ztos-sounds splash/ztos-splash devtest/zrndevtest devices/ztdevices files/ztfiles \
		sound/ztsound power/ztpower osial/ztosial camera/ztcamera store/ztstore notifyd/ztnotifyd \
		phone/ztphone messages/ztmessages contacts/ztcontacts; do
	install -Dm755 "$Z/$b" "$R/usr/local/bin/${b##*/}"
done
for b in perfd/zrnperfd sensord/ztoscsf-sensord modemd/ztoscorenetworkingd csf/ztoscsf; do
	install -Dm755 "$Z/$b" "$R/usr/local/sbin/${b##*/}"
done
A=$R/usr/local/share/applications I=$R/usr/local/share/icons/hicolor/scalable/apps
install -d "$A" "$I"
for d in devices/ztdevices files/ztfiles sound/ztsound osial/ztosial power/ztpower camera/ztcamera \
		store/data/ztstore comm/ztphone comm/ztmessages comm/ztcontacts; do
	install -m644 "$Z/$d.desktop" "$A/"
	install -m644 "$Z/$d.svg" "$I/"
done
install -Dm644 "$Z/store/data/sections" "$R/etc/ztstore/sections"
install -Dm644 "$Z/csf/components" "$R/etc/ztoscoresensorframework/components"

say "building zprops, zpoke"
cp "$SRC/modem/ril/zprops.c" "$SRC/modem/s5100/zpoke.c" "$B/"
ch sh -c 'cd /usr/src/omonullos && gcc -O2 -static -o zprops zprops.c && gcc -O2 -static -o zpoke zpoke.c'
install -m755 "$B/zprops" "$B/zpoke" "$R/usr/local/bin/"
rm -rf "$B"

# --- files ---
say "overlays"
cp -a --no-preserve=ownership "$SRC/os/overlay/." "$R/"
cp -a --no-preserve=ownership "$SRC/rootfs/overlay/." "$R/"
cp -a --no-preserve=ownership "$SRC/rootfs/overlay-beyondx/." "$R/"
grep -v '^#' "$SRC/image/beyondx/skip.txt" | while read -r p; do
	[ -n "$p" ] && rm -rf "${R:?}/${p#/}"
done
# networkd keeps to usb0; NetworkManager owns Wi-Fi (ROADMAP: as on the phone)
mv "$R/etc/systemd/network/20-wlan.network" "$R/etc/systemd/network/20-wlan.network.disabled"
# proprietary fonts are not in the public tree; ztos-fonts downloads them
chmod 755 "$R"/usr/local/sbin/* "$R"/usr/local/bin/*
chmod 440 "$R"/etc/sudoers.d/*
# deep idle stays off until it stops hanging the phone (ROADMAP 2026-11-12)
touch "$R/etc/zgov-sicd.off"

say "beyondx helpers"
K=$SRC/kernel/beyondx
for t in ztemp zbatt zcharge zthermal zchg zlowbatt zchgoff zbattlog zpdoff ztouch zgpu zmodem; do
	install -m755 "$K/$t.sh" "$R/usr/local/bin/$t"
done
install -m755 "$K/zacpm.py" "$R/usr/local/bin/zacpm"
install -m755 "$K/zgov.py" "$R/usr/local/bin/zgov"
install -m755 "$K/gnome-terminal" "$R/usr/local/bin/gnome-terminal"
install -Dm644 "$K/wdt-pet.sh" "$R/usr/local/lib/zboot/wdt-pet.sh"
# sound: zaudio.service with the VSS drop-in, as on the phone
U=$K/zfield-units
install -Dm644 "$U/zaudio.service" "$R/etc/systemd/system/zaudio.service"
install -Dm644 "$U/zaudio-vss.conf" "$R/etc/systemd/system/zaudio.service.d/zaudio-vss.conf"
install -Dm644 "$U/zaudio.env" "$R/etc/zaudio.env"
# the helpers the units and zmodem call by /root/tools/...: kept in
# /usr/local/lib/zboot/tools, /root/tools points there
T=$R/usr/local/lib/zboot/tools
install -d "$T"
install -m755 "$SRC/audio/beyondx/zaudio-load.sh" "$U/zaudio-vss.sh" "$U/zaudio-vss-check.sh" \
	"$SRC/audio/beyondx/zcall-uplink.sh" "$T/"
install -m644 "$SRC/modem/ril/zcell.py" "$SRC/modem/ril/zsms.py" "$SRC/modem/ril/zsend.py" "$T/"
install -m755 "$SRC/modem/ril/zril.py" "$T/"
rm -rf "$R/root/tools"; ln -s /usr/local/lib/zboot/tools "$R/root/tools"
# the ALSA core modules, unless the kernel job brings its own
for m in soundcore snd snd-timer snd-pcm i2c-algo-bit; do
	[ ! -s "$SRC/prebuilts/modules/$m.ko" ] ||
		install -Dm644 "$SRC/prebuilts/modules/$m.ko" "$R/usr/lib/modules/$KVER/extra/snd/$m.ko"
done
install -Dm644 "$SRC/audio/ringtone/omonullos-ring.wav" "$R/usr/local/share/sounds/ztos/ring/omonullos-ring.wav"
install -d "$R/usr/local/share/fonts/selawik"
install -m644 "$SRC"/os/fonts/selawik/*.ttf "$R/usr/local/share/fonts/selawik/"

# the ztOS SDK, as on the phone
rm -rf "$R/usr/local/lib/ztos-sdk"
cp -a --no-preserve=ownership "$SRC/sdk" "$R/usr/local/lib/ztos-sdk"
ln -sf ../lib/ztos-sdk/bin/ztos-sdk "$R/usr/local/bin/ztos-sdk"
# the system's own SDK apps (Updates): built here with the SDK, installed for
# everyone below /usr/local, so omonullos-update can update them too
for app in updates; do
	rm -rf "$WORK/app-$app"
	cp -a "$SRC/os/apps/$app" "$WORK/app-$app"
	(cd "$WORK/app-$app" && python3 "$SRC/sdk/bin/ztos-sdk" build) >/dev/null || die "cannot build app $app"
	id=$(sed -n 's/^id = "\(.*\)"/\1/p' "$WORK/app-$app/ztos.toml")
	name=$(sed -n 's/^name = "\(.*\)"/\1/p' "$WORK/app-$app/ztos.toml")
	install -Dm755 "$WORK/app-$app/dist/$id" "$R/usr/local/lib/omonullos/apps/$id"
	install -Dm644 "$WORK/app-$app/icon.svg" "$R/usr/local/share/icons/hicolor/scalable/apps/$id.svg"
	install -Dm644 /dev/stdin "$R/usr/local/share/applications/$id.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=$name
Exec=/usr/local/lib/omonullos/apps/$id
Icon=$id
Terminal=false
StartupWMClass=ztos.$id
Categories=System;
X-ZtOS-Id=$id
DESKTOP
	rm -rf "$WORK/app-$app"
done

if [ -n "$EXTRA" ]; then
	say "kernel-job files"
	cp -a --no-preserve=ownership "$EXTRA/." "$R/"
fi
if [ -e "$R/usr/local/bin/busybox-zboot" ]; then
	chmod 755 "$R/usr/local/bin/busybox-zboot"
	for a in devmem i2cget i2cset i2ctransfer i2cdetect; do
		ln -sf busybox-zboot "$R/usr/local/bin/$a"
	done
fi
[ ! -e "$R/var/lib/zmodem/data/zril/rild-lite" ] || chmod 755 "$R/var/lib/zmodem/data/zril/rild-lite"
[ ! -d "$R/usr/lib/modules/$KVER" ] || ch depmod "$KVER"

# three Commons wallpapers (the default start and lock ones); the rest is
# downloaded on the phone (Settings > theme, zcommons-walls.service)
say "wallpapers"
install -d "$R/usr/local/share/backgrounds/ztos"
cp -a --no-preserve=ownership "$SRC/os/wallpapers/default/." "$R/usr/local/share/backgrounds/ztos/"
install -Dm644 "$SRC/os/wallpapers/commons-fetch.py" "$SRC/os/wallpapers/commons-picked.json" \
	-t "$R/usr/local/lib/omonullos/wallpapers"

# --- system settings ---
say "user, locale, services"
echo beyondx > "$R/etc/hostname"
ln -sf /usr/share/zoneinfo/Asia/Tashkent "$R/etc/localtime"
sed -i -e 's/^#\(en_US.UTF-8 UTF-8\)/\1/' -e 's/^#\(uz_UZ UTF-8\)/\1/' "$R/etc/locale.gen"
ch locale-gen >/dev/null
# the tarball's user alarm (password alarm) is the phone's; root is locked
ch usermod -aG wheel,input,video alarm
ch passwd -l root >/dev/null
IFS=: read -r _ _ uid gid _ home _ < <(grep '^alarm:' "$R/etc/passwd")
# alarm comes from the tarball, so useradd never copied /etc/skel into its
# home: without ~/.config/ztmetro every choice in Settings (wallpaper, nav,
# language, ringtone) was dropped (2026-11-13 test of this image)
cp -a --no-preserve=ownership "$R/etc/skel/." "$R$home/"
chown -R "$uid:$gid" "$R$home"
[ -d "$R$home/.config/ztmetro" ] || die "no ~/.config/ztmetro in $home"
install -Dm644 /dev/stdin "$R/etc/ztos/user.conf" <<USERCONF
ZTOS_USER=alarm
ZTOS_UID=$uid
ZTOS_GID=$gid
ZTOS_HOME=$home
USERCONF
ch /usr/local/sbin/ztos-user-dropins >/dev/null
ch update-desktop-database /usr/local/share/applications >/dev/null 2>&1 || true
ch fc-cache -f >/dev/null 2>&1 || true

for u in greetd NetworkManager systemd-networkd systemd-resolved systemd-timesyncd; do
	ch systemctl enable "$u" >/dev/null 2>&1 || die "cannot enable $u"
done
ch systemctl disable sshd >/dev/null 2>&1 || true
# as on the phone: nothing at boot waits for the network
ch systemctl disable NetworkManager-wait-online >/dev/null 2>&1 || true
ch systemctl --global enable pipewire.socket pipewire-pulse.socket wireplumber.service >/dev/null 2>&1
ch systemctl set-default graphical.target >/dev/null
# the phone's own units, where the phone has them (not by their [Install])
S=$R/etc/systemd/system
want() {
	local t=$1; shift
	mkdir -p "$S/$t.wants"
	for u; do ln -sf "/etc/systemd/system/$u.service" "$S/$t.wants/$u.service"; done
}
want multi-user.target zboot-firstboot zboot-usb-gadget zbattlog zcpu zgpu zlowbatt zmodem zpdoff ztouch zwlan zaudio
want sysinit.target zvendor-firmware zboot-guards zclock zlastlog
want graphical.target zrn-cpuidle
want poweroff.target zchgoff
# omonullOS updates (omonullos-update): a look once a day, and the keys that
# may sign them (public halves only; the private key is with the owner)
mkdir -p "$S/timers.target.wants"
ln -sf /etc/systemd/system/omonullos-update-check.timer "$S/timers.target.wants/omonullos-update-check.timer"
if ls "$SRC"/image/beyondx/ota-keys/*.pem >/dev/null 2>&1; then
	install -Dm644 -t "$R/usr/local/share/omonullos/ota-keys" "$SRC"/image/beyondx/ota-keys/*.pem
else
	echo "warning: no image/beyondx/ota-keys/*.pem: this image cannot take updates" >&2
fi
mkdir -p "$S/getty.target.wants"
ln -sf /usr/lib/systemd/system/serial-getty@.service "$S/getty.target.wants/serial-getty@ttyGS0.service"
# no suspend through systemd (the phone's own sleep work is not a service yet)
for t in sleep suspend hibernate hybrid-sleep suspend-then-hibernate; do
	ln -sf /dev/null "$S/$t.target"
done
# no shutdown initramfs (build-arch-root.sh, 2026-10-18)
ln -sf /dev/null "$S/mkinitcpio-generate-shutdown-ramfs.service"
for u in $(ls "$S"/*.wants/ | grep '\.service$' | sort -u); do
	[ -e "$S/$u" ] || [ -e "$R/usr/lib/systemd/system/$u" ] || [ "${u%@*}" != "$u" ] ||
		die "enabled but missing: $u"
done

# --- tidy and pack ---
say "tidying"
touch "$R/etc/zboot-firstboot"
rm -f "$R/etc/resolv.conf"; ln -sf /run/systemd/resolve/stub-resolv.conf "$R/etc/resolv.conf"
: > "$R/etc/machine-id"
rm -rf "$R/var/log/pacman.log" "$R"/root/.bash_history "$R"/etc/ssh/ssh_host_*
{
	echo "OmonullOS beyondx root"
	echo "built $(date -u +%Y-%m-%dT%H:%MZ)"
	echo "commit $(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)"
	[ -z "$missing" ] || echo "MISSING:$missing"
} > "$R/etc/omonullos-build"
# the update made from this very root (OTA_VERSION/OTA_TAG/OTA_NOTES from the
# CI on a tag; OTA_BASE = the previous release's manifest, for a delta): the
# image then knows its version and which /etc files are still ours
if [ -n "${OTA_VERSION:-}" ]; then
	say "update package $OTA_VERSION"
	rm -rf "$(dirname "$OUT")/ota"
	python3 "$SRC/scripts/ota-make-package.py" --root "$R" --version "$OTA_VERSION" \
		--tag "${OTA_TAG:-beyondx-v$OTA_VERSION}" --notes "${OTA_NOTES:?OTA_NOTES}" \
		--commit "$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)" \
		--out "$(dirname "$OUT")/ota" ${OTA_BASE:+--base "$OTA_BASE"}
	install -Dm644 "$(dirname "$OUT")/ota/installed.json" "$R/var/lib/omonullos-update/installed.json"
fi
cleanup
trap - EXIT

say "image"
rm -f "$OUT"
mke2fs -q -t ext4 -L ZR_ARCH -m 1 -O ^orphan_file -d "$R" "$OUT" "$SIZE"
e2fsck -fn "$OUT" | tail -n 2
ls -ls "$OUT"
echo "BEYONDX_IMAGE_OK $OUT"
