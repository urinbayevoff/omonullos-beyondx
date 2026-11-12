#!/bin/sh
# Galaxy S10 5G: bring up the LTE modem (S5000AP) under Arch, as stock init
# did (vendor init.baseband.rc): load zrn-modem.ko (modem/beyondx), give the
# devices their stock owners, and run Samsung's own CP boot daemon (cbd) from
# the phone's VENDOR and SYSTEM partitions in a small Android root.
#
#   zmodem start   everything, in order, as transient services: zprops,
#                  rild-lite, then the 5G and the LTE modem's cbd, zsms and
#                  zcell (the status bar's signal and operator)
#   zmodem up      load, then run cbd in the foreground (as a service)
#   zmodem up5g    the same for the 5G modem (S5100): stock cpboot-daemon5g
#   zmodem load    only load the modules (pcie-exynos9820-ch1.ko first: the
#                  S5100's PCIe host, which zrn-modem.ko uses)
#   zmodem root    only set up the Android root
#   zmodem props   run zprops (system properties, property service, liblog
#                  sink) in the foreground, for the RIL and cbd
#   zmodem ril     run rild-lite (Samsung's libsec-ril.so) in the foreground;
#                  start it before the cbd's: the CP's first IPC message
#                  (power on) is sent once, when the RIL opens umts_ipc0
#   zmodem check   if the phone did not register (5G modem down, RIL not
#                  running), say so on screen: a notification to restart
#   zmodem down    stop cbd, take the Android root down (the module stays:
#                  never rmmod, reboot instead)
#
# EFS and CPEFS are never written: cbd and the modem get copies of them,
# made once from the read-only partitions into /var/lib/zmodem (a fresh copy:
# remove the file). The CP firmware is read from RADIO, read only.
# Added for OmonullOS, October 2026.

R=/run/android
V=/var/lib/zmodem
MOD=${ZMODEM_KO:-/usr/local/lib/zboot/zrn-modem.ko}
PCIE1=${ZMODEM_PCIE1_KO:-/usr/local/lib/zboot/pcie-exynos9820-ch1.ko}
MARK=$V/loading
part() { readlink -f /dev/disk/by-partlabel/$1; }

load() {
	mkdir -p $V
	[ -e /dev/umts_boot0 ] && return 0
	if [ -e $MARK ]; then
		echo "zmodem: the last load did not finish; not loading. Remove $MARK to retry." >&2
		return 1
	fi
	touch $MARK; sync
	[ -e /sys/module/pcie_exynos9820_ch1 ] || insmod $PCIE1 ||
		{ echo "zmodem: the PCIe channel 1 module did not load" >&2; rm -f $MARK; return 1; }
	insmod $MOD $ZMODEM_ARGS || { echo "zmodem: the module did not load" >&2; rm -f $MARK; return 1; }
	sleep 2
	rm -f $MARK; sync
	# /dev/drb for the RIL; a plain file left there by an older start goes
	[ -c /dev/drb ] || { rm -f /dev/drb; insmod ${MOD%/*}/zrn_drb.ko; }
	# stock ueventd.rc: radio (1001) owns the modem's devices
	chown 1000:1001 /dev/umts_* /dev/nr_* /dev/multipdp* 2>/dev/null
	chown 1001:1001 /dev/umts_dm0 /dev/umts_router /dev/nr_dm0 /dev/nr_router 2>/dev/null
	chown 1001:1001 /dev/modem_boot_spi /dev/drb 2>/dev/null
	chmod 0660 /dev/umts_* /dev/nr_* /dev/multipdp* /dev/modem_boot_spi 2>/dev/null
}

copy() {	# copy <partition> <file>: once, from the read-only partition
	[ -s $V/$2 ] && return 0
	dd if="$(part $1)" of=$V/$2.tmp bs=1M status=none && mv $V/$2.tmp $V/$2
}

root() {
	mountpoint -q $R/data && [ -d $R/system/bin ] && return 0
	# a half-unmounted root (umount -R hit a busy mount) gets rebuilt
	mountpoint -q $R && umount -Rl $R
	mkdir -p /mnt/system /mnt/vendor /mnt/product $V/data
	mountpoint -q /mnt/system || mount -o ro "$(part system)" /mnt/system || return 1
	mountpoint -q /mnt/vendor || mount -o ro "$(part vendor)" /mnt/vendor || return 1
	# OMC/CSC (cscfeature: voice capable, network config) is on PRODUCT
	mountpoint -q /mnt/product || mount -o ro "$(part product)" /mnt/product || return 1
	mkdir -p $R && mount -t tmpfs -o mode=755,size=1m tmpfs $R
	mkdir -p $R/system $R/vendor $R/product $R/apex/com.android.runtime $R/dev $R/proc $R/sys \
		$R/data $R/tmp $R/mnt/vendor/efs $R/mnt/vendor/cpefs
	mount --bind /mnt/system/system $R/system
	mount --bind /mnt/vendor $R/vendor
	mount --bind /mnt/product $R/product
	mount --bind /mnt/system/system/apex/com.android.runtime $R/apex/com.android.runtime
	mkdir -p $R/apex/com.android.i18n	# libandroidicu.so for the RIL's libsqlite
	mount --bind /mnt/system/system/apex/com.android.i18n $R/apex/com.android.i18n
	mount --rbind /dev $R/dev
	mount -t proc proc $R/proc
	mount --rbind /sys $R/sys
	# slaves: unmounting the chroot must not take the host's /dev/pts,
	# /sys/fs/cgroup... with it (umount -R did, 2026-10-18)
	mount --make-rslave $R/dev
	mount --make-rslave $R/sys
	# stock init.vendor.rilcommon.rc: the RIL keeps its files (build
	# fingerprint, CSC version, databases) here; without them it resets the CP
	mkdir -p $V/data/vendor/secradio $V/data/vendor/log/err $V/data/log
	chown 1001:1001 $V/data/vendor/secradio; chmod 0770 $V/data/vendor/secradio
	mount --bind $V/data $R/data
	ln -s system/bin $R/bin; ln -s system/etc $R/etc
	# as Android, /efs is SEC_EFS (the RIL reads FactoryApp/, imei/omcnw_code.dat
	# there); it was a link to EFS until 2026-10-18
	mkdir -p $R/efs
	# EFS, SEC_EFS and CPEFS: copies (see above)
	copy efs efs.img && copy sec_efs sec_efs.img && copy cpefs cpefs.img || return 1
	mount -o loop,nosuid,nodev,noatime $V/sec_efs.img $R/efs || return 1
	mount -o loop,nosuid,nodev,noatime $V/efs.img $R/mnt/vendor/efs || return 1
	mount -o loop,nosuid,nodev,noatime $V/cpefs.img $R/mnt/vendor/cpefs || return 1
	# cbd -P platform/13d60000.ufs/by-name/radio, as stock; /dev is shared
	mkdir -p /dev/block/platform/13d60000.ufs/by-name
	ln -sf "$(part radio)" /dev/block/platform/13d60000.ufs/by-name/radio
	ln -sf "$(part radio2)" /dev/block/platform/13d60000.ufs/by-name/radio2
	ln -sf "$(part radio)" /dev/mbin0
	ln -sf "$(part radio2)" /dev/mbin1	# the RIL reads the 5G NV offsets
	chgrp 1001 "$(part radio)" "$(part radio2)"; chmod 0640 "$(part radio)" "$(part radio2)"
}

case "${1:-up}" in
load)	load ;;
root)	root ;;
up)
	load && root || exit 1
	pgrep -f -- "cbd -d -tss310" >/dev/null && exit 0
	exec chroot --groups=1001,2001,3003,9998,1005,1015,1028,2000,1000,1007 $R \
		/vendor/bin/cbd -d -tss310 -bm -mm -P platform/13d60000.ufs/by-name/radio
	;;
up5g)
	load && root || exit 1
	pgrep -f -- "cbd -d -ts5100" >/dev/null && exit 0
	exec chroot --groups=1001,2001,3003,9998,1005,1015,1028,2000,1000,1007 $R \
		/vendor/bin/cbd -d -ts5100 -bs -me -P platform/13d60000.ufs/by-name/radio2
	;;
start)
	load || exit 1
	Z=$(readlink -f "$0")
	for u in zprops:props:2 zril:ril:3 zmodem5g:up5g:8 zmodem-cbd:up:0; do
		IFS=: read unit cmd wait <<-EOF
		$u
		EOF
		# when rild-lite ends (a crash too), the modems go down with it:
		# left running without their RIL they kept the phone hot (2026-10-17)
		if systemctl -q is-active $unit; then
			:
		elif [ $unit = zril ]; then
			systemd-run -q --unit=$unit --collect -p "ExecStopPost=$Z down" \
				${ZRIL_WRAP:+-E "ZRIL_WRAP=$ZRIL_WRAP"} $Z $cmd
		else
			systemd-run -q --unit=$unit --collect $Z $cmd
		fi
		sleep $wait
	done
	# incoming SMS and network time (modem/ril/zsms.py), only while the RIL runs
	systemctl -q is-active zsms ||
		systemd-run -q --unit=zsms --collect -p BindsTo=zril.service -p After=zril.service \
			python3 /root/tools/zsms.py
	# the status bar's signal and operator (modem/ril/zcell.py)
	systemctl -q is-active zcell ||
		systemd-run -q --unit=zcell --collect -p BindsTo=zril.service -p After=zril.service \
			python3 /root/tools/zcell.py
	;;
check)
	grep -qE '^registered=(home|roam)' /run/ztoscorenetworking/status 2>/dev/null && exit 0
	why="RIL işlamayapti"
	systemctl -q is-active zril && why="tarmoqda röyxatdan ötmadi"
	systemctl -q is-active zmodem5g || why="5G modem kötarilmadi"
	echo "zmodem: not registered: $why" >&2
	runuser -u alarm -- env DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
		gdbus call --session --dest org.freedesktop.Notifications \
		--object-path /org/freedesktop/Notifications \
		--method org.freedesktop.Notifications.Notify "Modem" 0 "" \
		"Modem işga tuşmadi" "$why. Telefonni öçirib, qayta yoqing." "[]" "{}" 0 >/dev/null
	exit 1
	;;
props)
	root || exit 1
	S=/mnt/system/system
	exec /usr/local/bin/zprops -p $V/persist.prop $S/build.prop \
		$S/system_ext/etc/build.prop /mnt/vendor/build.prop \
		/mnt/vendor/odm/etc/build.prop $V/boot.prop
	;;
ril)
	load && root || exit 1
	[ -e /dev/__properties__ ] || { echo "zmodem: start zprops first (zmodem props)" >&2; exit 1; }
	export LD_LIBRARY_PATH=/apex/com.android.i18n/lib64:/apex/com.android.runtime/lib64
	# stock's network_config service first: ro.vendor.radio.default_network
	# and voice_capable from telephony.prop and the CSC, read by the RIL
	chroot --userspec=1001:1001 $R /vendor/bin/secril_config_svc NetworkConfig
	# ZRIL_WRAP="strace -f -o /tmp/ril.st": a tracer around it
	exec $ZRIL_WRAP chroot --groups=1001,2001,3003,9998,1005,1015,1028,2000,1000,1007 $R \
		/data/zril/rild-lite
	;;
down)
	systemctl stop zmodem-cbd zmodem5g zprops 2>/dev/null
	pkill -x cbd
	umount -Rl $R 2>/dev/null
	umount /mnt/vendor /mnt/system /mnt/product 2>/dev/null
	;;
*)	echo "usage: zmodem start|check|up|up5g|props|ril|load|root|down" >&2; exit 2 ;;
esac
