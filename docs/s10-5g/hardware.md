# Galaxy S10 5G (SM-G977N) — hardware

Sources, 2026-10-16:
- the phone over `adb` (stock Android 12, no root): `getprop`, the driver
  names the stock kernel registered, the live device tree's node names
  (their properties need root);
- the stock firmware `G977NKSU6HWD3` (stock/firmware/HWD3): `dt.img` (one
  SoC tree) and `dtbo.img` (board overlays 00–08), decompiled with `dtc`;
- LineageOS `android_kernel_samsung_exynos9820` (lineage-23.2) in
  third_party/vendor-kernel/, until Samsung's own source is downloaded.

This phone boots **dtbo index 8** (`ro.boot.dtbo_idx=8`, board revision 9:
`ro.boot.revision=9`), overlay "BEYONDX KOR SINGLE 08". Nothing below is
tested on mainline yet.

| | S10 5G (beyondx) | S9 (starlte) equivalent | Notes |
|---|---|---|---|
| Codename | `beyondx` (product `beyondxks`) | `starlte` | |
| SoC | Exynos 9820: 2x Mongoose M4 (part 0x003), 2x Cortex-A75, 4x Cortex-A55 | Exynos 9810 (4x M3 + 4x A55) | three clusters, not two |
| RAM | 8 GB (MemTotal 7.1 GiB). Stock DT lists 3 x 2 GB at 0x80000000, 0x880000000, 0x900000000; S-Boot fills in the rest | 4 GB | |
| Stock firmware | `G977NKSU6HWD3` (Android 12, patch 2023-04), BL rev **6**; server latest is HXJ2 | | |
| Baseband | `G977NKOU6HWD1` | | |
| Stock kernel | 4.14.113; boot.img header v1, page 2048, cmdline `androidboot.selinux=permissive loop.max_part=7` | | |
| Storage | UFS (`exynos-ufs`), LUNs sda–sde | UFS 2.1 | |
| Partitions | non-A/B, no dynamic partitions (launched on API 28) | | list below |
| PMIC | S2MPS19 + S2MPS20 (`s2mps19mfd`, `s2mps20mfd`), S2MPB02, S2MPB03, S2DOS05 | S2MPS18/19 | |
| Charger / fuel gauge | MAX77705 (charger, fuel gauge, USB-C, haptic) + NXP PCA9468 direct charger + IDT MFC wireless | MAX77705 | reuse likely |
| Display | DECON-F at 0x19030000 (max_win 6, default_win 5), DSIM at 0x19080000, `samsung,panel-drv` (panel picked by `panel-lookup` in the overlay) | DECON, S6E3HA8 | 1440x3040 |
| Framebuffer | added by S-Boot at boot as reserved memory `bootloader_fb` (`sboot,fb`); **address not confirmed** | | uniLoader's beyond1lte uses 0xca000000; read the live `/sys/firmware/fdt` once rooted |
| Touch | `sec,sec_ts` (I2C) | `sec_ts` (S6SY761) | reuse likely |
| Audio codec | Cirrus **CS47L93** (`cirrus,cs47l93`, Madera), `samsung,beyond-madera`, ABOX | CS47L35 (Madera) | same family |
| Speaker amps | Cirrus **CS35L40** (DT; driver `cs35l41`) | | new |
| Wi-Fi / BT | Broadcom BCM4375 (`samsung,brcm-wlan`, `dhd`, PCIe), BT `samsung,bcm43xx`, FW `BCM4375B1 Beyond X5G` | BCM4361 | |
| Sensor hub | SSP on Broadcom **BCM4773** (`ssp,BCM4773`, SPI) | SSP | |
| Modems | Shannon LTE (`exynos,modem_if`, shared memory) + **S5100 5G** over PCIe (`exynos,s5100-msi`, `modem_boot_spi`, `exynos,modem_if2`); partitions `radio` + `radio2` | Shannon | two modems |
| Fingerprint | Qualcomm QBT2000 ultrasonic (SPI) | capacitive | later |
| Cameras | S5K2L4, S5K3J1, S5K3M3, S5K3P9, IMX316 (ToF) | | later |
| Other | NFC (`sec-nfc`), MST, eSE (P3), SX9330 grip, TCS3407 light, hall, FM radio RTC6213N, TDMB | | later |

## Mainline 7.1 starting points

- DT: `exynos9810*.dtsi`, `exynos9810-starlte.dts`, `exynos990*.dtsi` and
  three Exynos 990 boards. **No Exynos 9820.**
- Clocks: `clk-exynos990.c` (and 8895, gs101); no 9810 in mainline, the S9
  port carries its own (third_party/s9plus/src). Pinctrl:
  `pinctrl-exynos-arm64.c` has the 9810 and 990 banks.

## For the first mainline tree (from the stock DT)

- CPUs, all `enable-method = "psci"`: cpu@0000–0003 Cortex-A55
  (`arm,ananke`, MPIDR 0x0–0x3), cpu@0004–0005 Cortex-A75
  (`arm,prometheus`, MPIDR 0x4–0x5), cpu@0100–0101 Mongoose M4
  (`arm,cheetah`, MPIDR 0x100–0x101). The A55s and A75s share affinity 1 = 0
  (DynamIQ), unlike the 9810.
- GIC: GIC-400 style (`arm,cortex-a15-gic`) at 0x10100000, distributor
  0x10101000, CPU interface 0x10102000. Timer `arm,armv8-timer`.
- chipid 0x10000000 (stock says `samsung,exynos9810-chipid`).
- Pinctrl banks (`samsung,exynos9820-pinctrl`): 0x15850000, 0x18c60000,
  0x15c30000, 0x13030000, 0x13c40000, 0x10430000, 0x10830000, 0x15580000.
- UARTs 0x10440000 … 0x10900000 (12); no serial console reachable on this
  phone without opening it, so early output is the screen.
- UFS 0x13d60000 (+0x13d61100, 0x13d50000 …). CMU top at 0x1a240000
  (`samsung,exynos9820-clock`).

## Partitions (`/dev/block/by-name`)

boot sda14, recovery sda15, dtb sda12, dtbo sda13, vbmeta sda24,
system sda26, vendor sda27, product sda28, cache sda29, userdata sda33,
radio sda17, radio2 sda16, param sda4, up_param sda5, misc sda20, omr sda30,
**efs sda3, sec_efs sda7, cpefs sdd1 — never written (CLAUDE.md rule 1)**,
keystorage sda6, keyrefuge sda21, keydata sda22, persistent sda19,
nad_refer sda18, dqmdbg sda23, cp_debug sda31, cp2_debug sda32,
steady sda9, uh sda10, uhcfg sda11, bota0-3 sda1/sda2/sda8/sda25.

Firmware images in the AP tar: boot, recovery, dt, dtbo, system, vendor,
vbmeta, dqmdbg. In BL: sboot, param, up_param, cm, keystorage, uh, vbmeta.
