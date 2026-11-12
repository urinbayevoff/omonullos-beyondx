#!/usr/bin/env python3
"""Add the Galaxy S10 5G (beyondx) to a copy of uniLoader.

  uniloader-board.py <uniLoader copy>

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


def insert_after(path, anchor, text):
    s = path.read_text()
    if s.count(anchor) != 1:
        sys.exit(f"uniloader-board: expected 1x {anchor!r} in {path}, found {s.count(anchor)}")
    path.write_text(s.replace(anchor, anchor + text))


shutil.copy(here / "board-beyondx.c", stage / "board/samsung/board-beyondx.c")
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
print("uniloader-board: beyondx added")
