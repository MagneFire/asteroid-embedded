#!/usr/bin/env python3
"""Wrap a flat binary, an optional ramdisk and a device-tree table into an
Android boot image (header version 0) the way the Qualcomm bootloader
expects it."""
import argparse, hashlib, struct

PAGE = 2048

def pad(b):
    return b + b"\0" * (-len(b) % PAGE)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--kernel", required=True)
    p.add_argument("--ramdisk")
    p.add_argument("--dt", required=True)
    p.add_argument("--output", "-o", required=True)
    p.add_argument("--kernel-addr", type=lambda s: int(s, 0), default=0x00008000)
    p.add_argument("--ramdisk-addr", type=lambda s: int(s, 0), default=0x02000000)
    p.add_argument("--tags-addr", type=lambda s: int(s, 0), default=0x01e00000)
    p.add_argument("--cmdline", default="")
    a = p.parse_args()
    kernel = open(a.kernel, "rb").read()
    dt = open(a.dt, "rb").read()
    ramdisk = open(a.ramdisk, "rb").read() if a.ramdisk else b""
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b"", dt):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    hdr = struct.pack("<8sIIIIIIIII", b"ANDROID!", len(kernel), a.kernel_addr, len(ramdisk),
                      a.ramdisk_addr, 0, 0, a.tags_addr, PAGE, len(dt))
    hdr += b"\0" * 4 + b"\0" * 16 + a.cmdline.encode().ljust(512, b"\0")
    hdr += sha.digest().ljust(32, b"\0") + b"\0" * 1024
    with open(a.output, "wb") as f:
        f.write(pad(hdr) + pad(kernel) + pad(ramdisk) + pad(dt))

if __name__ == "__main__":
    main()
