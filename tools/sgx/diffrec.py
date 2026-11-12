#!/usr/bin/env python3
"""Compare GPU memory images of glrec.py recordings.

  diffrec.py RECA RECB [--kick N] [--phase pre|post]   same kick of two recordings
  diffrec.py REC --kick N                              pre against post of one kick

RECx are directories such as build/sgx/draw/tri.  Allocations are matched by
(heap, GPU address, size).  Differences are listed per allocation as 32-bit words with their GPU
addresses.  Add --limit N to cap the words shown per allocation (default 48), --all for no cap,
--summary for one line per allocation.
"""
import argparse
import json
import os
import struct
import sys

ROLES = {
    (3, 68): "HWPBDESC", (3, 16): "HWPBBlock", (10, 4050944): "PB parameter pages",
    (10, 65536): "PB page-link table", (3, 67208): "TA client CCB", (3, 64): "HW render context",
    (3, 1040): "HWRENDERDETAILS", (3, 56): "HW device sync list", (3, 4): "RT dataset pending counter", (3, 8): "TA CCB control",
    (3, 128): "RT dataset + RTDATA[2]", (1, 4096): "tail pointers", (1, 64): "context control",
    (1, 80): "context OTPM", (10, 512): "special objects", (1, 6912): "region headers",
    (1, 136): "context state (3D registers shadow)", (4, 1024): "USSE_FRAG CBUF8 (surface tile-store code)",
    (5, 16): "terminate vertex code", (5, 32768): "VertexUSSE code heap chunk",
    (4, 32768): "PixelUSSE code heap chunk", (6, 32768): "PDSPixel code heap chunk",
    (0, 409600): "pbuffer colour storage", (7, 51200): "PDS vertex ring (CBUF3 / MTE copy)",
    (6, 102400): "PDS fragment ring", (0, 204800): "vertex / index data ring", (0, 20480): "VDM control ring (CBUF0)",
    (0, 32768): "General heap code chunk",
}


def load(rec):
    with open(os.path.join(rec, "manifest.json")) as f:
        m = json.load(f)
    return m


def label(a, m):
    r = ROLES.get((a["heap_id"], a["size"]))
    if a["tag"] == "sync":
        r = "sync data"
    if r is None and a["heap_id"] == 8:
        r = "status / access word" if a["size"] <= 8 else "cache-coherent data"
    if r is None and a["tag"]:
        r = a["tag"]
    return r or a["heap_name"]


def read_alloc(rec, kick, phase, a):
    p = os.path.join(rec, "kick%02d_%s" % (kick, phase), a["file"])
    with open(p, "rb") as f:
        return f.read()


def diff_words(x, y):
    n = max(len(x), len(y))
    x = x.ljust(n, b"\0")
    y = y.ljust(n, b"\0")
    out = []
    for off in range(0, n, 4):
        if x[off:off + 4] != y[off:off + 4]:
            out.append((off, struct.unpack_from("<I", x, off)[0] if off + 4 <= len(x) else None,
                        struct.unpack_from("<I", y, off)[0] if off + 4 <= len(y) else None))
    return out


def runs(words):
    res = []
    for w in words:
        if res and w[0] == res[-1][-1][0] + 4:
            res[-1].append(w)
        else:
            res.append([w])
    return res


def key(a):
    return (a["heap_id"], a["devva"], a["size"])


def compare(recA, recB, kickA, phaseA, kickB, phaseB):
    ma, mb = load(recA), load(recB)
    A = {key(a): a for a in ma["allocations"] if a["freed_at_bridge_call"] is None}
    B = {key(a): a for a in mb["allocations"] if a["freed_at_bridge_call"] is None}
    result = {"only_a": [], "only_b": [], "changed": []}
    pairs = []
    ra = {k: v for k, v in A.items() if k not in B}
    rb = {k: v for k, v in B.items() if k not in A}
    for k in sorted(set(A) & set(B), key=lambda k: (k[1], k[0])):
        pairs.append((A[k], B[k]))
    byaddr = {(k[0], k[1]): v for k, v in rb.items()}
    for k, v in sorted(ra.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        w = byaddr.pop((k[0], k[1]), None)
        if w is not None:
            pairs.append((v, w))
        else:
            result["only_a"].append(v)
    result["only_b"] = [v for k, v in sorted(rb.items(), key=lambda kv: (kv[0][1], kv[0][0]))
                        if (k[0], k[1]) in byaddr]
    for a, b in pairs:
        x = read_alloc(recA, kickA, phaseA, a)[:a["size"]]
        y = read_alloc(recB, kickB, phaseB, b)[:b["size"]]
        if x != y:
            result["changed"].append((a, b, diff_words(x, y)))
    return result, ma, mb


def fmt_word(v):
    return "        -" if v is None else "0x%08x" % v


def report(result, ma, limit, summary, names):
    total = 0
    for a, b, words in result["changed"]:
        total += len(words)
        sz = "size %d" % a["size"] if a["size"] == b["size"] else "size %d -> %d" % (a["size"], b["size"])
        print("#%d / #%d  heap %s  GPU 0x%08x  %s  [%s]  %d words differ" %
              (a["index"], b["index"], a["heap_name"], a["devva"], sz, label(a, ma), len(words)))
        if summary:
            continue
        shown = 0
        for r in runs(words):
            for off, x, y in r:
                if limit is not None and shown >= limit:
                    break
                print("    0x%08x (+0x%04x)  %s -> %s" % (a["devva"] + off, off, fmt_word(x), fmt_word(y)))
                shown += 1
        if limit is not None and len(words) > limit:
            print("    ... %d more words" % (len(words) - limit))
    for a in result["only_a"]:
        print("only in %s: #%d heap %s GPU 0x%08x size %d [%s]" % (names[0], a["index"], a["heap_name"], a["devva"], a["size"], label(a, ma)))
    for b in result["only_b"]:
        print("only in %s: #%d heap %s GPU 0x%08x size %d [%s]" % (names[1], b["index"], b["heap_name"], b["devva"], b["size"], label(b, ma)))
    print("total: %d allocations changed, %d words" % (len(result["changed"]), total))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recs", nargs="+")
    ap.add_argument("--kick", type=int, default=0)
    ap.add_argument("--kick-b", type=int, default=None, help="kick of the second recording (default: same)")
    ap.add_argument("--phase", choices=("pre", "post"), default="pre")
    ap.add_argument("--limit", type=int, default=48)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--summary", action="store_true")
    a = ap.parse_args()
    limit = None if a.all else a.limit
    if len(a.recs) == 1:
        res, ma, mb = compare(a.recs[0], a.recs[0], a.kick, "pre", a.kick, "post")
        names = ("pre", "post")
        print("%s kick %d: pre -> post" % (a.recs[0], a.kick))
    elif len(a.recs) == 2:
        kb = a.kick if a.kick_b is None else a.kick_b
        res, ma, mb = compare(a.recs[0], a.recs[1], a.kick, a.phase, kb, a.phase)
        names = tuple(os.path.basename(r.rstrip("/")) for r in a.recs)
        print("%s kick %d %s -> %s kick %d %s" % (names[0], a.kick, a.phase, names[1], kb, a.phase))
    else:
        ap.error("one or two recordings")
    report(res, ma, limit, a.summary, names)


if __name__ == "__main__":
    main()
