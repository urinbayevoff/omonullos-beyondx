#!/bin/sh
# zbatt: the Galaxy S10 5G's battery and charger, read from the MAX77705
# over I2C. Read-only: no register of the fuel gauge or the charger is
# written (CLAUDE.md rule 5: battery and charger settings stay stock); only
# the AP's ADC is driven, for the NTC temperatures.
#
# Added for OmonullOS, October 2026. Needs the I2C bus that pre-usb.sh's
# muic step brings up (build-boot.sh --muic); installed as /bin/zbatt.
# Tested on the phone (2026-10-17, bx9, run from /tmp over telnet; the
# limits line on bx13).
#
# Why not the mainline drivers: max77705_charger rewrites a dozen charger
# settings at probe (timers, currents, input limits) and enables charging;
# max17042_battery (as max77705-battery) rewrites the gauge's CGAIN,
# MiscCFG and LearnCFG. Neither is checked against the stock values yet.
#
# Sources: Samsung's drivers/battery_v2/max77705_{fuelgauge,charger}.c and
# include/linux/battery/fuelgauge/max17050_fuelgauge.h; this phone's tree:
# fuelgauge,fg_resistor = 2 (5 mOhm sense resistor), capacity 4400 mAh.
#   gauge 0x36 (16-bit, little endian): 0x05 RepCap, 0x06 RepSOC, 0x08 Temp
#     (written by Samsung's driver from its thermistor: stale here), 0x09
#     VCell, 0x0a Current, 0x0b AvgCurrent, 0x10 FullCapRep, 0x17 Cycles,
#     0x18 DesignCap, 0x19 AvgVCell, 0x00 Status
#   charger 0x69: 0xb2 INT_OK, 0xb3-0xb5 DETAILS_00-02, 0xb7-0xc3 CNFG_00-12

BUS=$(ls -d /sys/bus/platform/devices/15d60000.i2c/i2c-* 2>/dev/null | head -n1)
BUS=${BUS##*/i2c-}
[ -n "$BUS" ] || { echo "zbatt: no I2C bus for 15d60000.i2c (needs --muic)" >&2; exit 1; }

r16() {	# r16 <reg>: unsigned 16-bit gauge register
	set -- $(i2ctransfer -y "$BUS" w1@0x36 "$1" r2 2>/dev/null)
	[ $# -eq 2 ] || { echo -1; return; }
	echo $(( ($2 << 8) | $1 ))
}

s16() {	# s16 <reg>: signed
	v=$(r16 "$1")
	[ "$v" -ge 32768 ] && v=$((v - 65536))
	echo "$v"
}

r8() {
	v=$(i2cget -y "$BUS" 0x69 "$1" 2>/dev/null)
	echo "${v:-?}"
}

soc=$(r16 0x06)
vcell=$(r16 0x09)
avgv=$(r16 0x19)
cur=$(s16 0x0a)
avgc=$(s16 0x0b)
temp=$(s16 0x08)
# VCell: 78.125 uV per LSB; current: 1.5625 uV / 5 mOhm = 0.3125 mA per LSB;
# capacities: 1 mAh per LSB at 5 mOhm; cycles: 1 % per LSB
echo "battery: $((soc >> 8)).$(( (soc & 255) * 10 / 256 ))%" \
	"$((vcell * 78125 / 1000000)) mV (avg $((avgv * 78125 / 1000000)) mV)" \
	"$((cur * 3125 / 10000)) mA (avg $((avgc * 3125 / 10000)) mA, + = charging)"
echo "capacity: $(r16 0x05) of $(r16 0x10) mAh (design $(r16 0x18) mAh)," \
	"cycles $(( $(r16 0x17) / 100 )), gauge temp $((temp / 256)) C (stale without Samsung's driver)," \
	"status $(printf 0x%04x "$(r16 0x00)")"

d0=$(r8 0xb3)
d1=$(r8 0xb4)
c0=$(r8 0xb7)
case $(( (d0 >> 5) & 3 )) in
	3) chgin="valid" ;;
	2) chgin="over voltage" ;;
	1) chgin="weak (below battery + headroom)" ;;
	*) chgin="none" ;;
esac
case $((d1 & 15)) in
	0|1|2) chg="charging (dtls $((d1 & 15)))" ;;
	3|4) chg="full / top-off (dtls $((d1 & 15)))" ;;
	5|6|7) chg="not charging (dtls $((d1 & 15)))" ;;
	8|10|11) chg="off (dtls $((d1 & 15)))" ;;
	*) chg="dtls $((d1 & 15))" ;;
esac
echo "charger: input $chgin, $chg, battery dtls $(( (d1 >> 4) & 7 ))," \
	"mode $((c0 & 15)), charger watchdog $( [ $(( (c0 >> 4) & 1 )) -eq 1 ] && echo on || echo off)"
# Limits as Samsung's driver reads them: CNFG_09 (0xc0) CHGIN_ILIM bits 6:0,
# (code + 1) * 25 mA, 100 mA at 0-3; CNFG_02 (0xb9) CHG_CC bits 5:0, 50 mA
# per step, 100 mA at 0-2. Whatever S-Boot set for the cable it saw at boot.
ilim=$(( $(r8 0xc0) & 0x7f ))
cc=$(( $(r8 0xb9) & 0x3f ))
[ $ilim -le 3 ] && ilim=100 || ilim=$(( (ilim + 1) * 25 ))
[ $cc -le 2 ] && cc=100 || cc=$((cc * 50))
echo "limits: input $ilim mA, charge $cc mA (set by S-Boot)"
# NTC temperatures through the AP's ADC (0x15c40000, exynos-adc-v3, the
# CMGP block S-Boot leaves running): the battery (channel 4), the charger
# (3) and the USB port (1), as Samsung's sec_battery reads them (io-channels
# adc-temp, adc-chg-temp, adc-usb-temp). The only writes in this script go
# to that ADC: CON2's channel select and CON1's start bit, then INT_ST to
# clear (Samsung's exynos_adc.c v2/v3 conversion). Tables from the stock
# tree (battery,*_table_adc/_data, 0.1 C): linear between points, as
# sec_battery does.
ADC=0x15c40000
adc() {	# adc <channel>: 12-bit raw value, or -1 (/run/adc.lock: zchg)
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
ntc() {	# ntc <raw> <adc table> <temp table>: 0.1 C, tables low ADC first
	set -- "$1" $2 -- $3
	raw=$1; shift
	n=0; for x; do [ "$x" = -- ] && break; n=$((n + 1)); done
	i=0; pa=; pt=
	for x; do
		[ "$x" = -- ] && break
		eval "ta=\${$((n + 2 + i))}"
		if [ "$raw" -le "$x" ]; then
			[ -z "$pa" ] && { echo "$ta"; return; }
			echo $(( pt + (ta - pt) * (raw - pa) / (x - pa) ))
			return
		fi
		pa=$x; pt=$ta; i=$((i + 1))
	done
	echo "$pt"
}
T_DATA="900 850 800 750 700 650 600 550 500 450 400 350 300 250 200 150 100 50 0 -50 -100 -150 -200"
T_BAT="293 343 404 474 557 652 764 893 1040 1207 1390 1594 1812 2041 2275 2511 2738 2951 3135 3321 3463 3592 3693"
T_CHG="275 325 388 457 538 633 744 872 1009 1184 1366 1570 1786 2014 2248 2483 2710 2916 3111 3294 3443 3560 3672"
T_USB="293 343 409 479 562 657 770 899 1045 1212 1395 1598 1815 2042 2275 2507 2733 2945 3139 3301 3442 3566 3677"
c10() { [ "$1" -lt 0 ] && echo "-$(( -$1 / 10 )).$(( -$1 % 10 ))" || echo "$(($1 / 10)).$(($1 % 10))"; }
tb=$(adc 4); tc=$(adc 3); tu=$(adc 1)
echo "ntc: battery $(c10 "$(ntc "$tb" "$T_BAT" "$T_DATA")") C, charger $(c10 "$(ntc "$tc" "$T_CHG" "$T_DATA")") C," \
	"usb port $(c10 "$(ntc "$tu" "$T_USB" "$T_DATA")") C (adc $tb $tc $tu)"
if [ "$1" = -v ]; then
	l="charger regs:"
	for r in 0xb2 0xb3 0xb4 0xb5 0xb7 0xb8 0xb9 0xba 0xbb 0xbc 0xbd 0xbe 0xbf 0xc0 0xc1 0xc2 0xc3; do
		l="$l $r=$(r8 $r)"
	done
	echo "$l"
fi
