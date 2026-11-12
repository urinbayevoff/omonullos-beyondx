#!/bin/sh
# zpdoff: power down the Exynos 9820's blocks OmonullOS never uses, which
# S-Boot leaves on (and nothing here turns off: the kernel has no power
# domains for them): NPU, DSP, the camera's ISPs, IVA, MFC, G2D, VRA2.
# Each as Samsung's pmucal does it (flexpmu_cal_local_exynos9820.h *_off):
# CONFIGURATION bit 0 = 0, then wait for STATUS (CONFIGURATION + 4) to
# read 0. Children before parents (stock DT: NPU1 under NPU0, DSPS under
# DSPM). VTS (the voice trigger: its own CM4, DMIC and mailboxes) too:
# nothing here loads its firmware, the live DT has no node in its range and
# 9820's ABOX does not share the VTS SRAM (no "share vts sram" quirk).
# AUD, DPU, G3D and FSYS0A stay as they are.
#
# Added for OmonullOS, October 2026 (the owner said «ha», 2026-10-18 21:44):
# every domain was on, ~40 mA of the ~690 mA drawn with the screen off.
# VTS added November 2026 (stock vts_off is the same CONFIGURATION/STATUS
# pair; the rest of it only reads).

say() {
	echo "zboot| pdoff: $*" > /dev/kmsg 2>/dev/null
}

off() {	# off NAME CONFIGURATION (hex, no 0x: zpoke reads hex)
	st=$(printf %x $((0x$2 + 4)))
	[ "$(zpoke $st | cut -d' ' -f3)" = 00000000 ] && return
	zpoke $2 0 > /dev/null
	for i in $(seq 20); do
		[ "$(zpoke $st | cut -d' ' -f3)" = 00000000 ] && { gone="$gone $1"; return; }
		sleep 0.1
	done
	say "$1 did not power down"
}

gone=
off NPU1 15861d80
off NPU0 15861d00
off DSPS 15861f00
off DSPM 15861e80
off ISPHQ 15862400
off ISPLP 15862480
off ISPPRE 15862500
off IVA 15862580
off MFC 15862600
off G2D 15862300
off VRA2 15862b00
off VTS 15862a80
say "off:${gone:- none (already)}"
