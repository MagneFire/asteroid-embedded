#!/usr/bin/env python3
"""Put a device tree behind a flat binary the way a bootloader without a QCDT
table expects it: the zImage magic and the offset of the tree in the header
words start.S leaves free, the tree after the binary on an 8-byte boundary."""
import argparse, struct

MAGIC_OFFSET = 0x24
END_OFFSET = 0x2c
ZIMAGE_MAGIC = 0x016f2818

def append(kernel, dtb):
    assert kernel[MAGIC_OFFSET:END_OFFSET + 4] == bytes(12), "no free header words"
    kernel += b"\0" * (-len(kernel) % 8)
    out = bytearray(kernel)
    struct.pack_into("<I", out, MAGIC_OFFSET, ZIMAGE_MAGIC)
    struct.pack_into("<I", out, END_OFFSET, len(kernel))
    return bytes(out) + dtb

def main():
    p = argparse.ArgumentParser()
    p.add_argument("-o", "--output", required=True)
    p.add_argument("kernel")
    p.add_argument("dtb")
    a = p.parse_args()
    out = append(open(a.kernel, "rb").read(), open(a.dtb, "rb").read())
    open(a.output, "wb").write(out)

if __name__ == "__main__":
    main()
