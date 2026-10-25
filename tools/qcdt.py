#!/usr/bin/env python3
"""Build a QCDT table (the "dt" blob of a Qualcomm boot image) from compiled
device trees: a header, one entry per (msm-id, board-id) pair read from the
tree's qcom,msm-id and qcom,board-id properties, then the trees themselves."""
import argparse, struct

PAGE = 2048

def fdt_props(blob, wanted):
    magic, totalsize, off_struct, off_strings = struct.unpack(">IIII", blob[:16])
    assert magic == 0xd00dfeed, "not a device tree blob"
    found = {}
    pos = off_struct
    while True:
        tok = struct.unpack(">I", blob[pos:pos + 4])[0]
        pos += 4
        if tok == 1:
            end = blob.index(b"\0", pos)
            pos = (end + 1 + 3) & ~3
        elif tok == 3:
            length, nameoff = struct.unpack(">II", blob[pos:pos + 8])
            pos += 8
            name = blob[off_strings + nameoff:blob.index(b"\0", off_strings + nameoff)].decode()
            if name in wanted:
                found[name] = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif tok in (2, 4):
            continue
        elif tok == 9:
            break
        else:
            raise ValueError("bad fdt token %d" % tok)
    return found

def words(b):
    return list(struct.unpack(">%dI" % (len(b) // 4), b))

def main():
    p = argparse.ArgumentParser()
    p.add_argument("-o", "--output", required=True)
    p.add_argument("dtb", nargs="+")
    a = p.parse_args()
    entries = []
    blobs = []
    for path in a.dtb:
        blob = open(path, "rb").read()
        props = fdt_props(blob, {"qcom,msm-id", "qcom,board-id"})
        msm = words(props["qcom,msm-id"])
        board = words(props["qcom,board-id"])
        idx = len(blobs)
        blobs.append(blob)
        for i in range(0, len(msm), 2):
            for j in range(0, len(board), 2):
                entries.append((msm[i], board[j], board[j + 1], msm[i + 1], idx))
    header = struct.pack("<4sII", b"QCDT", 2, len(entries))
    table_size = len(header) + len(entries) * 24 + 4
    offset = (table_size + PAGE - 1) & ~(PAGE - 1)
    offsets = []
    for blob in blobs:
        offsets.append(offset)
        offset += (len(blob) + PAGE - 1) & ~(PAGE - 1)
    out = header
    for plat, variant, subtype, rev, idx in entries:
        out += struct.pack("<IIIIII", plat, variant, subtype, rev, offsets[idx], len(blobs[idx]))
    out += struct.pack("<I", 0)
    out += b"\0" * (-len(out) % PAGE)
    for blob in blobs:
        out += blob + b"\0" * (-len(blob) % PAGE)
    open(a.output, "wb").write(out)

if __name__ == "__main__":
    main()
