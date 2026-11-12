# UFS host for the Exynos 9820 (Galaxy S10 5G)

- `exynos9820-ufs.c`: host glue, the S9+ port's `ufs-exynos9810.c` moved to
  the 9820 (what changed and why is in its header comment).
- `ufs-cal-9820.c`, `ufs-cal-9820.h`: Samsung's PHY/UniPro calibration,
  copied **unchanged** from the SM-G977N kernel
  (`drivers/scsi/ufs/`, GPL-2.0), as the S9 port did with `ufs-cal-9810`.
- `Kbuild`: built as `ufs-exynos9820.ko` by `scripts/build-boot.sh --ufs`.

Loaded by hand from the rescue shell with `/bin/zufs` (kernel/beyondx/zufs.sh),
which checks that BLK_FSYS1 is clocked first and makes every disk read-only.
