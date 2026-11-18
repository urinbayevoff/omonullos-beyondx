#!/bin/sh
# zufs: bring the Galaxy S10 5G's internal UFS up by hand, read-only.
#
#   zufs [module parameters]   check BLK_FSYS1, load ufs-exynos9820.ko, make
#                              every disk read-only, list the partitions
#   zufs root                  as above, then only the partition labelled
#                              ZR_ARCH writable, and only if it is userdata
#                              or omonull
#                              (initramfs runs this before it looks for root)
#   zufs list                  the partitions again (name, device, size)
#   zufs check <name> <file>   sha256 of the first <file size> bytes of the
#                              partition named <name> against <file>
#
# Added for OmonullOS, October 2026. Untested. scripts/build-boot.sh --ufs
# puts it in initramfs as /bin/zufs together with the module.
# Modified for OmonullOS, November 2026: the root may also sit on a
# partition named omonull (dual boot, docs/s10-5g/DUAL-BOOT-STEP3.md).
#
# The host sits in BLK_FSYS1: no power domain of its own, but its clocks
# come through two CMU_TOP gates and four CMU_FSYS1 gates, and a read from a
# gated block hangs the bus with no message (CLAUDE.md rule 6). So, as
# pre-usb.sh does for FSYS0A, every read is written to /dev/pmsg0 first
# (TWRP shows it as /sys/fs/pstore/pmsg-ramoops-0 after a watchdog reset)
# and nothing is loaded unless every gate on the way reads open.
# Addresses: stock cmucal-sfr.c (CMU_CMU 0x1a240000, CMU_FSYS1 0x13c00000),
# ufs-exynos.h (PMU UFS_PHY_CONTROL 0x724) and the stock tree's ufs node.
#
# EFS lives on this disk (CLAUDE.md rule 1): every sd device is set
# read-only the moment it shows up, and this script never writes to one.

MOD=/lib/modules/zboot/ufs-exynos9820.ko
HOST=/sys/bus/platform/devices/13d60000.ufs

pm() {
	echo "$*" > /dev/pmsg0 2>/dev/null
}

rd() {	# rd <name> <address>: read 32 bits, logged before and after
	pm "rd $1 $2"
	v=$(devmem "$2" 32 2>/dev/null)
	echo "zufs: $1 [$2] = ${v:-?}"
	pm "  = ${v:-?}"
}

# A CLK_CON_GAT register is off only when MANUAL (bit 20) is set and
# CG_VALUE (bit 21) is not; otherwise the hardware runs it.
gate_on() {
	[ -n "$1" ] && [ $(( ($1 >> 20) & 3 )) -ne 1 ]
}

stop() {
	echo "zufs: $*; not loading the UFS driver"
	pm "stop: $*"
	exit 1
}

setro() {	# every sd disk and partition there is, read-only
	# Each one gets its own flag, always: a partition of a read-only disk
	# reads ro=1 without one, and would turn writable with the disk (bx21).
	for b in /sys/class/block/sd*; do
		[ -e "$b" ] || continue
		d=/dev/${b##*/}
		[ -b "$d" ] || continue
		blockdev --setro "$d"
	done
}

rootrw() {	# the ZR_ARCH partition writable, if it is userdata or omonull; the rest stays ro
	d=$(findfs LABEL=ZR_ARCH 2>/dev/null)
	[ -n "$d" ] || { echo "zufs: no ZR_ARCH"; return 1; }
	n=$(sed -n 's/^PARTNAME=//p' "/sys/class/block/${d#/dev/}/uevent")
	# omonull: its name once the disk is shared with Android, whose data
	# partition has to be called userdata (docs/s10-5g/DUAL-BOOT-STEP3.md)
	case "$n" in
	userdata|omonull) ;;
	*) echo "zufs: ZR_ARCH is $d ($n), not userdata or omonull: left read-only"; return 1 ;;
	esac
	# the disk's flag only lifts what the partitions' own flags allow
	blockdev --setrw "/dev/$(basename "$(readlink -f "/sys/class/block/${d#/dev/}/..")")"
	blockdev --setrw "$d"
	for b in /sys/class/block/sd*; do
		[ "/dev/${b##*/}" = "$d" ] && continue
		[ "$(cat "$b/partition" 2>/dev/null)" ] || continue
		[ "$(cat "$b/ro")" = 1 ] || { echo "zufs: ${b##*/} writable, all back to ro"; setro; return 1; }
	done
	echo "zufs: $d ($n) writable, every other partition read-only"
	pm "zufs root $d"
}

list() {
	for b in /sys/class/block/sd*; do
		[ -e "$b" ] || continue
		n=$(sed -n 's/^PARTNAME=//p' "$b/uevent")
		echo "${b##*/} ${n:--} $(( $(cat "$b/size") / 2048 )) MB ro=$(cat "$b/ro")"
	done
}

byname() {	# byname <partition name>: its /dev node
	for b in /sys/class/block/sd*; do
		[ -e "$b" ] || continue
		grep -qx "PARTNAME=$1" "$b/uevent" && { echo "/dev/${b##*/}"; return 0; }
	done
	return 1
}

ROOTRW=
[ "$1" = root ] && { ROOTRW=1; shift; }
case "$1" in
list)
	list
	exit 0
	;;
check)
	[ $# -eq 3 ] || { echo "zufs check <partition name> <file>"; exit 2; }
	d=$(byname "$2") || { echo "zufs: no partition named $2"; exit 1; }
	[ "$(cat /sys/class/block/${d#/dev/}/ro)" = 1 ] ||
		{ echo "zufs: $d is not read-only, not touching it"; exit 1; }
	n=$(wc -c < "$3")
	a=$(head -c "$n" "$d" | sha256sum | cut -d' ' -f1)
	b=$(sha256sum "$3" | cut -d' ' -f1)
	echo "zufs: $2 ($d) first $n bytes $a"
	echo "zufs: $3 $b"
	[ "$a" = "$b" ] && { echo "zufs: match"; exit 0; }
	echo "zufs: DIFFERENT"
	exit 1
	;;
esac

if [ -e "$HOST/driver" ]; then
	echo "zufs: already loaded"
	setro
	[ -n "$ROOTRW" ] && rootrw
	list
	exit 0
fi
pm "zufs $(cut -d' ' -f1 /proc/uptime)"

rd cmu_top.gate.fsys1_bus 0x1a242000
g1=$v
rd cmu_top.gate.fsys1_ufs_embd 0x1a242080
g2=$v
rd cmu_top.mux.fsys1_ufs_embd 0x1a241078
mux=$v
rd cmu_top.div.fsys1_ufs_embd 0x1a241864
div=$v
gate_on "$g1" && gate_on "$g2" || stop "a top gate into FSYS1 is off"
# What S-Boot left on bx20: SELECT 3 (PLL_SHARED4 / 2), DIVRATIO 1 (/ 2):
# 166.5625 MHz, the rate the device tree's fixed clock says (the driver
# checks it again against the host's 1 us tick)
[ $((mux & 3)) -eq 3 ] && [ $((div & 7)) -eq 1 ] ||
	echo "zufs: UFS_EMBD is not at S-Boot's usual 166.5625 MHz (mux $mux div $div)"

rd cmu_fsys1.mux.bus_user 0x13c00100
rd cmu_fsys1.mux.ufs_embd_user 0x13c00180
rd cmu_fsys1.gate.sysreg_pclk 0x13c02084
g3=$v
rd cmu_fsys1.gate.ufs_aclk 0x13c02094
g4=$v
rd cmu_fsys1.gate.ufs_unipro 0x13c02098
g5=$v
rd cmu_fsys1.gate.ufs_fmp 0x13c0209c
g6=$v
gate_on "$g3" && gate_on "$g4" && gate_on "$g5" && gate_on "$g6" ||
	stop "a CMU_FSYS1 gate of the UFS host is off"

rd pmu.ufs_phy_ctrl 0x15860724
rd sysreg_fsys1.io_coherency 0x13c10700
rd ufs.hci.version 0x13d60008
rd ufs.hci.hcs 0x13d60030
rd ufs.vs.1us_to_cnt 0x13d6110c

pm "insmod ufs $*"
insmod "$MOD" "$@" || stop "insmod failed"
pm "insmod returned $(cut -d' ' -f1 /proc/uptime)"
# Runtime PM keeps the link active (level 0) until hibernate has been tried
# on purpose: one new thing at a time.
[ -w "$HOST/rpm_lvl" ] && echo 0 > "$HOST/rpm_lvl"

# ufshcd scans asynchronously; set every disk read-only as it appears
i=0
while [ $i -lt 40 ]; do
	setro
	i=$((i + 1))
	sleep 0.25
done
setro
pm "zufs done $(cut -d' ' -f1 /proc/uptime)"
if [ -n "$ROOTRW" ]; then
	rootrw
	exit 0
fi
dmesg | grep -i -E 'ufs|scsi|sd[a-f]' | tail -40
list
