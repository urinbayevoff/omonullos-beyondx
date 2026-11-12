#!/usr/bin/env python3
"""Read the BCM4375 dongle's console ring and pcie_shared fields over BAR2.

Added for OmonullOS, October 2026, to debug Wi-Fi bring-up on the S10 5G
without a BRCMDBG build. Read only: mmaps the endpoint's TCM window
(/sys/bus/pci/devices/<dev>/resource2) and reads; never writes.
Offsets: brcmfmac pcie.c (BRCMF_SHARED_*, BRCMF_CONSOLE_*).
  wlconsole.py [--shared] [dev]
"""
import mmap, struct, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
dev = args[0] if args else '0000:01:00.0'
f = open(f'/sys/bus/pci/devices/{dev}/resource2', 'rb')
m = mmap.mmap(f.fileno(), 0x400000, mmap.MAP_SHARED, mmap.PROT_READ)
r32 = lambda a: struct.unpack_from('<I', m, a)[0]
r16 = lambda a: struct.unpack_from('<H', m, a)[0]

shared = r32(0x300000 - 4)		# rambase 0x170000 + ramsize - 4
if '--shared' in sys.argv:
    print(f'shared {shared:#x} flags {r32(shared):#x} (version {r32(shared) & 0xff})')
    print(f'max_rxbufpost {r16(shared + 34)} rx_dataoffset {r32(shared + 36)}')
    print(f'ring_info {r32(shared + 48):#x} console {r32(shared + 20):#x}')
con = r32(shared + 20)
buf, size, widx = r32(con + 8), r32(con + 12), r32(con + 16)
data = bytes(m[buf + i] for i in range(size))
text = (data[widx:] + data[:widx]).replace(b'\0', b'').decode('ascii', 'replace')
print(text)
