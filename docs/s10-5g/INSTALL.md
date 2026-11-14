# Installing omonullOS on a Galaxy S10 5G (SM-G977N)

A release (`beyondx-v*` on the Releases page) has three files:

| file | what it is | partition |
|---|---|---|
| `bxci*.img` | BOOT: mainline Linux 7.1, initramfs, uniLoader, the S10 5G device tree | `BOOT` |
| `omonullos-beyondx-root.img.zst` | the root filesystem: ext4, label `ZR_ARCH`, 8 GiB, zstd | `USERDATA` |
| `SHA256SUMS` | checksums of the two | |

Built by `.github/workflows/build-image-beyondx.yml` from this tree only.
No firmware is in them: on the first boot `zvendor-firmware` copies the
Wi-Fi and audio firmware from the phone's own `VENDOR` partition (read only),
and the modem boots from the phone's own `RADIO2`. So `VENDOR` and `RADIO2`
must still be the stock ones (tested with G977NKSU6HWD3).

**This wipes Android.** USERDATA is overwritten: all of your Android data is
gone. Only the Korean SM-G977N (Exynos 9820) was tried. Other S10 models are
different phones.

## Before you start

1. Unlock the bootloader (OEM unlock), as for any custom ROM.
2. Flash TWRP into `RECOVERY`. It is your way back, see the end of this page.
3. **Back up `EFS` and keep the copy off the phone.** It holds the IMEI and
   the radio calibration. Nothing here writes to it, but you cannot get it back
   if it is lost. Back up `BOOT` too, so you can go back.
4. Check the downloads: `sha256sum -c SHA256SUMS`.

## Flashing

Unpack the root image and make it sparse. Download mode writes sparse images
and skips the empty part:

```sh
zstd -d omonullos-beyondx-root.img.zst
img2simg omonullos-beyondx-root.img root.simg     # android-tools / simg2img package
```

Put the phone in Download mode (power off, hold Vol Down + Bixby, plug in the
cable), then write the two partitions. We flash with
[samloader-rs](https://github.com/topjohnwu/samloader-rs) 2.2.0:
`samloader flash -p BOOT bxci*.img`, then `samloader flash -p USERDATA root.simg`.
Its stock timeouts are too short for USERDATA (see below); we raised the
session-end and chunk timeouts to 900 s. Odin and Heimdall have not been tried.

**Let the tool finish the session.** After the last data chunk, the phone
spends a few minutes writing out the empty end of USERDATA, and only then
closes the session and reboots by itself. Do not unplug, do not press any
keys, and give your tool a long enough timeout for that step. With a 3 s
timeout the session was cut short twice. Each time, the first ext4 superblock
was left as a placeholder (`00 01 02 ...` at offset 0x400) and the phone stopped at
`zboot: rescue shell: no filesystems labelled ZR_ARCH`.

## First boot

Takes longer than later boots: the firmware is copied out of `VENDOR` and the
filesystem grows from 8 GiB to the whole of USERDATA (`zboot-firstboot`). Then
the ztMETRO Start screen comes up, in Uzbek. Settings > language switches to English.

- User `alarm`, password `alarm` (as in Arch Linux ARM). Change it with `passwd`.
  `root` is locked; `alarm` is in `wheel` (sudo).
- SSH is installed but not enabled (`sudo systemctl enable --now sshd`); it
  takes keys only.
- Wi-Fi: Settings > Wi-Fi. Calls and SMS: a SIM in slot 1.
- Segoe fonts, Apple emoji, ztOS's wallpapers and ringtones are downloaded
  from Settings > theme (see FONTS.md); three wallpapers are built in.

What works and what does not: [STATUS.md](STATUS.md). Battery life is poor and
there is no suspend yet.

## If it does not boot

- **`rescue shell: no filesystems labelled ZR_ARCH`**: the superblock was
  not written (see above). Boot TWRP (Vol Down + Power ~7 s, then at once
  Vol Up + Bixby + Power; on our phone TWRP came up only with a USB cable
  plugged in). In TWRP's terminal, check first: `e2fsck -fn /dev/block/sda33`
  (`sda33` is userdata on our phone; check that it is on yours, e.g. with
  `ls -l /dev/block/platform/*/by-name/userdata`, before writing anything). If it says bad magic / superblock, repair from the backup superblock:
  `e2fsck -fy -b 32768 -B 4096 /dev/block/sda33`, then reboot to system.
- **Anything else**: flash your saved BOOT back, or stock firmware with Odin.
