#!/usr/bin/env python3
"""Add the Galaxy S10 5G (beyondx) to a copy of uniLoader.

  uniloader-board.py <uniLoader copy> [--menu]

--menu (November 2026, dual boot): also copies bootmenu-bx.c and defines
ZBOOT_MENU in the board file: a key-driven menu (OmonullOS / TWRP) before
the kernel boots.

Added for OmonullOS, October 2026. Copies board-beyondx.c and
beyondx_defconfig in, and adds the board to board/Kconfig and
board/Makefile next to beyond1lte. Every edit is asserted, so a changed
upstream fails here instead of building the wrong board.
"""
import shutil
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
stage = Path(sys.argv[1])
menu = sys.argv[2:] == ["--menu"]
if sys.argv[2:] and not menu:
    sys.exit("usage: uniloader-board.py <uniLoader copy> [--menu]")


def insert_after(path, anchor, text):
    s = path.read_text()
    if s.count(anchor) != 1:
        sys.exit(f"uniloader-board: expected 1x {anchor!r} in {path}, found {s.count(anchor)}")
    path.write_text(s.replace(anchor, anchor + text))


shutil.copy(here / "board-beyondx.c", stage / "board/samsung/board-beyondx.c")
if menu:
    shutil.copy(here / "bootmenu-bx.c", stage / "board/samsung/bootmenu-bx.c")
    board = stage / "board/samsung/board-beyondx.c"
    b = board.read_text()
    if b.count("#include <board.h>\n") != 1:
        sys.exit("uniloader-board: #include <board.h> not found once in board-beyondx.c")
    board.write_text(b.replace("#include <board.h>\n", "#define ZBOOT_MENU 1\n#include <board.h>\n"))
shutil.copy(here / "beyondx_defconfig", stage / "configs/beyondx_defconfig")
insert_after(stage / "board/Kconfig", """		  Say Y if you want to include support for Samsung Galaxy S10
""", """
	config SAMSUNG_BEYONDX
		bool "Support for Samsung Galaxy S10 5G"
		default n
		depends on EXYNOS_9820
		help
		  Say Y if you want to include support for Samsung Galaxy S10 5G
""")
insert_after(stage / "board/Makefile", "lib-$(CONFIG_SAMSUNG_BEYOND1LTE) += samsung/board-beyond1lte.o\n",
             "lib-$(CONFIG_SAMSUNG_BEYONDX) += samsung/board-beyondx.o\n")
print("uniloader-board: beyondx added" + (", with the boot menu" if menu else ""))
