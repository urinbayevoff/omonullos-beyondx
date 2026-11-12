#!/usr/bin/env python3
"""Build a boot image with an Android boot header v1, as the Galaxy S10 5G's.

  mkboot-v1.py --ref <stock boot.img> --kernel <file> [--ramdisk <file>]
               [--cmdline "..."] -o <out.img>

Added for OmonullOS, October 2026. The S9's images are header v0 with a dt
section (third_party/s9plus/scripts/build/mkboot.py); the S10 5G's are v1,
its device tree lives in the DTB partition, and its stock boot.img has no
ramdisk. Every header field but the sizes, the command line and the id is
copied from --ref, an image this phone already boots, so only the payload
differs. The id is the SHA1 mkbootimg writes: S-Boot checks it, and a
wrong one sends the phone to recovery as if the kernel had hung.

Nothing is signed: no SEANDROIDENFORCE trailer, no AVB footer. An
unlocked bootloader boots it with its warning.

Checked by rebuilding the stock boot.img from its own kernel: the result
matches the stock image byte for byte up to the payload's end.
"""
import argparse
import hashlib
import struct
from pathlib import Path

FMT = "<8sIIIIIIIIII16s512s32s1024sIQI"	# boot_img_hdr_v1, 1648 bytes
HEADER_SIZE = struct.calcsize(FMT)

ap = argparse.ArgumentParser()
ap.add_argument("--ref", required=True)
ap.add_argument("--kernel", required=True)
ap.add_argument("--ramdisk")
ap.add_argument("--cmdline")
ap.add_argument("-o", "--out", required=True)
args = ap.parse_args()

ref = Path(args.ref).read_bytes()
(magic, _ksz, kaddr, _rsz, raddr, _ssz, saddr, tags, page, version, os_version,
 name, cmdline, _id, extra, _dtbo_sz, _dtbo_off, hdr_size) = struct.unpack_from(FMT, ref)
if magic != b"ANDROID!" or version != 1 or hdr_size != HEADER_SIZE:
    raise SystemExit(f"mkboot-v1: {args.ref} is not a header v1 boot image")

kernel = Path(args.kernel).read_bytes()
ramdisk = Path(args.ramdisk).read_bytes() if args.ramdisk else b""
if args.cmdline is not None:
    if len(args.cmdline) >= 512:
        raise SystemExit("mkboot-v1: command line longer than 511 bytes")
    cmdline = args.cmdline.encode()

sha = hashlib.sha1()
for part in (kernel, ramdisk, b"", b""):	# kernel, ramdisk, second, recovery dtbo
    sha.update(part)
    sha.update(struct.pack("<I", len(part)))

header = struct.pack(FMT, magic, len(kernel), kaddr, len(ramdisk), raddr, 0, saddr, tags,
                     page, version, os_version, name, cmdline, sha.digest(), extra, 0, 0,
                     HEADER_SIZE)


def padded(blob):
    return blob + b"\0" * (-len(blob) % page)


Path(args.out).write_bytes(padded(header) + padded(kernel) + padded(ramdisk))
print(f"mkboot-v1: {args.out}: kernel {len(kernel)}, ramdisk {len(ramdisk)}, id {sha.hexdigest()[:12]}")
