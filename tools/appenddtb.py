#!/usr/bin/env python3
"""Put device trees behind a flat binary the way a bootloader without a QCDT
table expects them: the zImage magic and the offset of the first tree in the
header words start.S leaves free, the trees after the binary on an 8-byte
boundary, one after another. A bootloader that takes the first one finds it
through the header; one that picks a tree by its ids reads them all."""
import argparse, struct

MAGIC_OFFSET = 0x24
END_OFFSET = 0x2c
ZIMAGE_MAGIC = 0x016f2818

def append(kernel, *dtbs):
    assert kernel[MAGIC_OFFSET:END_OFFSET + 4] == bytes(12), "no free header words"
    kernel += b"\0" * (-len(kernel) % 8)
    out = bytearray(kernel)
    struct.pack_into("<I", out, MAGIC_OFFSET, ZIMAGE_MAGIC)
    struct.pack_into("<I", out, END_OFFSET, len(kernel))
    for d in dtbs[:-1]:
        assert len(d) % 4 == 0, "a tree that would leave the next one unaligned"
    return bytes(out) + b"".join(dtbs)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("-o", "--output", required=True)
    p.add_argument("kernel")
    p.add_argument("dtb", nargs="+")
    a = p.parse_args()
    out = append(open(a.kernel, "rb").read(), *(open(d, "rb").read() for d in a.dtb))
    open(a.output, "wb").write(out)

if __name__ == "__main__":
    main()
