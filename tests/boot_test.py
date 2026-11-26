#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""The boot image's device trees: sparrow's and sturgeon's QCDT entries, at the
offsets they have always had, with smelt's, catfish's, swift's and narwhal's
after them, dory's and lenok's in LG's own layout last, and the
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
            b"mobvoi,catfish" in blob, b"lge,dory" in blob, b"asus,swift" in blob, b"lge,lenok" in blob,
            b"lge,narwhal" in blob)

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
    assert compatible(img[off:off + size]) == (True, False, False, False, False, False, False, False)
    assert compatible(img[e[0][4]:e[0][4] + e[0][5]]) == (False, True, False, False, False, False, False, False)
    assert compatible(img[e[2][4]:e[2][4] + e[2][5]]) == (False, False, True, False, False, False, False, False)
    catfish = e[18:22]
    assert [x[:4] for x in catfish] == [(265, 8, 0x105, 0), (265, 8, 5, 0), (301, 8, 0x105, 0), (301, 8, 5, 0)], catfish
    assert len({x[4] for x in catfish}) == 1 and catfish[0][4] >= off + size
    assert compatible(img[catfish[0][4]:catfish[0][4] + catfish[0][5]]) == (False, False, False, True, False, False, False, False)
    swift = e[22:24]
    assert [x[:4] for x in swift] == [(265, 8, 0x133, 0), (301, 8, 0x133, 0)], swift
    assert len({x[4] for x in swift}) == 1 and swift[0][4] >= catfish[0][4] + catfish[0][5]
    assert compatible(img[swift[0][4]:swift[0][4] + swift[0][5]]) == (False, False, False, False, False, True, False, False)
    narwhal = e[24:27]
    assert [x[:4] for x in narwhal] == [(301, 0x899, 0, 0), (301, 0x699, 0, 0), (301, 0x499, 0, 0)], narwhal
    assert len({x[4] for x in narwhal}) == 1 and narwhal[0][4] >= swift[0][4] + swift[0][5]
    assert compatible(img[narwhal[0][4]:narwhal[0][4] + narwhal[0][5]]) == (False,) * 7 + (True,)
    # LG's layout: platform, board, soc rev, offset, size, board revision.
    dory = e[27:39]
    assert len(dory) == 12 and all(x[1] == 131 and x[5] in (10, 11) for x in dory), dory
    assert dory[:2] == [dory[0][:5] + (10,), dory[0][:5] + (11,)] and dory[0][:3] == (199, 131, 0x20000), dory
    assert len({x[3:5] for x in dory}) == 1 and dory[0][3] >= narwhal[0][4] + narwhal[0][5]
    assert compatible(img[dory[0][3]:dory[0][3] + dory[0][4]]) == (False, False, False, False, True, False, False, False)
    lenok = e[39:]
    assert lenok == [(199, 132, 0x20000) + lenok[0][3:5] + (10,)], lenok
    assert lenok[0][3] >= dory[0][3] + dory[0][4]
    assert compatible(img[lenok[0][3]:lenok[0][3] + lenok[0][4]]) == (False, False, False, False, False, False, True, False)

    kernel = bytes(0x31) + b"code"
    out = appenddtb.append(kernel, b"\xd0\x0d\xfe\xedtree")
    end = struct.unpack("<I", out[0x2c:0x30])[0]
    assert struct.unpack("<I", out[0x24:0x28])[0] == 0x016f2818
    assert end % 8 == 0 and end >= len(kernel) and out[end:] == b"\xd0\x0d\xfe\xedtree"
    assert out[0x30:len(kernel)] == kernel[0x30:]
    print("boot_test: ok")

if __name__ == "__main__":
    main()
