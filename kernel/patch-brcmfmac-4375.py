#!/usr/bin/env python3
"""brcmfmac: the BCM4375 (Galaxy S10 5G Wi-Fi, PCIe) (zboot).

Added for OmonullOS, October 2026. Mainline brcmfmac knows the 4375's
siblings (43752, 4377, 4378) but not the 4375 itself. Samsung's bcmdhd
(bcmdhd_101_16, the stock driver for this phone) treats it like them:
dongle RAM base 0x170000 (CR4_4369_RAM_BASE, the value mainline uses for
43752/4377), a random seed written after the NVRAM for every CR4 chip
(dhdpcie_wrt_rnd, mainline's WCC_SEED), no MSI (INTx; the host has none).
The firmware (pcie_shared v7) also needs pcie_shared.host_cap, which
brcmfmac never writes: with it 0, control and events work but no data
frame ever reaches the host (rx completion ring never written, EAPOL
lost, WPA times out); written like bcmdhd's dhd_set_host_cap, the link
works (docs/s10-5g/log.md 2026-10-17).
PCI device ID 0x4475 (bcmdevs.h BCM4375_D11AX_ID), with the 2G/5G-only IDs.
Firmware brcm/brcmfmac4375-pcie.{bin,txt,clm_blob}: the phone's own
bcmdhd_sta.bin_b1, nvram.txt_CS01_semco_b1 and bcmdhd_clm.blob from VENDOR.
"""
from pathlib import Path
import sys

base = Path(sys.argv[1]) / 'drivers/net/wireless/broadcom/brcm80211'


def edit(rel, pairs):
    p = base / rel
    s = p.read_text()
    if 'zboot' in s:
        return
    for anchor, new in pairs:
        if s.count(anchor) != 1:
            sys.exit(f'patch-brcmfmac-4375: {rel}: anchor not found once: {anchor!r}')
        s = s.replace(anchor, new)
    p.write_text(s)


edit('include/brcm_hw_ids.h', [
    ('#define BRCM_CC_4377_CHIP_ID\t\t0x4377\n',
     '#define BRCM_CC_4375_CHIP_ID\t\t0x4375\t/* zboot: Galaxy S10 5G */\n'
     '#define BRCM_CC_4377_CHIP_ID\t\t0x4377\n'),
    ('#define BRCM_PCIE_4377_DEVICE_ID\t0x4488\n',
     '#define BRCM_PCIE_4375_DEVICE_ID\t0x4475\t/* zboot */\n'
     '#define BRCM_PCIE_4375_2G_DEVICE_ID\t0x4476\n'
     '#define BRCM_PCIE_4375_5G_DEVICE_ID\t0x4477\n'
     '#define BRCM_PCIE_4377_DEVICE_ID\t0x4488\n'),
])

edit('brcmfmac/chip.c', [
    ('\tcase BRCM_CC_43752_CHIP_ID:\n\tcase BRCM_CC_4377_CHIP_ID:\n\t\treturn 0x170000;\n',
     '\tcase BRCM_CC_43752_CHIP_ID:\n\tcase BRCM_CC_4375_CHIP_ID:\t/* zboot */\n'
     '\tcase BRCM_CC_4377_CHIP_ID:\n\t\treturn 0x170000;\n'),
])

edit('brcmfmac/pcie.c', [
    ('\tbrcmf_pcie_bus_console_init(devinfo);\n\tbrcmf_pcie_bus_console_read(devinfo, false);\n\n\treturn 0;\n}\n',
     "\t/*\n\t * zboot: v6+ firmware reads the host's IPC version and capabilities\n\t * from pcie_shared.host_cap (offset 84), which brcmfmac leaves 0.\n\t * Samsung's bcmdhd (dhd_set_host_cap) writes it before the rings are\n\t * used; the 4375 gets the same: IPC = shared version, HOSTRDY, no OOB\n\t * device wake, no trap on UR (no extended trap data: no buffer given).\n\t */\n\tif (devinfo->ci->chip == BRCM_CC_4375_CHIP_ID && shared->version >= 6)\n\t\tbrcmf_pcie_write_tcm32(devinfo, sharedram_addr + 84,\n\t\t\t\t       shared->version | 0x400 | 0x1000 | 0x800000);\n\n\tbrcmf_pcie_bus_console_init(devinfo);\n\tbrcmf_pcie_bus_console_read(devinfo, false);\n\n\treturn 0;\n}\n"),
    ('BRCMF_FW_CLM_DEF(43752, "brcmfmac43752-pcie");\n',
     'BRCMF_FW_CLM_DEF(43752, "brcmfmac43752-pcie");\n'
     'BRCMF_FW_CLM_DEF(4375, "brcmfmac4375-pcie");\t/* zboot */\n'),
    ('\tBRCMF_FW_ENTRY(BRCM_CC_43752_CHIP_ID, 0xFFFFFFFF, 43752),\n',
     '\tBRCMF_FW_ENTRY(BRCM_CC_43752_CHIP_ID, 0xFFFFFFFF, 43752),\n'
     '\tBRCMF_FW_ENTRY(BRCM_CC_4375_CHIP_ID, 0xFFFFFFFF, 4375),\n'),
    ('\tBRCMF_PCIE_DEVICE(BRCM_PCIE_43752_DEVICE_ID, WCC_SEED),\n',
     '\tBRCMF_PCIE_DEVICE(BRCM_PCIE_43752_DEVICE_ID, WCC_SEED),\n'
     '\tBRCMF_PCIE_DEVICE(BRCM_PCIE_4375_DEVICE_ID, WCC_SEED),\t/* zboot */\n'
     '\tBRCMF_PCIE_DEVICE(BRCM_PCIE_4375_2G_DEVICE_ID, WCC_SEED),\n'
     '\tBRCMF_PCIE_DEVICE(BRCM_PCIE_4375_5G_DEVICE_ID, WCC_SEED),\n'),
])
