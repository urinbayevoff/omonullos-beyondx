# Galaxy S10 5G: look before the USB drivers touch anything, then load them.
# Sourced by initramfs/init (as /etc/zboot/pre-usb) right before usb_up, with
# its log() at hand; scripts/build-boot.sh --device beyondx --usb puts it there
# together with the modules in /lib/modules/zboot.
#
# Added for OmonullOS, October 2026. Tested (bx3, 2026-10-16): FSYS0A is
# powered at handover, every read below answers, the modules load and the
# gadget binds; the host sees nothing yet, hence usb_watch (bx4: the PHY
# pulls D+ up, the pair stops at the MAX77705's MUIC switch, hence muic;
# bx5: its checks held, S-Boot leaves USI_CMGP3 unconfigured, hence the
# setup there; bx8: with the APCmdResI handshake the switch closes and the
# host enumerates the gadget, telnet works).
#
# A read from an unpowered or gated Exynos block hangs the bus with no
# message, and the watchdog then resets into TWRP with nothing in ramoops.
# So every step is written to /dev/pmsg0 first: pmsg lives in the same
# persistent RAM and TWRP shows it as /sys/fs/pstore/pmsg-ramoops-0, so the
# last line there names the read that hung. The PMU and the top CMU are
# always on; FSYS0A (USBCON, DWC3, its CMU) is read only once the PMU says
# it is powered and the top gates feeding it are not switched off by hand.
# Addresses: stock cmucal-sfr.c and flexpmu_cal_local_exynos9820.h.

USB_PRODUCT="Galaxy S10 5G zboot rescue"
USB_SERIAL=beyondx

pm() {
	echo "$*" > /dev/pmsg0 2>/dev/null
}

rd() {	# rd <name> <address>: read 32 bits, logged before and after
	pm "rd $1 $2"
	v=$(devmem "$2" 32 2>/dev/null)
	log "usb-probe: $1 [$2] = ${v:-?}"
	pm "  = ${v:-?}"
}

# A CLK_CON_GAT register is off only when MANUAL (bit 20) is set and
# CG_VALUE (bit 21) is not; otherwise the hardware runs it.
gate_on() {
	[ $(( ($1 >> 20) & 3 )) -ne 1 ]
}

pre_usb() {
	pm "zboot pre-usb $(cut -d' ' -f1 /proc/uptime)"

	rd pmu.fsys0a.config 0x15862200
	rd pmu.fsys0a.status 0x15862204
	st=$v
	rd pmu.usb20.phy_ctrl 0x1586072c
	rd cmu_top.gate.fsys0a_bus 0x1a242060
	g1=$v
	rd cmu_top.gate.fsys0a_usb31drd 0x1a242064
	g2=$v
	rd cmu_top.mux.fsys0a_bus 0x1a241050
	rd cmu_top.mux.fsys0a_usb31drd 0x1a241054

	if [ -z "$st" ] || [ $((st & 1)) -ne 1 ]; then
		log "usb-probe: FSYS0A is powered off; not loading the USB drivers"
		pm "stop: fsys0a off"
		return 1
	fi
	if [ -z "$g1" ] || [ -z "$g2" ] || ! gate_on "$g1" || ! gate_on "$g2"; then
		log "usb-probe: a top gate into FSYS0A is off; not loading the USB drivers"
		pm "stop: top gate off"
		return 1
	fi

	rd cmu_fsys0a.mux.bus_user 0x10a00100
	rd cmu_fsys0a.mux.usb31drd_user 0x10a00120
	rd cmu_fsys0a.gate.aclk_bus 0x10a02014
	rd cmu_fsys0a.gate.aclk_phyctrl 0x10a0201c
	rd cmu_fsys0a.gate.bus_clk_early 0x10a02020
	rd cmu_fsys0a.gate.ref_clk_40 0x10a02024
	rd usbcon.ctrl_ver 0x10b00000
	rd usbcon.linkctrl 0x10b00004
	rd usbcon.linkport 0x10b00008
	rd usbcon.clkrst 0x10b00020
	rd usbcon.utmi 0x10b00050
	rd usbcon.hsp 0x10b00054
	rd usbcon.hsp_test 0x10b0005c
	rd dwc3.gsnpsid 0x10c0c120

	for m in $(cat /lib/modules/zboot/order); do
		pm "insmod $m"
		if insmod "/lib/modules/zboot/$m" 2>/run/insmod.err; then
			log "usb-probe: insmod $m"
		else
			log "usb-probe: insmod $m failed: $(cat /run/insmod.err)"
		fi
	done
	pm "modules loaded $(cut -d' ' -f1 /proc/uptime)"
	rd pmu.usb20.phy_ctrl 0x1586072c
	muic
	usb_watch &
}

# --- MAX77705 (build-boot.sh --muic) -------------------------------------------
# bx4: the AP's USB works up to the connector, but the pair goes through the
# MAX77705's MUIC switch, which only Samsung's driver closes. The chip is
# reached through its USB-C firmware's mailbox at I2C 0x25 (stock
# drivers/ccic/max77705_usbc.c): the opcode and its data at 0x21.., 0x00 at
# 0x41 to end it, the answer at 0x51 (opcode echoed, then data). CTRL1_R 0x05
# reads the switch, CTRL1_W 0x06 sets it; COM_USB = 0x09 (COMN1SW and
# COMP2SW = USB, include/linux/muic/max77705-muic.h). Only status registers
# are read (0x02-0x05 clear on read and are left alone); the switch is
# written only with zboot.muic=usb on the command line.
ic() {	# ic <addr> <reg>: one byte, logged
	pm "i2cget $1 $2"
	v=$(i2cget -y "$IBUS" "$1" "$2" 2>&1)
	log "muic: i2c $1 [$2] = $v"
	pm "  = $v"
}

# bx7: CTRL1_W after an unanswered CTRL1_R did nothing (the switch read 0x3f
# before and after). Samsung's driver runs one opcode at a time: it sends
# the next only after the firmware's APCmdResI (UIC_INT bit 7, cleared by
# reading UIC_INT) has come for the last one, and reads the answer then. So:
# UIC_INT is read (cleared) before a command and polled after it.
uic_int() {
	v=$(i2cget -y "$IBUS" 0x25 0x02 2>/dev/null)
	echo "${v:-0}"
}

mbox() {	# mbox <name> <opcode> [data]: one opcode, answer logged
	n=$1
	shift
	pm "mbox $n $*"
	log "muic: $n: UIC_INT before = $(uic_int)"
	if [ $# -gt 1 ]; then
		i2ctransfer -y "$IBUS" w$(($# + 1))@0x25 0x21 "$@"
	else
		i2cset -y "$IBUS" 0x25 0x21 "$1"
	fi
	i2cset -y "$IBUS" 0x25 0x41 0x00
	i=0
	u=0
	while [ $i -lt 50 ]; do
		u=$(uic_int)
		[ $((u & 0x80)) -ne 0 ] && break
		i=$((i + 1))
		sleep 0.02
	done
	v=$(i2ctransfer -y "$IBUS" w1@0x25 0x51 r2 2>&1)
	log "muic: $n: UIC_INT $u after $i polls, answer (opcode, data) = $v"
	pm "  int $u polls $i = $v"
}

ctrl1() {
	mbox CTRL1_R 0x05
}

# --touch: USI03 of PERIC0 as I2C, the touchscreen's bus (hsi2c_10), before
# the bus driver loads, the way muic() does USI_CMGP3. Stock: usi_3 mode i2c,
# hsi2c10-bus = gpp1-4/gpp1-5 function 2, no pull, drive 0. Read on bx22:
# PERIC0's USI03 gates on, SW_CONF 0, gpp1 inputs with pull-down, drive 2,
# USI_CON 1.
# The touch IC's rails stay off here (zacpm touch on, from the root system).
touch_i2c() {
	[ "$(cat /proc/device-tree/soc@0/i2c@104b0000/status 2>/dev/null | tr -d '\0')" = okay ] || return 0
	pm "zboot touch_i2c"
	rd cmu_peric0.gate.usi03_ipclk 0x1040209c
	gi=$v
	rd cmu_peric0.gate.usi03_pclk 0x104020a0
	gp=$v
	if [ -z "$gi" ] || [ -z "$gp" ] || ! gate_on "$gi" || ! gate_on "$gp"; then
		log "touch: a USI03 gate is off; leaving hsi2c_10 alone"
		pm "stop: usi03 gate off"
		return 1
	fi
	rd sysreg_peric0.usi03_mode 0x1042101c
	rd gpio_peric0.gpp1_con 0x10430020
	rd gpio_peric0.gpp1_pud 0x10430028
	rd gpio_peric0.gpp1_drv 0x1043002c
	pm "setup usi03 as i2c"
	log "touch: setting USI03 up as I2C (SW_CONF 4, gpp1-4/gpp1-5 function 2, no pull, drive 0)"
	devmem 0x1042101c 32 4
	devmem 0x10430028 32 $(( $(devmem 0x10430028 32) & ~0xff0000 ))
	devmem 0x1043002c 32 $(( $(devmem 0x1043002c 32) & ~0xff0000 ))
	devmem 0x10430020 32 $(( ($(devmem 0x10430020 32) & ~0xff0000) | 0x220000 ))
	rd sysreg_peric0.usi03_mode 0x1042101c
	rd gpio_peric0.gpp1_con 0x10430020
	rd gpio_peric0.gpp1_pud 0x10430028
	rd gpio_peric0.gpp1_drv 0x1043002c
	rd hsi2c10.usi_con 0x104b00c4
	if [ -n "$v" ] && [ $((v & 1)) -eq 1 ]; then
		pm "clear usi03 usi_reset"
		devmem 0x104b00c4 32 0
		rd hsi2c10.usi_con 0x104b00c4
	fi
}

muic() {
	[ -f /lib/modules/zboot/i2c-exynos5.ko ] || return 0
	touch_i2c
	pm "zboot muic $(cut -d' ' -f1 /proc/uptime)"
	rd cmu_cmgp.mux.usi3 0x15c01020
	rd cmu_cmgp.div.usi3 0x15c01824
	rd cmu_cmgp.gate.usi3_ipclk 0x15c02088
	gi=$v
	rd cmu_cmgp.gate.usi3_pclk 0x15c0208c
	gp=$v
	rd sysreg_cmgp.usi3_mode 0x15c22030
	mode=$v
	rd gpio_cmgp.gpm12_con 0x15c30180
	c12=$v
	rd gpio_cmgp.gpm13_con 0x15c301a0
	c13=$v
	if [ -z "$gi" ] || [ -z "$gp" ] || ! gate_on "$gi" || ! gate_on "$gp"; then
		log "muic: a USI_CMGP3 gate is off; not loading the I2C driver"
		pm "stop: usi3 gate off"
		return 1
	fi
	rd gpio_cmgp.gpm12_dat 0x15c30184
	rd gpio_cmgp.gpm12_pud 0x15c30188
	rd gpio_cmgp.gpm13_dat 0x15c301a4
	rd gpio_cmgp.gpm13_pud 0x15c301a8
	# SW_CONF bit 2 is I2C; function 2 on both pins is the I2C one. bx5:
	# S-Boot leaves SW_CONF 0 and both pins plain outputs (function 1): it
	# bit-bangs this bus (the stock tree's gpio_scl/gpio_sda). So set them up
	# the way Samsung's kernel does at boot: usi_v2.c writes the mode, the
	# stock hsi2c45-bus pin group is function 2, no pull, drive 0.
	if [ $((mode & 4)) -eq 0 ] || [ $((c12 & 15)) -ne 2 ] || [ $((c13 & 15)) -ne 2 ]; then
		pm "setup usi3 as i2c"
		log "muic: setting USI_CMGP3 up as I2C (SW_CONF 4, gpm12-0/gpm13-0 function 2, no pull)"
		devmem 0x15c22030 32 4
		devmem 0x15c30188 32 $(( $(devmem 0x15c30188 32) & ~15 ))
		devmem 0x15c301a8 32 $(( $(devmem 0x15c301a8 32) & ~15 ))
		devmem 0x15c30180 32 $(( ($(devmem 0x15c30180 32) & ~15) | 2 ))
		devmem 0x15c301a0 32 $(( ($(devmem 0x15c301a0 32) & ~15) | 2 ))
		rd sysreg_cmgp.usi3_mode 0x15c22030
		mode=$v
		rd gpio_cmgp.gpm12_con 0x15c30180
		c12=$v
		rd gpio_cmgp.gpm13_con 0x15c301a0
		c13=$v
	fi
	if [ $((mode & 4)) -eq 0 ] || [ $((c12 & 15)) -ne 2 ] || [ $((c13 & 15)) -ne 2 ]; then
		log "muic: USI_CMGP3 is not set up as I2C; not loading the I2C driver"
		pm "stop: usi3 not i2c"
		return 1
	fi
	rd hsi2c.ctl 0x15d60000
	rd hsi2c.conf 0x15d60040
	rd hsi2c.usi_con 0x15d600c4
	# USI_RESET (bit 0) holds the pads until cleared; Samsung's driver
	# clears it at every init, mainline's USI driver would do it for us
	if [ -n "$v" ] && [ $((v & 1)) -eq 1 ]; then
		pm "clear usi_reset"
		devmem 0x15d600c4 32 0
		rd hsi2c.usi_con 0x15d600c4
	fi
	pm "insmod i2c-exynos5.ko"
	if ! insmod /lib/modules/zboot/i2c-exynos5.ko 2>/run/insmod.err; then
		log "muic: insmod i2c-exynos5.ko failed: $(cat /run/insmod.err)"
		return 1
	fi
	IBUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* 2>/dev/null | head -n1)
	IBUS=${IBUS##*/i2c-}
	if [ -z "$IBUS" ]; then
		log "muic: no I2C adapter for 15d60000.i2c"
		return 1
	fi
	log "muic: MAX77705 bus is i2c-$IBUS"
	ic 0x66 0x00	# PMICID1
	ic 0x66 0x01	# PMICREV
	ic 0x25 0x00	# UIC_HW_REV
	ic 0x25 0x01	# UIC_FW_REV
	ic 0x25 0x06	# USBC_STATUS1
	ic 0x25 0x07	# USBC_STATUS2
	ic 0x25 0x08	# BC_STATUS: bit 7 VBUS, bits 1:0 charger type (1 = SDP)
	ic 0x25 0x0a	# CC_STATUS0
	ic 0x25 0x0b	# CC_STATUS1
	ic 0x25 0x0c	# PD_STATUS0
	ic 0x25 0x0d	# PD_STATUS1
	ic 0x25 0x0e	# UIC_INT_M
	ctrl1
	if [ "$(arg zboot.muic)" = usb ]; then
		log "muic: CTRL1_W 0x09 (COMN1SW = COMP2SW = USB)"
		mbox CTRL1_W 0x06 0x09
		ctrl1
	fi
	pm "muic done $(cut -d' ' -f1 /proc/uptime)"
}

# bx3: the gadget bound but the host saw nothing at all. Once a second, what
# the PHY sees on the wire (HSP_TEST bits 21:20, LINESTATE: 0 SE0, 1 J, 2 K)
# and what the controller does (DCTL bit 31 RUN_STOP; DSTS bits 21:18 link
# state, 2:0 speed). Read-only, and only blocks the probe above found powered.
usb_watch() {
	i=0
	while [ $i -lt 15 ]; do
		sleep 1
		i=$((i + 1))
		l="$(cat /sys/class/udc/*/state 2>/dev/null)"
		for a in 0x10b00050 0x10b00054 0x10b0005c 0x10b00020 0x10b00004 \
			0x10c0c110 0x10c0c118 0x10c0c200 0x10c0c700 0x10c0c704 0x10c0c70c; do
			l="$l $(devmem $a 32 2>/dev/null)"
		done
		log "usb-watch $i: udc utmi hsp hsp_test clkrst linkctrl gctl gsts gusb2phycfg dcfg dctl dsts: $l"
		pm "w$i $l"
	done
}

pre_usb
