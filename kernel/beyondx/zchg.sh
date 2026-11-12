#!/bin/sh
# zchg: charge the Galaxy S10 5G at the stock TA rate from a wall charger,
# guarded by the battery's own temperature, until a real charger driver is
# in. Owner's «ha» for these charger writes, 2026-10-17 (CLAUDE.md rule 5:
# stock values only).
#
# S-Boot sets the MAX77705's limits for the cable it sees at boot (from the
# PC: input 500 mA, charge 450 mA) and nothing changes them afterwards, so a
# wall charger got the same ~2.5 W (zcharge, 2026-10-17). Every 3 s this
# reads the cable type (USB-C side 0x25:0x08 BC_STATUS) and the NTCs (the
# AP's ADC, as zbatt) and sets:
#
#   DCP (CHGTYP 3), or a USB-C charger that offers 3 A on CC (CC_STATUS0
#     0x25:0x0a CCIStat bits 5:4 = 3, as max77705_cc.c reads it; a PD
#     charger on a C-C cable fails BC1.2 with DCD timeout and reads as SDP,
#     BC_STATUS 0x85, 2026-10-18; owner asked for the TA rate there too),
#     with VBUS, temperatures fine: the stock tree's default
#     (TA) row, input 1800 mA and charge 2100 mA: CNFG_09 (0xc0) CHGIN_ILIM
#     = 1800 / 25 - 1 = 0x47, CNFG_02 (0xb9) CHG_CC = 2100 / 50 = 0x2a, as
#     Samsung's max77705_set_input/charge_current do, other bits kept
#   a PC port (SDP, CHGTYP 1, or CDP, 2), temperatures fine: input 1000 mA,
#     charge 1000 mA (CNFG_09 0x27, CNFG_02 0x14). Not a stock value (stock
#     SDP is 500 mA); owner's «ha» 2026-10-18: with the modems up the phone
#     drew ~610 mA and lost ~110 mA a minute on S-Boot's 500 mA from the PC
#     (ASUS H310 ports are not limited to 500 mA). The charger's own input
#     voltage regulation backs off if the port sags.
#   anything else (no cable, unknown), battery >= 42 C (stock lowers at 46),
#     charger or USB-port NTC >= 60 C, or an NTC that cannot be read: back to
#     the values S-Boot left (read at start); the TA rate returns only once
#     the battery is below 40 C and the others below 55 C
#
# AFC (Samsung's Adaptive Fast Charging, 9 V), owner's «ha» 2026-10-17: once
# per plug-in, at the TA rate with the battery below 38 C, the input limit
# goes to 1650 mA first, then the USB-C firmware is asked for 9 V the way
# max77705-muic-afc.c does (mailbox opcode 0x21 COMMAND_AFC_RESULT_READ,
# tx byte 0x46; answer at 0x51: opcode, result 0 = success, VBADC). Only if
# USBC_STATUS1's VBADC then reads 7.5-10.5 V does the charge current go to
# the stock 9V_TA row's 3150 mA (input 1650). Hot, or VBUS back at 5 V: ask
# for 5 V (tx 0x08) and drop to the TA or S-Boot values. zboot.afc=off
# keeps it at the TA rate.
#
# Writes only CNFG_09/CNFG_02, only when they differ, and reads them back;
# plus the AFC mailbox commands. zboot.chg=off leaves zchg out (wdt-pet.sh).
#
# Added for OmonullOS, October 2026. Tested on the phone (bx18) on the
# owner's PD charger: +1.2-1.3 A into the battery, back to S-Boot's values
# within 3 s of unplugging.

ADC=0x15c40000
TA_ILIM=0x47 TA_CC=0x2a PC_ILIM=0x27 PC_CC=0x14 AFC_ILIM=0x41 AFC_CC=0x3f
AFC_TRY=380
AFC=1; grep -q 'zboot.afc=off' /proc/cmdline && AFC=
BAT_HOT=420 BAT_OK=400 OTHER_HOT=600 OTHER_OK=550

say() { echo "zboot| chg: $*" > /dev/kmsg 2>/dev/null; }
# started from /etc/zboot/early, before pre-usb.sh has loaded the I2C bus
k=0
while :; do
	BUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* 2>/dev/null | head -n1)
	BUS=${BUS##*/i2c-}
	[ -n "$BUS" ] && break
	k=$((k + 1)); [ $k -gt 120 ] && { say "no I2C bus after 120 s, not running"; exit 1; }
	sleep 1
done

r8() { v=$(i2cget -y "$BUS" "$1" "$2" 2>/dev/null) && echo $((v)) || echo -1; }

# the stock tree's NTC tables (battery,temp_table_*, chg_ and usb_; 0.1 C)
T_DATA="900 850 800 750 700 650 600 550 500 450 400 350 300 250 200 150 100 50 0 -50 -100 -150 -200"
T_BAT="293 343 404 474 557 652 764 893 1040 1207 1390 1594 1812 2041 2275 2511 2738 2951 3135 3321 3463 3592 3693"
T_CHG="275 325 388 457 538 633 744 872 1009 1184 1366 1570 1786 2014 2248 2483 2710 2916 3111 3294 3443 3560 3672"
T_USB="293 343 409 479 562 657 770 899 1045 1212 1395 1598 1815 2042 2275 2507 2733 2945 3139 3301 3442 3566 3677"

adc() {	# adc <channel>: raw, or -1; /run/adc.lock keeps zbatt out meanwhile
	k=0
	until mkdir /run/adc.lock 2>/dev/null; do
		k=$((k + 1)); [ $k -gt 20 ] && { echo -1; return; }; sleep 0.1
	done
	c2=$(devmem $((ADC + 4)) 32)
	devmem $((ADC + 4)) 32 $(( (c2 & ~0xf) | $1 ))
	devmem $ADC 32 $(( $(devmem $ADC 32) | 1 ))
	i=0
	while [ $i -lt 100 ]; do
		d=$(devmem $((ADC + 8)) 32)
		[ $((d >> 31)) -eq 1 ] && break
		i=$((i + 1))
	done
	devmem $((ADC + 0x14)) 32 1
	rmdir /run/adc.lock
	[ $i -lt 100 ] && echo $((d & 0xfff)) || echo -1
}

ntc() {	# ntc <raw> <adc table>: 0.1 C, or 9999 when off the table
	raw=$1
	[ "$raw" -ge 0 ] || { echo 9999; return; }
	set -- $2
	[ "$raw" -ge "$1" ] || { echo 9999; return; }
	i=0; pa=; pt=
	for x; do
		ta=$(echo "$T_DATA" | cut -d' ' -f$((i + 1)))
		if [ "$raw" -le "$x" ]; then
			[ -z "$pa" ] && { echo "$ta"; return; }
			echo $(( pt + (ta - pt) * (raw - pa) / (x - pa) ))
			return
		fi
		pa=$x; pt=$ta; i=$((i + 1))
	done
	echo 9999
}

setreg() {	# setreg <reg> <mask> <value>: update bits, read back
	c=$(r8 0x69 "$1")
	[ "$c" -ge 0 ] || return 1
	n=$(( (c & ~$2) | $3 ))
	[ "$n" -eq "$c" ] && return 0
	i2cset -y "$BUS" 0x69 "$1" "$n" 2>/dev/null
	[ "$(r8 0x69 "$1")" -eq "$n" ]
}

limits() {	# limits <ilim code> <cc code> <why>
	if setreg 0xc0 0x7f "$1" && setreg 0xb9 0x3f "$2"; then
		say "input $(( ($1 + 1) * 25 )) mA, charge $(( $2 * 50 )) mA: $3"
	else
		say "writing the limits failed ($3)"
	fi
}

# S-Boot's values are read once per boot and kept in /run, which the root
# system inherits: zchg starts again there (zboot-guards), when the
# registers may already hold the TA rate, and must not take that for the
# values to fall back to on a PC port.
SAVED=/run/zboot.chg-sboot
if [ -s "$SAVED" ]; then
	read -r s9 s2 < "$SAVED"
else
	s9=$(r8 0x69 0xc0); s2=$(r8 0x69 0xb9)
fi
# a failed read must never become a limit (-1 & 0x7f is 3200 mA)
[ "$s9" -ge 0 ] && [ "$s2" -ge 0 ] || { say "cannot read CNFG_09/02, not running"; exit 1; }
[ -s "$SAVED" ] || echo "$s9 $s2" > "$SAVED"
S_ILIM=$((s9 & 0x7f)) S_CC=$((s2 & 0x3f))

vbadc() { v=$(r8 0x25 0x06); [ "$v" -ge 0 ] && echo $((v >> 4)) || echo 0; }

afc() {	# afc <tx byte>: one AFC request, as pre-usb.sh's mbox; prints the answer
	i2cget -y "$BUS" 0x25 0x02 >/dev/null 2>&1	# UIC_INT: clear
	i2ctransfer -y "$BUS" w3@0x25 0x21 0x21 "$1" 2>/dev/null
	i2cset -y "$BUS" 0x25 0x41 0x00 2>/dev/null
	i=0
	while [ $i -lt 150 ]; do	# AFC takes its time: up to 3 s
		u=$(r8 0x25 0x02)
		[ "$u" -ge 0 ] && [ $((u & 0x80)) -ne 0 ] && break
		i=$((i + 1)); sleep 0.02
	done
	i2ctransfer -y "$BUS" w1@0x25 0x51 r3 2>/dev/null
}
say "on; S-Boot left input code $S_ILIM, charge code $S_CC"

mode=sboot
while :; do
	bc=$(r8 0x25 0x08)
	tb=$(ntc "$(adc 4)" "$T_BAT")
	tc=$(ntc "$(adc 3)" "$T_CHG")
	tu=$(ntc "$(adc 1)" "$T_USB")
	other=$tc; [ "$tu" -gt "$other" ] && other=$tu
	cc=$(r8 0x25 0x0a)
	dcp=0 pc=0
	if [ "$bc" -ge 0 ] && [ $((bc >> 7)) -eq 1 ]; then
		# PD 3.0 sources flip Rp 3 A <-> 1.5 A (SinkTxOk/NG) around their
		# messages: 1.5 A keeps the TA rate once it is set, 3 A starts it
		rp=0; [ "$cc" -ge 0 ] && rp=$(((cc >> 4) & 3))
		if [ $((bc & 3)) -eq 3 ] || [ $rp -eq 3 ] ||
		   { [ $rp -eq 2 ] && { [ "$mode" = ta ] || [ "$mode" = afc ]; }; }; then
			dcp=1
		elif [ $((bc & 3)) -ge 1 ]; then
			pc=1
		fi
	fi
	if [ "$mode" = afc ]; then
		vb=$(vbadc)
		if [ $dcp -eq 0 ]; then
			limits $S_ILIM $S_CC "no wall charger (BC_STATUS $bc)"; mode=sboot; tried=
		elif [ "$tb" -ge $BAT_HOT ] || [ "$other" -ge $OTHER_HOT ]; then
			limits $S_ILIM $S_CC "hot at 9 V: battery $tb, charger/usb $other (0.1 C)"
			say "afc 5V: $(afc 0x08), vbadc $(vbadc)"; mode=hot
		elif [ "$vb" -lt 5 ]; then
			limits $TA_ILIM $TA_CC "VBUS back at 5 V (vbadc $vb)"; mode=ta
		fi
	elif [ "$mode" = ta ]; then
		if [ $dcp -eq 0 ]; then
			limits $S_ILIM $S_CC "no wall charger (BC_STATUS $bc)"; mode=sboot; tried=
		elif [ "$tb" -ge $BAT_HOT ] || [ "$other" -ge $OTHER_HOT ]; then
			limits $S_ILIM $S_CC "hot: battery $tb, charger/usb $other (0.1 C)"; mode=hot
		elif [ -n "$AFC" ] && [ -z "$tried" ] && [ "$tb" -lt $AFC_TRY ] && [ "$other" -lt $OTHER_OK ]; then
			tried=1
			limits $AFC_ILIM $TA_CC "before asking for 9 V"
			a=$(afc 0x46); sleep 1; vb=$(vbadc)
			say "afc 9V: answer $a, vbadc $vb"
			if [ "$vb" -ge 5 ] && [ "$vb" -le 7 ]; then
				limits $AFC_ILIM $AFC_CC "AFC 9 V"; mode=afc
			else
				[ "$vb" -gt 7 ] && say "afc: vbadc $vb is above 10.5 V, asking 5 V: $(afc 0x08)"
				limits $TA_ILIM $TA_CC "no AFC, staying at 5 V"
			fi
		fi
	elif [ "$mode" = pc ]; then
		if [ $pc -eq 0 ]; then
			limits $S_ILIM $S_CC "no PC port (BC_STATUS $bc)"; mode=sboot; tried=
		elif [ "$tb" -ge $BAT_HOT ] || [ "$other" -ge $OTHER_HOT ]; then
			limits $S_ILIM $S_CC "hot on a PC port: battery $tb, charger/usb $other (0.1 C)"; mode=hot
		fi
	else
		if [ $dcp -eq 1 ] && [ "$tb" -lt $BAT_OK ] && [ "$other" -lt $OTHER_OK ]; then
			limits $TA_ILIM $TA_CC "wall charger, battery $tb, charger/usb $other (0.1 C)"; mode=ta
		elif [ $pc -eq 1 ] && [ "$tb" -lt $BAT_OK ] && [ "$other" -lt $OTHER_OK ]; then
			limits $PC_ILIM $PC_CC "PC port, battery $tb, charger/usb $other (0.1 C)"; mode=pc
		elif [ "$mode" = hot ] && [ $dcp -eq 0 ] && [ $pc -eq 0 ]; then
			mode=sboot; tried=
		fi
	fi
	# a line a minute, for the log
	[ $((t = ${t:-0} + 1)) -ge 20 ] && { say "$mode, BC_STATUS $bc, CC_STATUS0 $cc, battery $tb, charger $tc, usb $tu (0.1 C)"; t=0; }
	sleep 3
done
