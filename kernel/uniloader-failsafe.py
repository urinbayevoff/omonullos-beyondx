#!/usr/bin/env python3
"""Arm the way back in uniLoader's first board hook.

  uniloader-failsafe.py <board-starlte.c | board-beyondx.c> <INFORM3 value>

The port's kernel arms Samsung's reboot magic and the cluster-1 watchdog in a
postcore initcall, about 0.3 s into the boot. Everything before that (the
kernel copy, the early page tables, the first console) is unprotected: a hang
there leaves the phone on the logo until someone holds the buttons. These are
the same register writes, moved to the first C code that runs after sboot.

  PMU INFORM2 (0x14060808) = 0x12345678   "this was a reset", not a cold boot
  PMU INFORM3 (0x1406080c) = <value>      where sboot goes next
  PMU 0x408 / 0x40c bit 23 cleared        let WDT1 expiry reset the SoC
  WDT1 (0x10060000)                       30 s: 26 MHz / 256 / 128 = 793 Hz

Modified for OmonullOS, October 2026: the Galaxy S10 5G (board-beyondx.c).
Its PMU is at 0x15860000 with INFORM2/3 at the same offsets and the same
magic (Samsung's sec_reboot.c), and its watchdogs reset the SoC through a
cluster NONCPU_INT_EN bit that is set, not a mask bit that is cleared
(s3c2410_wdt.c, exynos9-v1/v2-wdt): bit 2 of CLUSTER0_NONCPU_INT_EN
(0x15861244) for the cluster-0 watchdog at 0x10050000, the cluster sboot
and uniLoader run on. Its clock is OSCCLK, 26 MHz, so the count is the
S9's. Untested on the phone.
"""
import sys
from pathlib import Path

path, inform3 = Path(sys.argv[1]), int(sys.argv[2], 0)
s = path.read_text()

if path.name == "board-beyondx.c":
    PMU, WDT = 0x15860000, 0x10050000
    route = [f"	*(volatile unsigned int *){PMU + 0x1244:#x} |= 1u << 2;"]
else:
    PMU, WDT = 0x14060000, 0x10060000
    route = [f"	*(volatile unsigned int *){PMU + 0x408:#x} &= ~(1u << 23);",
             f"	*(volatile unsigned int *){PMU + 0x40c:#x} &= ~(1u << 23);"]
anchor = """	/* Allow framebuffer to be written to */
	*(int*) (DECON_F_BASE + HW_SW_TRIG_CONTROL) = 0x1281;
"""
if s.count(anchor) != 1:
    sys.exit(f"uniloader-failsafe: anchor not found in {path.name}")
if "zboot failsafe" in s:
    sys.exit("uniloader-failsafe: already applied")
s = s.replace(anchor, f"""	/* zboot failsafe: fallback boot target, then the watchdog */
	*(volatile unsigned int *){PMU + 0x808:#x} = 0x12345678;
	*(volatile unsigned int *){PMU + 0x80c:#x} = {inform3:#x};
""" + "\n".join(route) + f"""
	*(volatile unsigned int *){WDT:#x} = 0;
	*(volatile unsigned int *){WDT + 4:#x} = 23805;
	*(volatile unsigned int *){WDT + 8:#x} = 23805;
	*(volatile unsigned int *){WDT:#x} = 0xff39;

""" + anchor)
path.write_text(s)
print(f"uniloader-failsafe: {path.name}: INFORM3={inform3:#x}, WDT {WDT:#x} armed for 30 s")
