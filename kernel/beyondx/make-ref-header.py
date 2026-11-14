#!/usr/bin/env python3
"""Write a boot header v1 that kernel/mkboot-v1.py can take as --ref when the
stock firmware (stock/firmware/HWD3/img/boot.img) is not at hand: on CI and
on the phone, since the PC that held it burned out.

  make-ref-header.py <out>

Added for OmonullOS, November 2026. mkboot-v1.py only reads the header of
its --ref and copies every field but the sizes, the command line and the id.
These are those fields as the stock G977NKSU6HWD3 boot.img has them, read
back from the bx35 image on the phone (mkboot-v1 copied them there).
Checked: bx35's kernel packed against this header gives bx35's sha256
(a97196144a04...0c2). No firmware bytes, only the numbers.
"""
import struct
import sys

FMT = "<8sIIIIIIIIII16s512s32s1024sIQI"	# boot_img_hdr_v1, as mkboot-v1.py
header = struct.pack(
    FMT,
    b"ANDROID!",
    0, 0x10008000,		# kernel size (set by mkboot-v1), load address
    0, 0x00000000,		# ramdisk size, address
    0, 0x00000000,		# second size, address
    0x10000100,			# tags
    2048,			# page size
    1,				# header version
    0x18000174,			# os_version / patch level
    b"SRPRK21D006KU",		# board name
    b"console=tty0",
    b"",			# id (mkboot-v1 recomputes it)
    b"",			# extra command line
    0, 0,			# recovery dtbo size, offset
    struct.calcsize(FMT),
)
with open(sys.argv[1], "wb") as f:
    f.write(header + b"\0" * (2048 - len(header)))
