#!/usr/bin/env python3
"""brcmfmac: strict PCIe ordering and sane ring indices (zboot).

Added for OmonullOS, November 2026. The BCM4375 comes out of reset with
Relaxed Ordering enabled in its Device Control (lspci: RlxdOrd+). With it,
the chip's DMA write of a completion may land after the ring index it
updates, so the host reads an old entry: dozens of
"brcmf_msgbuf_get_pktid: Invalid packet id N (not in use)", one per
interrupt, then an oops in brcmf_msgbuf_process_rx reading past the end of
the ring (11-13 under load, 11-14 after a power-off, 11-16 after a wake).
Every reset (FLR, re-probe after a sleep) turns RO back on, so it is cleared
at every probe: strict ordering is always correct.

A ring index read from the dongle that is not below the ring depth (an
all-ones MMIO read, a confused firmware) is refused instead of walking off
the ring; the old index is kept.
"""
from pathlib import Path
import sys

base = Path(sys.argv[1]) / 'drivers/net/wireless/broadcom/brcm80211'


def edit(rel, pairs):
    p = base / rel
    s = p.read_text()
    if 'zboot-order' in s:
        return
    for anchor, new in pairs:
        if s.count(anchor) != 1:
            sys.exit(f'patch-brcmfmac-ordering: {rel}: anchor not found once: {anchor!r}')
        s = s.replace(anchor, new)
    p.write_text(s)


def idx(name, other):
    old = (f'\tcommonring->{name} = devinfo->read_ptr(devinfo, ring->{name[0]}_idx_addr);\n')
    new = (f'\t{{\t/* zboot-order: never take an index past the ring */\n'
           f'\t\tu16 v = devinfo->read_ptr(devinfo, ring->{name[0]}_idx_addr);\n\n'
           f'\t\tif (v >= commonring->depth) {{\n'
           f'\t\t\tdev_warn_ratelimited(&devinfo->pdev->dev,\n'
           f'\t\t\t\t"ring %d: bad {name} %u (depth %u), kept %u\\n",\n'
           f'\t\t\t\tring->id, v, commonring->depth, commonring->{name});\n'
           f'\t\t\treturn -EIO;\n'
           f'\t\t}}\n'
           f'\t\tcommonring->{name} = v;\n'
           f'\t}}\n')
    return (old, new)


edit('brcmfmac/pcie.c', [
    ('\tpci_set_master(pdev);\n',
     '\tpci_set_master(pdev);\n'
     '\t/* zboot-order: completions must not pass their ring index */\n'
     '\tpcie_capability_clear_word(pdev, PCI_EXP_DEVCTL,\n'
     '\t\t\t\t   PCI_EXP_DEVCTL_RELAX_EN);\n'
     '\tbrcmf_dbg(PCIE, "relaxed ordering off\\n");\n'),
    idx('r_ptr', 'w_ptr'),
    idx('w_ptr', 'r_ptr'),
])
