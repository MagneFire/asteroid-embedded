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
            ma, mc = (json.load(open(os.path.join(x, "manifest.json"))) for x in (a, c))
            keys = ("allocations",)
            mdiff = [k for k in keys if ma[k] != mc[k]]
            def kick_key(x):
                pairs = lambda l: [(e["addr"], e["value"]) for e in l]
                return (pairs(x["fields"]["TAStatus"]), pairs(x["fields"]["3DStatus"]), x["ccb_offset"],
                        x["completion"], x["cmdta_command_hex"])
            kd = [i for i, (x, y) in enumerate(zip(ma["kicks"], mc["kicks"])) if kick_key(x) != kick_key(y)]
            ok = not diff and gc == ga and not mdiff and not kd
            print("%-12s shifted host addresses/handles/time: %d GPU images compared, %d differ%s: %s"
                  % (name, len(ga), len(diff), (" (%s)" % ", ".join(diff[:4])) if diff else "",
                     "PASS" if ok else "FAIL"))
            bad += not ok
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
