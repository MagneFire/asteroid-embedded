#!/usr/bin/env python3
"""Pack files into the boot image's ramdisk: a header ('AEBL', count), one
48-byte entry per file (name[40], offset, size; offsets from the start of
the archive, 64-byte aligned), then the data."""
import argparse, os, struct

def main():
    p = argparse.ArgumentParser()
    p.add_argument("-o", "--output", required=True)
    p.add_argument("files", nargs="*")
    a = p.parse_args()
    blobs = [(os.path.basename(f).encode(), open(f, "rb").read()) for f in a.files]
    off = (8 + 48 * len(blobs) + 63) & ~63
    table = b""
    data = b""
    for name, blob in blobs:
        assert len(name) < 40, name
        table += struct.pack("<40sII", name, off + len(data), len(blob))
        data += blob + b"\0" * (-len(blob) % 64)
    head = struct.pack("<4sI", b"AEBL", len(blobs)) + table
    open(a.output, "wb").write(head + b"\0" * (off - len(head)) + data)

if __name__ == "__main__":
    main()
