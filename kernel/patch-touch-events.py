#!/usr/bin/env python3
"""Read the left-event count of an s6sy761/771 event from 5 bits, not 6.

Added for OmonullOS, October 2026. Samsung's S6SY771 driver (sec_ts/y771
sec_ts.h, struct sec_ts_event_coordinate) has byte 7 as left_event:5,
max_energy_flag:1, ttype_1_0:2. Mainline masks 0x3f, so on the Galaxy S10 5G
every event with max_energy_flag set read as 32+ left events and the whole
batch was dropped, releases included: contacts never ended (bx24). The
driver accepts at most 31 left events, so 5 bits lose nothing on the 761.
"""
from pathlib import Path
import sys

p = Path(sys.argv[1]) / 'drivers/input/touchscreen/s6sy761.c'
s = p.read_text()
old = '#define S6SY761_MASK_LEFT_EVENTS\t0x3f /* byte 7 */'
new = '#define S6SY761_MASK_LEFT_EVENTS\t0x1f /* byte 7; bit 5: max energy (zboot) */'
if new not in s:
    if s.count(old) != 1:
        sys.exit('patch-touch-events: S6SY761_MASK_LEFT_EVENTS not found as expected')
    p.write_text(s.replace(old, new))
