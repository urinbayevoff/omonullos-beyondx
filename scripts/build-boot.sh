#!/bin/bash
# Build the zboot boot image for the Galaxy S9 (starlte): mainline 7.1 kernel
# with the Exynos 9810 drivers of the S9+ port (third_party/s9plus), our
# initramfs built in, wrapped in uniLoader, packed as a Samsung boot image.
#
#   scripts/build-boot.sh <name> [options]
#     --fallback boot|recovery   where a watchdog reset / panic lands (default
#                                boot: TWRP on BOOT, this image on RECOVERY;
#                                for beyondx recovery: this image on BOOT)
#     --no-ufs                   leave the UFS host disabled
#     --power                    MAX77705 charger + fuel gauge
#     --acpm                     ACPM mailbox to the S2MPS18 PMIC (rails)
#     --cpufreq                  CPU frequency scaling, vendor PLL tables (implies --acpm)
#     --touch                    S6SY761 touch controller (implies --acpm: its
#                                rails have to be power-cycled to answer);
#                                beyondx, with --muic: the I2C bus to its
#                                S6SY771 (exynos9820-beyondx-touch.dtsi), set
#                                up by pre-usb.sh, used from user space
#     --gpu                      Mali-G72 through Panfrost (implies --acpm)
#     --decon                    DECON scanout instead of the bootloader framebuffer
#     --wlan                     BCM4361 over the PCIe root complex, with this
#                                phone's own firmware from firmware/
#     --pm-debug                 suspend stage tests and device PM diagnostics
#     --lockup                   soft/hard lockup detectors that panic (backtrace in ramoops)
#     --cmdline "..."            extra kernel command line (zboot.rescue=1 ...)
#     --device starlte|beyondx   which phone (default starlte, the S9)
#     --usb                      beyondx: the USB device port (DWC3 + PHY as
#                                modules, loaded by initramfs once it has
#                                checked FSYS0A is powered; see
#                                kernel/beyondx/pre-usb.sh)
#     --muic                     beyondx, with --usb: the I2C bus to the
#                                MAX77705 (i2c-exynos5 as a module, used from
#                                user space by pre-usb.sh); zboot.muic=usb on
#                                the command line lets it close the MUIC
#                                switch onto the USB PHY
#     --pet                      beyondx: initramfs keeps uniLoader's 30 s
#                                watchdog fed (kernel/beyondx/wdt-pet.sh) and
#                                there is no zboot.dump panic: the rescue shell
#                                stays up until /run/zboot.nopet or a reboot
#     --mem                      beyondx: all of the RAM S-Boot reports, not
#                                just the 1177 MB below 0xe0000000
#                                (kernel/beyondx/exynos9820-beyondx-mem.dtsi)
#     --smp                      beyondx: all eight CPUs at boot, not
#                                maxcpus=1 (the A75s and M4s cannot be
#                                brought up later from the A55's
#                                capabilities: Spectre-v2 conflict)
#     --ufs                      beyondx, with --usb: the internal UFS host
#                                (kernel/beyondx/ufs, an external module) and
#                                /bin/zufs, which loads it by hand once it
#                                has read BLK_FSYS1's gates open; every disk
#                                read-only
#     --gpu (beyondx)            Mali-G76 for Panfrost, a module loaded by hand
#                                (exynos9820-beyondx-gpu.dtsi)
#     --keys                     beyondx: the side keys (gpio-keys on the ALIVE
#                                pin controller, exynos9820-beyondx-keys.dtsi)
#     --decon (beyondx)          DECON_F scanout, a module loaded by hand
#                                (kernel/beyondx/decon, exynos9820-beyondx-decon.dtsi)
#     --wlan (beyondx)           BCM4375 Wi-Fi: PCI in the kernel; the PCIe host
#                                (kernel/beyondx/pcie), cfg80211 and brcmfmac
#                                (patch-brcmfmac-4375.py) as modules loaded by
#                                hand, firmware from firmware/beyondx
#     --menu                     beyondx: uniLoader's boot menu, OmonullOS / TWRP
#     --splash                   beyondx: the boot screen from the initramfs on
#                                (kernel/beyondx/splash.sh); fbcon takes the
#                                screen only when there is text, and the
#                                bootloader's empty ramdisk is ignored
#                                (kernel/beyondx/bootmenu-bx.c); to add it to
#                                an image on the phone without rebuilding its
#                                kernel: scripts/rewrap-boot-bx.sh
#     --root                     beyondx, with --ufs and --pet: no
#                                zboot.rescue=1; initramfs runs zufs root
#                                (kernel/beyondx/pre-root.sh) and boots the
#                                Arch root labelled ZR_ARCH on userdata
#
# Modified for OmonullOS, November 2026: without stock/ (CI), the beyondx
# reference header comes from kernel/beyondx/make-ref-header.py.
# Modified for OmonullOS, October 2026: --device beyondx, the Galaxy S10 5G
# (SM-G977N) at stage 3: mainline 7.1 with the device tree in kernel/beyondx
# (CPUs, GIC, timer, the bootloader's framebuffer, nothing else), upstream
# uniLoader (third_party/uniLoader) with board-beyondx, and a header v1 boot
# image (kernel/mkboot-v1.py) made against the stock boot.img. None of the
# S9's block switches apply to it yet. Its kernel builds in $SRC/out-beyondx.
# Stage 4 adds its own switches, the first being --usb; pstore's pmsg is on
# for it, so initramfs can leave a trail that survives a hang.
#
# Output: out/<name>.img (+ .config, .dts, .sha256). Runs unprivileged; the
# toolchain, kernel tree and busybox live under build/.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
B=$ROOT/build
SRC=$B/linux-7.1
PORT=$ROOT/third_party/s9plus
TC=$B/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin
REF=$ROOT/stock/RECOVERY.img    # TWRP for starlte: a header and DTBH sboot accepts
DEVICE=starlte
export PATH=$TC:$PATH LC_ALL=C ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-

NAME=${1:?image name}
shift
FALLBACK=		# default per device, below
DTS_FLAGS=()
EXTRA=
UFS=1 ACPM= CPUFREQ= GPU= DECON= WLAN= WLANOPT= BXWLAN= PMDEBUG= USB= MUIC= PET= MEM= SMP= BXUFS= BXROOT= BXMENU= SPLASH= TOUCH= KEYS= GPUOPT= BXGPU= DECONOPT= BXDECON= LOCKUP=
while [ $# -gt 0 ]; do
	case "$1" in
	--fallback) FALLBACK=$2; shift ;;
	--no-ufs) UFS= ;;
	--power) DTS_FLAGS+=(--power) ;;
	--touch) TOUCH=1 ;;
	--gpu) GPUOPT=1 ;;
	--decon) DECONOPT=1 ;;
	--wlan) WLANOPT=1 ;;
	--pm-debug) PMDEBUG=1 ;;
	--lockup) LOCKUP=1 ;;
	--acpm) ACPM=1 ;;
	--cpufreq) CPUFREQ=1; ACPM=1 ;;
	--cmdline) EXTRA=$2; shift ;;
	--device) DEVICE=$2; shift ;;
	--usb) USB=1 ;;
	--muic) MUIC=1 ;;
	--pet) PET=1 ;;
	--mem) MEM=1 ;;
	--smp) SMP=1 ;;
	--ufs) BXUFS=1 ;;
	--root) BXROOT=1 ;;
	--keys) KEYS=1 ;;
	--menu) BXMENU=--menu ;;
	--splash) SPLASH=1 ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
case "$DEVICE" in
starlte)
	FALLBACK=${FALLBACK:-boot}
	[ -z "$TOUCH" ] || { DTS_FLAGS+=(--touch); ACPM=1; }
	[ -z "$GPUOPT" ] || { DTS_FLAGS+=(--gpu); GPU=1; ACPM=1; }
	[ -z "$DECONOPT" ] || { DTS_FLAGS+=(--decon); DECON=1; }
	[ -z "$WLANOPT" ] || { DTS_FLAGS+=(--wlan); WLAN=1; }
	[ -z "$USB$MUIC$PET$MEM$SMP$BXUFS$BXROOT$KEYS" ] || { echo "--usb, --muic, --pet, --mem, --smp, --ufs, --root and --keys are for beyondx (the S9 has UFS on, --no-ufs)" >&2; exit 2; }
	;;
beyondx)
	# this image goes to BOOT, TWRP or Magisk to RECOVERY: come back there
	FALLBACK=${FALLBACK:-recovery}
	if [ ${#DTS_FLAGS[@]} -gt 0 ] || [ -n "$ACPM$CPUFREQ$GPU$DECON$WLAN$PMDEBUG" ]; then
		echo "--device beyondx: the S9's block switches do not apply yet" >&2
		exit 2
	fi
	UFS=
	[ -z "$MUIC" ] || [ -n "$USB" ] || { echo "--muic needs --usb (pre-usb.sh drives it)" >&2; exit 2; }
	BXGPU=$GPUOPT
	BXDECON=$DECONOPT
	BXWLAN=$WLANOPT
	[ -z "$TOUCH" ] || [ -n "$MUIC" ] || { echo "--touch needs --muic (pre-usb.sh loads the bus driver)" >&2; exit 2; }
	[ -z "$BXUFS" ] || [ -n "$USB" ] || { echo "--ufs needs --usb (zufs is run from the telnet shell)" >&2; exit 2; }
	if [ -n "$BXROOT" ] && { [ -z "$BXUFS" ] || [ -z "$PET" ]; }; then
		echo "--root needs --ufs and --pet (the root system takes the watchdog over)" >&2
		exit 2
	fi
	REF=$ROOT/stock/firmware/HWD3/img/boot.img	# stock G977NKSU6HWD3, header v1
	# Without the stock firmware (CI, phone): the same header fields, written
	# by make-ref-header.py (mkboot-v1 only reads the header of its --ref)
	if [ ! -f "$REF" ]; then
		mkdir -p "$B"
		REF=$B/boot-ref-header.img
		python3 -I "$ROOT/kernel/beyondx/make-ref-header.py" "$REF"
	fi
	[ -f "$REF" ] || { echo "missing $REF (stock firmware, see docs/s10-5g/log.md)" >&2; exit 1; }
	;;
*) echo "--device starlte|beyondx" >&2; exit 2 ;;
esac
[ -n "$UFS" ] && DTS_FLAGS+=(--ufs)
case "$FALLBACK" in
boot) INFORM3=0x12345670 ;;
recovery) INFORM3=0x12345674 ;;
*) echo "--fallback boot|recovery" >&2; exit 2 ;;
esac
mkdir -p "$ROOT/out"
OUT=$ROOT/out/$NAME

echo "=== device tree ==="
if [ "$DEVICE" = beyondx ]; then
	DTS=arch/arm64/boot/dts/exynos/exynos9820-beyondx.dts
	cp "$ROOT/kernel/beyondx/exynos9820.dtsi" "$ROOT/kernel/beyondx/exynos9820-beyondx.dts" \
		"$ROOT/kernel/beyondx/exynos9820-beyondx-usb.dtsi" \
		"$ROOT/kernel/beyondx/exynos9820-beyondx-muic.dtsi" \
		"$ROOT/kernel/beyondx/exynos9820-beyondx-mem.dtsi" \
		"$ROOT/kernel/beyondx/exynos9820-beyondx-ufs.dtsi" \
		"$ROOT/kernel/beyondx/exynos9820-beyondx-touch.dtsi" \
		"$ROOT/kernel/beyondx/exynos9820-beyondx-keys.dtsi" \
		"$ROOT/kernel/beyondx/exynos9820-beyondx-gpu.dtsi" \
		"$ROOT/kernel/beyondx/exynos9820-beyondx-decon.dtsi" \
		"$ROOT/kernel/beyondx/exynos9820-beyondx-pcie.dtsi" \
		"$SRC/arch/arm64/boot/dts/exynos/"
	if [ -n "$USB" ]; then
		printf '\n#include "exynos9820-beyondx-usb.dtsi"\n' >> "$SRC/$DTS"
	fi
	if [ -n "$MUIC" ]; then
		printf '#include "exynos9820-beyondx-muic.dtsi"\n' >> "$SRC/$DTS"
	fi
	if [ -n "$TOUCH" ]; then
		printf '#include "exynos9820-beyondx-touch.dtsi"\n' >> "$SRC/$DTS"
		# the S6SY771's event count is 5 bits (mainline s6sy761 reads 6)
		python3 "$ROOT/kernel/patch-touch-events.py" "$SRC"
	fi
	if [ -n "$BXGPU" ]; then
		printf '#include "exynos9820-beyondx-gpu.dtsi"\n' >> "$SRC/$DTS"
		# the G76 r0p0 faults with Panfrost's legacy page tables
		python3 "$ROOT/kernel/patch-panfrost-zb.py" "$SRC"
	fi
	if [ -n "$KEYS" ]; then
		printf '#include "exynos9820-beyondx-keys.dtsi"\n' >> "$SRC/$DTS"
	fi
	if [ -n "$BXDECON" ]; then
		printf '#include "exynos9820-beyondx-decon.dtsi"\n' >> "$SRC/$DTS"
	fi
	if [ -n "$BXWLAN" ]; then
		printf '#include "exynos9820-beyondx-pcie.dtsi"\n' >> "$SRC/$DTS"
		# mainline brcmfmac has the 4375's siblings, not the 4375
		python3 "$ROOT/kernel/patch-brcmfmac-4375.py" "$SRC"
		# relaxed ordering off + sane ring indices (RX oops, 11-16)
		python3 "$ROOT/kernel/patch-brcmfmac-ordering.py" "$SRC"
	fi
	if [ -n "$MEM" ]; then
		printf '\n#include "exynos9820-beyondx-mem.dtsi"\n' >> "$SRC/$DTS"
	fi
	if [ -n "$BXUFS" ]; then
		printf '#include "exynos9820-beyondx-ufs.dtsi"\n' >> "$SRC/$DTS"
	fi
	grep -q exynos9820-beyondx "$SRC/arch/arm64/boot/dts/exynos/Makefile" ||
		sed -i 's|^\(\s*\)exynos9810-starlte.dtb\(.*\)$|&\n\1exynos9820-beyondx.dtb\2|' \
			"$SRC/arch/arm64/boot/dts/exynos/Makefile"
	grep -q exynos9820-beyondx "$SRC/arch/arm64/boot/dts/exynos/Makefile"
	# A55/A75 hardware MPIDRs alias in the suspend-context hash (CPU idle)
	python3 "$ROOT/kernel/patch-suspend-mpidr.py" "$SRC"
	# a late exynos5-hsi2c interrupt after a timeout read a stale message
	python3 "$ROOT/kernel/patch-i2c-exynos5-late-irq.py" "$SRC"
else
DTS=arch/arm64/boot/dts/exynos/exynos9810-starlte-zt.dts
python3 "$ROOT/kernel/make-dts.py" "$PORT/src/mainline-dts/exynos9810-star2lte.dts" \
	"$SRC/$DTS" "${DTS_FLAGS[@]}" --fallback "$FALLBACK"
grep -q starlte-zt "$SRC/arch/arm64/boot/dts/exynos/Makefile" ||
	sed -i 's|^\(\s*\)exynos9810-starlte.dtb\(.*\)$|&\n\1exynos9810-starlte-zt.dtb\2|' \
		"$SRC/arch/arm64/boot/dts/exynos/Makefile"
grep -q starlte-zt "$SRC/arch/arm64/boot/dts/exynos/Makefile"

echo "=== port drivers ==="
BUILD=$B bash "$PORT/scripts/build/apply_drivers.sh" "$SRC" | tail -n 3
# our own changes to the port's drivers, applied to the copies in the tree
# the panel brightness (AOR) as /sys/class/backlight/s9p-panel, and the panel
# commands sent with the frame trigger held off; kept here, not in the port
patch -d "$SRC" -p1 --forward --no-backup-if-mismatch < "$ROOT/kernel/s9p-decon-backlight.patch"
grep -q s9p_bl_ops "$SRC/drivers/gpu/drm/s9p/s9p-decon.c" ||
	{ echo "s9p-decon.c has no backlight device after patching" >&2; exit 1; }
# PCIe (the BCM4361 Wi-Fi) survives system sleep: stop the link on suspend and
# replay PHY/host setup on resume, since SYS_SLEEP loses ELBI and iATU state
grep -q exynos9810_pcie_pm_ops "$SRC/drivers/pci/controller/dwc/pcie-exynos9810.c" ||
	patch -d "$SRC" -p1 --forward --no-backup-if-mismatch < "$ROOT/kernel/s9p-pcie-pm.patch"
grep -q exynos9810_pcie_pm_ops "$SRC/drivers/pci/controller/dwc/pcie-exynos9810.c" ||
	{ echo "pcie-exynos9810.c has no suspend/resume PM ops after patching" >&2; exit 1; }
# a link that fails to train after sleep is tried again, and config accesses
# while it is down are answered as an absent device instead of hanging the SoC
grep -q exynos9810_pcie_guard_cfg "$SRC/drivers/pci/controller/dwc/pcie-exynos9810.c" ||
	patch -d "$SRC" -p1 --forward --no-backup-if-mismatch < "$ROOT/kernel/s9p-pcie-resume-retry.patch"
grep -q exynos9810_pcie_guard_cfg "$SRC/drivers/pci/controller/dwc/pcie-exynos9810.c" ||
	{ echo "pcie-exynos9810.c has no resume retry after patching" >&2; exit 1; }
python3 "$ROOT/kernel/patch-decon-wake.py" "$SRC"
python3 "$ROOT/kernel/patch-decon-flip.py" "$SRC"
python3 "$ROOT/kernel/patch-decon-panel-lock.py" "$SRC"
python3 "$ROOT/kernel/patch-decon-panel-io.py" "$SRC"
python3 "$ROOT/kernel/patch-suspend-mpidr.py" "$SRC"
python3 "$ROOT/kernel/patch-ufs-suspend.py" "$SRC"
python3 "$ROOT/kernel/patch-touch-suspend.py" "$SRC"
python3 "$ROOT/kernel/patch-pinctrl-resume.py" "$SRC"
fi

echo "=== initramfs ==="
LIST=$B/initramfs.list
cat > "$LIST" <<EOF
dir /bin 0755 0 0
dir /dev 0755 0 0
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /run 0755 0 0
dir /tmp 1777 0 0
dir /etc 0755 0 0
dir /newroot 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
nod /dev/kmsg 0600 0 0 c 1 11
file /bin/busybox $B/busybox-arm64 0755 0 0
slink /bin/sh busybox 0777 0 0
file /init $ROOT/initramfs/init 0755 0 0
EOF
if [ -n "$USB$PET$SPLASH" ]; then
	echo "dir /etc/zboot 0755 0 0" >> "$LIST"
fi
if [ -n "$SPLASH" ]; then
	echo "file /etc/zboot/splash $ROOT/kernel/beyondx/splash.sh 0644 0 0" >> "$LIST"
	echo "file /etc/zboot/splash.raw.gz $ROOT/kernel/beyondx/splash.raw.gz 0644 0 0" >> "$LIST"
fi
if [ -n "$PET" ]; then
	echo "file /etc/zboot/early $ROOT/kernel/beyondx/wdt-pet.sh 0644 0 0" >> "$LIST"
	echo "file /etc/zboot/pre-switch $ROOT/kernel/beyondx/pre-switch.sh 0644 0 0" >> "$LIST"
fi
# beyondx: ztemp reads the SoC's TMU (devmem only); zthermal guards against
# heat from it (started by wdt-pet.sh)
if [ "$DEVICE" = beyondx ]; then
	echo "file /bin/ztemp $ROOT/kernel/beyondx/ztemp.sh 0755 0 0" >> "$LIST"
	echo "file /bin/zthermal $ROOT/kernel/beyondx/zthermal.sh 0755 0 0" >> "$LIST"
fi
# beyondx --muic: zbatt reads the MAX77705's gauge and charger over that bus
if [ -n "$MUIC" ]; then
	echo "file /bin/zbatt $ROOT/kernel/beyondx/zbatt.sh 0755 0 0" >> "$LIST"
	echo "file /bin/zcharge $ROOT/kernel/beyondx/zcharge.sh 0755 0 0" >> "$LIST"
	echo "file /bin/zchg $ROOT/kernel/beyondx/zchg.sh 0755 0 0" >> "$LIST"
fi
# beyondx --ufs: zufs brings the internal UFS up by hand, read-only
if [ -n "$BXUFS" ]; then
	echo "file /bin/zufs $ROOT/kernel/beyondx/zufs.sh 0755 0 0" >> "$LIST"
	[ -n "$BXROOT" ] && echo "file /etc/zboot/pre-root $ROOT/kernel/beyondx/pre-root.sh 0644 0 0" >> "$LIST"
fi
# beyondx --usb adds its modules to this list in the kernel step below;
# USB_MODS are loaded in this order, the I2C bus driver on its own
USB_MODS="phy-exynos5-usbdrd.ko dwc3.ko dwc3-exynos.ko"
MOD_SRCS="drivers/phy/samsung/phy-exynos5-usbdrd.ko drivers/usb/dwc3/dwc3.ko drivers/usb/dwc3/dwc3-exynos.ko"
ALL_MODS=$USB_MODS
if [ -n "$MUIC" ]; then
	ALL_MODS="$ALL_MODS i2c-exynos5.ko"
	MOD_SRCS="$MOD_SRCS drivers/i2c/busses/i2c-exynos5.ko"
fi

echo "=== config ==="
cd "$SRC"
KOUT=out
[ "$DEVICE" = beyondx ] && KOUT=out-beyondx	# the S9's .config stays as it is
[ -f $KOUT/.config ] || make -s O=$KOUT defconfig
cfg() { ./scripts/config --file $KOUT/.config "$@"; }
for sym in ARCH_ACTIONS ARCH_AIROHA ARCH_SUNXI ARCH_ALPINE ARCH_APPLE ARCH_BCM \
	ARCH_BCM2835 ARCH_BCMBCA ARCH_BRCMSTB ARCH_BERLIN ARCH_BLAIZE ARCH_EXYNOSAUTOV9 \
	ARCH_SPARX5 ARCH_K3 ARCH_LG1K ARCH_HISI ARCH_KEEMBAY ARCH_MEDIATEK ARCH_MESON \
	ARCH_MVEBU ARCH_MXC ARCH_LAYERSCAPE ARCH_MA35 ARCH_NPCM ARCH_NXP ARCH_QCOM \
	ARCH_REALTEK ARCH_RENESAS ARCH_ROCKCHIP ARCH_S32 ARCH_SEATTLE ARCH_INTEL_SOCFPGA \
	ARCH_STM32 ARCH_SYNQUACER ARCH_TEGRA ARCH_TESLA_FSD ARCH_SPRD ARCH_THUNDER \
	ARCH_THUNDER2 ARCH_UNIPHIER ARCH_VEXPRESS ARCH_VISCONTI ARCH_XGENE ARCH_ZYNQMP \
	ARCH_SOPHGO ARCH_STM32MP ARCH_MMP ARCH_WPCM450 ARCH_VT8500 \
	SOUND SND MEDIA_SUPPORT PCI PCIE_DW ACPI EFI NUMA XEN KVM VIRTUALIZATION \
	VIRTIO_MENU INFINIBAND RDMA_RXE MTD ATA MD NVME_CORE SCSI_LOWLEVEL \
	ETHERNET WLAN WWAN USB_NET_DRIVERS PHYLIB NET_VENDOR_SAMSUNG \
	INPUT_JOYSTICK INPUT_TABLET INPUT_MISC \
	HID_SUPPORT IIO HWMON W1 FUSION FIREWIRE MACINTOSH_DRIVERS \
	BT CAN NFC WIRELESS RFKILL \
	BTRFS_FS XFS_FS F2FS_FS GFS2_FS OCFS2_FS JFS_FS NTFS3_FS EROFS_FS SQUASHFS \
	NFS_FS NFSD CIFS CEPH_FS 9P_FS CODA_FS AFS_FS ORANGEFS_FS \
	DRM_AMDGPU DRM_NOUVEAU DRM_I915 DRM_RADEON DRM_MSM DRM_ETNAVIV DRM_ROCKCHIP \
	DRM_MEDIATEK DRM_TEGRA DRM_VC4 DRM_SUN4I DRM_ARM DRM_KOMEDA DRM_HDLCD DRM_MALI_DISPLAY \
	DRM_PL111 DRM_RCAR_DU DRM_IMX DRM_MXSFB DRM_MESON DRM_TIDSS DRM_VIRTIO_GPU \
	DRM_LIMA DRM_V3D DRM_EXYNOS DRM_TVE200 DRM_LOGICVC DRM_SSD130X \
	DRM_PANEL_BRIDGE DRM_DISPLAY_DP_HELPER DRM_DP_AUX_CHARDEV \
	STAGING COMEDI GREYBUS FPGA MAILBOX_TEST TEE OPTEE \
	SERIAL_8250 I2C_MUX SPI_MEM MTD_SPI_NOR \
	COMPAT DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT FTRACE STACK_TRACER \
	SECURITY_SELINUX SECURITY_APPARMOR SECURITY_SMACK SECURITY_TOMOYO \
	NET_SCHED NET_CLS_ACT IP_VS BRIDGE VLAN_8021Q \
	TYPEC WATCHDOG_NOWAYOUT \
	USB_G_SERIAL RANDOMIZE_BASE ARM64_VA_BITS_52 ARM64_PA_BITS_52 \
	PSTORE_PMSG PSTORE_COMPRESS STRICT_DEVMEM SCSI_UFS_CRYPTO; do
	cfg --disable "$sym"
done
make -s O=$KOUT olddefconfig
for sym in BLK_DEV_INITRD DRM DRM_EXYNOS_BOOTFB DRM_FBDEV_EMULATION \
	FRAMEBUFFER_CONSOLE FRAMEBUFFER_CONSOLE_DETECT_PRIMARY SYSFB_SIMPLEFB \
	ARCH_EXYNOS PINCTRL_EXYNOS KEYBOARD_GPIO INPUT_EVDEV EXYNOS_PMU EXYNOS_PM_DOMAINS \
	DMA_CMA SCSI BLK_DEV_SD SCSI_UFSHCD SCSI_UFSHCD_PLATFORM SCSI_UFS_EXYNOS9810 \
	USB_GADGET USB_LIBCOMPOSITE USB_U_SERIAL USB_U_ETHER USB_F_ACM USB_F_NCM USB_F_ECM \
	USB_F_RNDIS USB_F_MASS_STORAGE USB_CONFIGFS USB_CONFIGFS_SERIAL USB_CONFIGFS_ACM \
	USB_CONFIGFS_NCM USB_CONFIGFS_ECM USB_CONFIGFS_RNDIS USB_CONFIGFS_MASS_STORAGE \
	USB_CONFIGFS_F_FS USB_DWC3 USB_DWC3_GADGET GENERIC_PHY PHY_EXYNOS5_USBDRD \
	I2C I2C_CHARDEV I2C_EXYNOS5 INPUT_TOUCHSCREEN TOUCHSCREEN_S6SY761 \
	MFD_MAX77705 MFD_SIMPLE_MFD_I2C CHARGER_MAX77705 BATTERY_MAX1720X BATTERY_MAX17042 \
	POWER_SUPPLY REGMAP_I2C REGMAP_IRQ EXT4_FS TMPFS DEVTMPFS DEVTMPFS_MOUNT \
	PSTORE PSTORE_RAM PSTORE_CONSOLE MAGIC_SYSRQ DEVMEM \
	POWER_RESET_SYSCON POWER_RESET_SYSCON_POWEROFF SYSCON_REBOOT_MODE \
	WATCHDOG WATCHDOG_SYSFS S3C2410_WATCHDOG ARM64_VA_BITS_48 ARM64_PA_BITS_48 \
	UCLAMP_TASK CMDLINE_FORCE \
	NETFILTER CGROUPS NAMESPACES FHANDLE AUTOFS_FS SECCOMP INOTIFY_USER SIGNALFD \
	SECURITY_LANDLOCK \
	TIMERFD EPOLL UNIX INET IPV6 PACKET TUN ZRAM FUSE_FS; do
	cfg --enable "$sym"
done
# The S10 5G at stage 3 draws on the bootloader's framebuffer through
# mainline's simple-framebuffer driver; the S9's bootfb is the port's own.
if [ "$DEVICE" = beyondx ]; then
	cfg --enable DRM_SIMPLEDRM
	cfg --enable PSTORE_PMSG
	if [ -n "$USB" ]; then
		cfg --module USB_DWC3
		cfg --module USB_DWC3_EXYNOS
		cfg --module PHY_EXYNOS5_USBDRD
	fi
	[ -n "$MUIC" ] && cfg --module I2C_EXYNOS5
	# --gpu: Panfrost as modules, loaded by hand from the root system
	if [ -n "$BXGPU" ]; then
		cfg --module DRM_PANFROST
		cfg --enable IOMMU_SUPPORT
		cfg --enable IOMMU_IO_PGTABLE_LPAE
	fi
	# --wlan: PCI in the kernel (PCI_EXYNOS=m only to pull the DesignWare
	# host core in); the host, cfg80211 and brcmfmac are modules
	if [ -n "$BXWLAN" ]; then
		cfg --enable PCI
		cfg --enable PCI_MSI
		cfg --module PCI_EXYNOS
		cfg --enable WIRELESS
		cfg --module CFG80211
		cfg --enable WLAN
		cfg --enable WLAN_VENDOR_BROADCOM
		cfg --module BRCMUTIL
		cfg --module BRCMFMAC
		cfg --enable BRCMFMAC_PCIE
	fi
	# 8x16 is a line of dust on 1440x3040
	cfg --enable FONTS
	cfg --keep-case --enable FONT_TER16x32	# scripts/config would make it TER16X32
fi
# Suspend stage tests are opt-in and do not enter sleep automatically.
if [ -n "$PMDEBUG" ]; then
	cfg --enable PM_DEBUG
	cfg --enable PM_ADVANCED_DEBUG
fi
# A CPU stuck with interrupts on or off panics with its backtrace (into
# ramoops) instead of hanging silently until the hardware watchdog.
if [ -n "$LOCKUP" ]; then
	cfg --enable SOFTLOCKUP_DETECTOR
	cfg --enable BOOTPARAM_SOFTLOCKUP_PANIC
	cfg --enable HARDLOCKUP_DETECTOR
	cfg --enable HARDLOCKUP_DETECTOR_PREFER_BUDDY
	cfg --enable BOOTPARAM_HARDLOCKUP_PANIC
fi
# Everything below this line is a block that hangs the bus when it is read
# while gated or unpowered, so each one is a switch, off by default.
if [ -n "$ACPM" ]; then
	cfg --enable S9P_ACPM
else
	cfg --disable S9P_ACPM
fi
if [ -n "$CPUFREQ" ]; then
	cfg --enable CPU_FREQ
	cfg --enable CPU_FREQ_STAT
	cfg --enable CPU_FREQ_GOV_SCHEDUTIL
	cfg --enable CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
	cfg --enable ARM_S9P_CPUFREQ
else
	cfg --disable CPU_FREQ
	cfg --disable ARM_S9P_CPUFREQ
fi
if [ -n "$GPU" ]; then
	cfg --enable DRM_PANFROST
	cfg --enable IOMMU_SUPPORT
	cfg --enable IOMMU_IO_PGTABLE_LPAE
	cfg --enable ARM_MALI_LPAE
	cfg --enable DRM_GEM_SHMEM_HELPER
elif [ -z "$BXGPU" ]; then
	cfg --disable DRM_PANFROST
fi
if [ -n "$DECON" ]; then
	cfg --enable DRM_S9P_DECON
	cfg --enable DRM_KMS_HELPER
	cfg --enable DRM_GEM_DMA_HELPER
	# the panel's own brightness (AOR), as a backlight device
	cfg --enable BACKLIGHT_CLASS_DEVICE
else
	cfg --disable DRM_S9P_DECON
fi
if [ -n "$WLAN" ]; then
	# The firmware is this phone's own, out of its VENDOR partition, renamed
	# the way brcmfmac asks for it; the nvram names itself "STAR1 KR", so it
	# is the S9's calibration and not the S9+'s.
	for f in bcmdhd_sta.bin_b2 nvram.txt_r02a_b2 bcmdhd_clm.blob; do
		[ -s "$ROOT/firmware/$f" ] || { echo "missing firmware/$f" >&2; exit 1; }
	done
	cfg --enable PCI
	cfg --enable PCIE_DW
	cfg --enable PCIE_DW_HOST
	cfg --enable PCIE_EXYNOS9810
	cfg --enable WLAN
	cfg --enable WIRELESS
	cfg --enable CFG80211
	cfg --enable BRCMUTIL
	cfg --enable BRCMFMAC
	cfg --enable BRCMFMAC_PCIE
	FW=$B/firmware
	mkdir -p "$FW/brcm"
	cp "$ROOT/firmware/bcmdhd_sta.bin_b2" "$FW/brcm/brcmfmac4361-pcie.bin"
	cp "$ROOT/firmware/nvram.txt_r02a_b2" "$FW/brcm/brcmfmac4361-pcie.txt"
	cp "$ROOT/firmware/bcmdhd_clm.blob" "$FW/brcm/brcmfmac4361-pcie.clm_blob"
	sed -i 's/\r$//' "$FW/brcm/brcmfmac4361-pcie.txt"
	cp "$PORT/firmware/regulatory.db" "$PORT/firmware/regulatory.db.p7s" "$FW/"
	cfg --set-str EXTRA_FIRMWARE_DIR "$FW"
	cfg --set-str EXTRA_FIRMWARE "brcm/brcmfmac4361-pcie.bin brcm/brcmfmac4361-pcie.txt brcm/brcmfmac4361-pcie.clm_blob regulatory.db regulatory.db.p7s"
else
	cfg --set-str EXTRA_FIRMWARE ""
fi
cfg --set-str INITRAMFS_SOURCE "$LIST"
if [ -n "$SPLASH" ]; then
	# fbcon leaves the framebuffer alone until there is text for it (the
	# rescue shell, a panic), and the boot image's 20-byte empty ramdisk is
	# not unpacked ("Initramfs unpacking failed: junk at the end", the one
	# KERN_EMERG line that came through loglevel=1); needs CMDLINE_FORCE
	cfg --enable FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER
	cfg --enable INITRAMFS_FORCE
fi
cfg --set-val CMA_SIZE_MBYTES 128
cfg --set-str LOCALVERSION "-zboot"
# The watchdog is armed by uniLoader and again by the clock-gates node, then
# taken over by s3c2410_wdt (tmr_atboot). The kernel pets it until userspace
# opens /dev/watchdog, so a wedged kernel resets into the fallback target.
CMDLINE="console=tty0 console=ramoops-1 loglevel=7 panic=5 keep_bootcon maxcpus=8 s9p_vendor_m3_defaults s3c2410_wdt.tmr_atboot=1 s3c2410_wdt.tmr_margin=60${EXTRA:+ $EXTRA}"
if [ "$DEVICE" = beyondx ]; then
	# Stage 3: one CPU, the log stays on the screen after a panic, and the
	# rescue shell instead of a root filesystem. Nothing pets uniLoader's
	# watchdog yet, so 30 s after sboot it resets into the fallback target
	# whatever happens: that is the way back. zboot.dump=10 panics on
	# purpose first, so the log is in ramoops when TWRP comes up.
	# With --usb the dump waits until usb_up has had its 10 s for a UDC.
	# With --pet the watchdog is fed and nothing panics on purpose.
	# With --smp all CPUs come up at boot.
	DUMP=" zboot.dump=10"
	RESCUE=" zboot.rescue=1"
	[ -n "$BXROOT" ] && RESCUE=
	MAXCPUS=" maxcpus=1"
	[ -z "$SMP" ] || MAXCPUS=
	[ -n "$USB" ] && DUMP=" zboot.dump=22"
	[ -n "$PET" ] && DUMP=" zboot.wdt=pet"
	CMDLINE="console=tty0 fbcon=font:TER16x32 loglevel=8 panic=0 keep_bootcon$MAXCPUS clk_ignore_unused pd_ignore_unused$RESCUE$DUMP${EXTRA:+ $EXTRA}"
fi
cfg --set-str CMDLINE "$CMDLINE"
make -s O=$KOUT olddefconfig
grep -E '^CONFIG_(CMDLINE|SCSI_UFS_EXYNOS9810|USB_CONFIGFS_NCM|S3C2410_WATCHDOG|PSTORE_RAM|DRM_EXYNOS_BOOTFB|PHY_EXYNOS5_USBDRD|USB_DWC3_GADGET|USB_DWC3_MODE)' $KOUT/.config ||
	true
grep -E '^(# )?CONFIG_(S9P_ACPM|ARM_S9P_CPUFREQ|DRM_PANFROST|DRM_S9P_DECON|CPU_FREQ)[ =]' $KOUT/.config || true

echo "=== kernel ==="
: > "$B/kernel-build.log"
if [ -n "$USB" ]; then
	# Modules are checked against the vmlinux they will load into, and that
	# vmlinux carries the initramfs they go into: link it once without them,
	# build them, then link again with them in the list.
	grep -E '^CONFIG_(USB_DWC3|USB_DWC3_EXYNOS|PHY_EXYNOS5_USBDRD|PSTORE_PMSG|I2C_EXYNOS5|I2C_CHARDEV)=' $KOUT/.config
	make -j"$(nproc)" O=$KOUT Image >> "$B/kernel-build.log" 2>&1 || {
		grep -nE '\*\*\*|error:|Error [0-9]|undefined reference' "$B/kernel-build.log" | head -40
		exit 1
	}
	# shellcheck disable=SC2086
	make -j"$(nproc)" O=$KOUT $MOD_SRCS >> "$B/kernel-build.log" 2>&1 || {
		grep -nE '\*\*\*|error:|Error [0-9]|undefined' "$B/kernel-build.log" | head -40
		exit 1
	}
	# --gpu: Panfrost and its scheduler, copied to the phone by hand
	if [ -n "$BXGPU" ]; then
		make -j"$(nproc)" O=$KOUT drivers/gpu/drm/scheduler/gpu-sched.ko \
			drivers/gpu/drm/panfrost/panfrost.ko >> "$B/kernel-build.log" 2>&1 || exit 1
		rm -rf "$B/gpu-modules"
		mkdir -p "$B/gpu-modules"
		cp $KOUT/drivers/gpu/drm/scheduler/gpu-sched.ko \
			$KOUT/drivers/gpu/drm/panfrost/panfrost.ko "$B/gpu-modules/"
		"${CROSS_COMPILE}strip" --strip-debug "$B"/gpu-modules/*.ko
	fi
	# --decon: kernel/beyondx/decon (external) and the GEM DMA helper it
	# needs, copied to the phone by hand
	if [ -n "$BXDECON" ]; then
		rm -rf "$B/decon-module" "$B/decon-modules"
		cp -r "$ROOT/kernel/beyondx/decon" "$B/decon-module"
		make -j"$(nproc)" O=$KOUT drivers/gpu/drm/drm_dma_helper.ko >> "$B/kernel-build.log" 2>&1 &&
		make -j"$(nproc)" O=$KOUT M="$B/decon-module" modules >> "$B/kernel-build.log" 2>&1 || {
			grep -nE '\*\*\*|error:|Error [0-9]|undefined|warning:' "$B/kernel-build.log" | head -40
			exit 1
		}
		mkdir -p "$B/decon-modules"
		cp $KOUT/drivers/gpu/drm/drm_dma_helper.ko "$B/decon-module/bx-decon.ko" "$B/decon-modules/"
		"${CROSS_COMPILE}strip" --strip-debug "$B"/decon-modules/*.ko
	fi
	# --wlan: kernel/beyondx/pcie (external), cfg80211 and brcmfmac with
	# its WCC vendor part, copied to the phone by hand
	if [ -n "$BXWLAN" ]; then
		rm -rf "$B/pcie-module" "$B/wlan-modules"
		cp -r "$ROOT/kernel/beyondx/pcie" "$B/pcie-module"
		BRCM=drivers/net/wireless/broadcom/brcm80211
		make -j"$(nproc)" O=$KOUT net/wireless/cfg80211.ko \
			$BRCM/brcmutil/brcmutil.ko $BRCM/brcmfmac/brcmfmac.ko \
			$BRCM/brcmfmac/wcc/brcmfmac-wcc.ko >> "$B/kernel-build.log" 2>&1 &&
		make -j"$(nproc)" O=$KOUT M="$B/pcie-module" modules >> "$B/kernel-build.log" 2>&1 || {
			grep -nE '\*\*\*|error:|Error [0-9]|undefined|warning:' "$B/kernel-build.log" | head -40
			exit 1
		}
		mkdir -p "$B/wlan-modules"
		cp "$B/pcie-module/pcie-exynos9820.ko" $KOUT/net/wireless/cfg80211.ko \
			$KOUT/$BRCM/brcmutil/brcmutil.ko $KOUT/$BRCM/brcmfmac/brcmfmac.ko \
			$KOUT/$BRCM/brcmfmac/wcc/brcmfmac-wcc.ko "$B/wlan-modules/"
		"${CROSS_COMPILE}strip" --strip-debug "$B"/wlan-modules/*.ko
	fi
	rm -rf "$B/usb-modules"
	mkdir -p "$B/usb-modules"
	for m in $ALL_MODS; do
		cp "$KOUT/$(cd $KOUT && find drivers -name "$m")" "$B/usb-modules/$m"
	done
	# load order: the PHY, the DWC3 core, then the glue that adds the core's device
	for m in $USB_MODS; do
		echo "$m" >> "$B/usb-modules/order"
	done
	if [ -n "$BXUFS" ]; then
		# kernel/beyondx/ufs is an external module, built from a copy on
		# the SSD next to the tree; zufs loads it, not the order file
		rm -rf "$B/ufs-module"
		cp -r "$ROOT/kernel/beyondx/ufs" "$B/ufs-module"
		make -j"$(nproc)" O=$KOUT M="$B/ufs-module" modules >> "$B/kernel-build.log" 2>&1 || {
			grep -nE '\*\*\*|error:|Error [0-9]|undefined|warning:' "$B/kernel-build.log" | head -40
			exit 1
		}
		cp "$B/ufs-module/ufs-exynos9820.ko" "$B/usb-modules/"
	fi
	"${CROSS_COMPILE}strip" --strip-debug "$B"/usb-modules/*.ko
	ls -l "$B/usb-modules"
	cat >> "$LIST" <<EOF
dir /lib 0755 0 0
dir /lib/modules 0755 0 0
dir /lib/modules/zboot 0755 0 0
file /etc/zboot/pre-usb $ROOT/kernel/beyondx/pre-usb.sh 0644 0 0
file /lib/modules/zboot/order $B/usb-modules/order 0644 0 0
EOF
	for m in $ALL_MODS ${BXUFS:+ufs-exynos9820.ko}; do
		echo "file /lib/modules/zboot/$m $B/usb-modules/$m 0644 0 0" >> "$LIST"
	done
fi
make -j"$(nproc)" O=$KOUT Image dtbs >> "$B/kernel-build.log" 2>&1 || {
	grep -nE '\*\*\*|error:|Error [0-9]|undefined reference' "$B/kernel-build.log" | head -40
	exit 1
}
IMAGE=$SRC/$KOUT/arch/arm64/boot/Image
DTB=$SRC/$KOUT/arch/arm64/boot/dts/exynos/$(basename "$DTS" .dts).dtb
ls -l "$IMAGE" "$DTB"
if grep -q '=m$' $KOUT/.config; then
	echo "note: $(grep -c '=m$' $KOUT/.config) symbols are =m and are not built"
fi

echo "=== uniLoader ==="
STAGE=$B/uniloader-stage
rm -rf "$STAGE"
if [ "$DEVICE" = beyondx ]; then
	cp -r "$ROOT/third_party/uniLoader" "$STAGE"
	rm -rf "$STAGE/.git"
	python3 "$ROOT/kernel/beyondx/uniloader-board.py" "$STAGE" $BXMENU
	python3 "$ROOT/kernel/uniloader-failsafe.py" "$STAGE/board/samsung/board-beyondx.c" "$INFORM3"
	UNILOADER_DEFCONFIG=beyondx_defconfig
else
	cp -r "$PORT/bootloader/uniLoader" "$STAGE"
	python3 "$ROOT/kernel/uniloader-failsafe.py" "$STAGE/board/samsung/board-starlte.c" "$INFORM3"
	UNILOADER_DEFCONFIG=star2lte_defconfig
fi
mkdir -p "$STAGE/blob"
cp "$IMAGE" "$STAGE/blob/Image"
cp "$DTB" "$STAGE/blob/dtb"
# The port's validated ramdisk blob: a 20-byte empty gzip (see its
# build_uniloader.sh for why this is not a cpio archive).
printf '' | gzip -n > "$STAGE/blob/ramdisk"
(
	cd "$STAGE"
	make ARCH=aarch64 CROSS_COMPILE=$CROSS_COMPILE $UNILOADER_DEFCONFIG
	make ARCH=aarch64 CROSS_COMPILE=$CROSS_COMPILE -j"$(nproc)"
) > "$B/uniloader-build.log" 2>&1 || { tail -n 30 "$B/uniloader-build.log"; exit 1; }
ls -l "$STAGE/uniLoader"

echo "=== boot image ==="
rm -f "$OUT.img"
if [ "$DEVICE" = beyondx ]; then
	# initramfs is built into the kernel; the DTB partition stays stock
	python3 -I "$ROOT/kernel/mkboot-v1.py" --ref "$REF" --kernel "$STAGE/uniLoader" \
		--cmdline "console=tty0" -o "$OUT.img"
else
PARTS=$B/parts
mkdir -p "$PARTS"
python3 -I - "$PORT/scripts/build" "$REF" "$PARTS" <<'PY'
import gzip, sys
sys.path.insert(0, sys.argv[1])
from boot_parts import parts
p = parts(open(sys.argv[2], "rb").read())
open(sys.argv[3] + "/dt.img", "wb").write(p["dt"])
open(sys.argv[3] + "/ramdisk.gz", "wb").write(gzip.compress(b"", compresslevel=9, mtime=0))
PY
python3 -I "$PORT/scripts/build/mkboot.py" --ref "$REF" --kernel "$STAGE/uniLoader" \
	--ramdisk "$PARTS/ramdisk.gz" --dt "$PARTS/dt.img" --cmdline "console=tty0" -o "$OUT.img"
fi
cp "$SRC/$KOUT/.config" "$OUT.config"
cp "$SRC/$DTS" "$OUT.dts"
echo "$CMDLINE" > "$OUT.cmdline"
(cd "$ROOT/out" && sha256sum "$NAME.img" > "$NAME.img.sha256" && cat "$NAME.img.sha256")
echo "BUILD_OK $OUT.img device=$DEVICE fallback=$FALLBACK"
