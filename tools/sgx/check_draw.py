#!/usr/bin/env python3
"""Determinism and host-independence checks for glrec.py recordings.

Usage: check_draw.py SCENARIO...

For each scenario: run it twice (all output files must be identical), then once with the guest
layout, kernel handle numbering and clock shifted (every recorded GPU memory image and every
allocation must be identical; only handles, CPU addresses and the manifest's bridge-call hex may
differ).  Exit status 1 on any difference.
"""
import filecmp
import json
import os
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import glrec  # noqa: E402


def tree_files(root):
    out = []
    for d, _, fs in os.walk(root):
        for f in fs:
            out.append(os.path.relpath(os.path.join(d, f), root))
    return sorted(out)


def gpu_files(root):
    return [f for f in tree_files(root) if f.endswith(".bin")]


SHIFT = 0x08000000


def host_pointer_words(path_a, path_c):
    """The words in which two images differ if every difference is the guest-address shift (a CPU
    pointer the vendor driver stored in GPU memory, in the heap or the library region, which move in
    opposite directions); None if anything else differs."""
    a, c = (open(p, "rb").read() for p in (path_a, path_c))
    if len(a) != len(c):
        return None
    out = []
    for off in range(0, len(a), 4):
        if a[off:off + 4] != c[off:off + 4]:
            x, y = struct.unpack_from("<I", a, off)[0], struct.unpack_from("<I", c, off)[0]
            if y - x not in (SHIFT, -SHIFT):
                return None
            out.append(off)
    return out


def main():
    bad = 0
    for name in sys.argv[1:]:
        tmp = tempfile.mkdtemp(prefix="sgxchk")
        try:
            a, b, c = (os.path.join(tmp, x) for x in "abc")
            glrec.run(name, a)
            glrec.run(name, b)
            glrec.run(name, c, shift=0x08000000)
            fa, fb = tree_files(a), tree_files(b)
            same = fa == fb and all(filecmp.cmp(os.path.join(a, f), os.path.join(b, f), shallow=False) for f in fa)
            print("%-12s two runs identical (%d files): %s" % (name, len(fa), "PASS" if same else "FAIL"))
            bad += not same
            ga, gc = gpu_files(a), gpu_files(c)
            diff = [f for f in ga if f not in gc or not filecmp.cmp(os.path.join(a, f), os.path.join(c, f), shallow=False)]
            hostptr = {}
            for f in list(diff):
                w = host_pointer_words(os.path.join(a, f), os.path.join(c, f)) if f in gc else None
                if w:
                    hostptr[f] = w
                    diff.remove(f)
            ma, mc = (json.load(open(os.path.join(x, "manifest.json"))) for x in (a, c))
            keys = ("allocations",)
            mdiff = [k for k in keys if ma[k] != mc[k]]
            def kick_key(x):
                pairs = lambda l: [(e["addr"], e["value"]) for e in l]
                return (pairs(x["fields"]["TAStatus"]), pairs(x["fields"]["3DStatus"]), x["ccb_offset"],
                        x["completion"], x["cmdta_command_hex"])
            kd = [i for i, (x, y) in enumerate(zip(ma["kicks"], mc["kicks"])) if kick_key(x) != kick_key(y)]
            def xfer_key(x):
                return (x["ccb_offset"], x["completion"], x["sync_objects"], x["command"], x["fields"]["ui32Flags"])
            xa, xc = ma.get("transfers", []), mc.get("transfers", [])
            kd += ["transfer %d" % i for i in range(max(len(xa), len(xc)))
                   if i >= len(xa) or i >= len(xc) or xfer_key(xa[i]) != xfer_key(xc[i])]
            ok = not diff and gc == ga and not mdiff and not kd
            print("%-12s shifted host addresses/handles/time: %d GPU images compared, %d differ%s: %s"
                  % (name, len(ga), len(diff), (" (%s)" % ", ".join(diff[:4])) if diff else "",
                     "PASS" if ok else "FAIL"))
            if hostptr:
                print("%-12s   note: %d images hold a guest CPU pointer written by the vendor driver (differs by the "
                      "shift only): %s" % (name, len(hostptr), ", ".join("%s+%s" % (f, ",".join("0x%x" % o for o in w))
                                                                       for f, w in sorted(hostptr.items())[:3])))
            bad += not ok
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
