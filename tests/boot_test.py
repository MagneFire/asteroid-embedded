#!/usr/bin/env python3
"""The boot image's device trees: sparrow's and sturgeon's QCDT entries, at the
offsets they have always had, with smelt's and catfish's after them, and the
tree behind the
kernel where a bootloader looks for it."""
import os, struct, sys
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import appenddtb

PAGE = 2048

def entries(img):
    magic, version, n = struct.unpack("<4sII", img[:12])
    assert magic == b"QCDT" and version == 2
    return [struct.unpack("<6I", img[12 + 24 * i:36 + 24 * i]) for i in range(n)]

def compatible(blob):
    return (b"moto,smelt" in blob, b"asus,sparrow" in blob, b"huawei,sturgeon" in blob,
            b"mobvoi,catfish" in blob)

def main():
    img = open(sys.argv[1], "rb").read()
    e = entries(img)
    assert (e[0][:4], e[1][:4]) == ((199, 8, 3006, 0x20000), (199, 8, 3005, 0x20000)), e[:2]
    assert e[2][:4] == (199, 8, 4, 0x20000), e[2]
    assert e[0][4] == PAGE and e[1][4] == PAGE, e[:2]
    assert e[2][4] == e[0][4] + (e[0][5] + PAGE - 1) // PAGE * PAGE, e[2]
    smelt = e[3:18]
    assert len(smelt) == 15, len(smelt)
    assert all(x[0] == 199 and x[1] == 0x41 and x[2] == 0 for x in smelt), smelt
    assert {x[3] for x in smelt} >= {0x0704, 0x0707, 0x070a}, smelt
    assert len({x[4] for x in smelt}) == 1
    off, size = smelt[0][4:6]
    assert off >= e[2][4] + e[2][5]
    assert compatible(img[off:off + size]) == (True, False, False, False)
    assert compatible(img[e[0][4]:e[0][4] + e[0][5]]) == (False, True, False, False)
    assert compatible(img[e[2][4]:e[2][4] + e[2][5]]) == (False, False, True, False)
    catfish = e[18:]
    assert [x[:4] for x in catfish] == [(265, 8, 0x105, 0), (265, 8, 5, 0), (301, 8, 0x105, 0), (301, 8, 5, 0)], catfish
    assert len({x[4] for x in catfish}) == 1 and catfish[0][4] >= off + size
    assert compatible(img[catfish[0][4]:catfish[0][4] + catfish[0][5]]) == (False, False, False, True)

    kernel = bytes(0x31) + b"code"
    out = appenddtb.append(kernel, b"\xd0\x0d\xfe\xedtree")
    end = struct.unpack("<I", out[0x2c:0x30])[0]
    assert struct.unpack("<I", out[0x24:0x28])[0] == 0x016f2818
    assert end % 8 == 0 and end >= len(kernel) and out[end:] == b"\xd0\x0d\xfe\xedtree"
    assert out[0x30:len(kernel)] == kernel[0x30:]
    print("boot_test: ok")

if __name__ == "__main__":
    main()
