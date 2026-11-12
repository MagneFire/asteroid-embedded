#!/usr/bin/env python3
"""Texture layout table: what the GLES driver allocates and encodes for textures of different sizes.

Usage: texprobe.py [WxH[:a8] ...]       (default: the list below; a8 = GL_ALPHA, else GL_RGBA)

For each size the quad_tex scenario runs on the host stack and the table shows the texture memory
the driver allocated, the number of SUBMITTRANSFER calls the upload took, the size/format word and
sampling-control word the driver put in the pixel PDS program (read back from the program, found by its
sampling-control signature), and, from the first transfer, the two destination words that depend on the
layout (the layout-flag immediate and the stride immediate of the end-of-tile program) and the two
texture-state words with which the blit samples the staging buffer.
Nothing is written to build/sgx.
"""
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import glrec  # noqa: E402

DEFAULT = ["32x32", "64x32", "256x256", "512x64", "33x33", "100x50", "100x100", "96x96", "200x200", "320x240",
           "320x320", "32x32:a8", "100x100:a8"]
SAMPLING_SIG = 0x03fe0090        # nearest, clamp-to-edge (A8 sets bit 30 as well)


def probe(w, h, a8):
    d = tempfile.mkdtemp(prefix="sgxtex")
    try:
        s = glrec.Session(d, 0, False, transfer=True)
        k = s.kernel
        first = {}
        orig = k.dispatch[98]

        def hook(inb, out):
            if not first:
                tq = [a for a in k.allocs if a.group and a.heap == 4 and a.size == 2048][0]
                first["words"] = struct.unpack_from("<12I", k.read(tq), 0)
                pds = [a for a in k.allocs if a.group and a.heap == 6 and a.size == 1024][0]
                first["src"] = struct.unpack_from("<8I", k.read(pds), 0)
            return orig(inb, out)
        k.dispatch[98] = hook
        glrec.quad_scene(s, (w, h), glrec.GL_ALPHA if a8 else glrec.GL_RGBA)
        tex = [a for a in k.allocs if a.heap == 0 and a.index > 140 and a.size > 100 and a.size != 2048][-1]
        pds = [a for a in k.allocs if a.heap == 6 and a.size == 32768 and not a.group][0]
        p = struct.unpack("<%dI" % (pds.size // 4), k.read(pds))
        i = next(i for i, x in enumerate(p) if x & 0xBFFFFFFF == SAMPLING_SIG)
        q = first["words"]
        return {"bytes": tex.size, "xfers": len(k.transfers), "sampling": p[i], "size": p[i + 1],
                "addr_word": p[i + 8], "dst_flags": q[5], "dst_stride": q[8], "src_a": first["src"][4],
                "src_b": first["src"][5]}
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    specs = sys.argv[1:] or DEFAULT
    print("%-11s %9s %-11s %5s  %-9s %-8s %-8s %-8s  %-8s %-8s" % ("size", "bytes", "aligned", "xfers", "size word",
                                                                  "sampling", "dst flag", "dst strd", "src A", "src B"))
    for sp in specs:
        a8 = sp.endswith(":a8")
        w, h = (int(x) for x in sp.split(":")[0].split("x"))
        r = probe(w, h, a8)
        bpp = 1 if a8 else 4
        al = lambda v: (v + 31) & ~31
        print("%-11s %9d %-11s %5d  %08x  %08x %08x %08x  %08x %08x" % (
            sp, r["bytes"], "%dx%dx%d" % (al(w), al(h), bpp), r["xfers"], r["size"], r["sampling"], r["dst_flags"],
            r["dst_stride"], r["src_a"], r["src_b"]))


if __name__ == "__main__":
    main()
