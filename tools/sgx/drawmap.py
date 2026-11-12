#!/usr/bin/env python3
"""Decode the VDM control stream of one kick of a glrec.py recording into draws and follow every
pointer in it: MTE-copy launches to their state blocks, vertex-secondary launches to their constant
blocks, index-list blocks to the vertex PDS program, state blocks to the pixel PDS programs.

  drawmap.py REC [--kick N] [--raw]

REC is a directory such as build/sgx/draw/mix.  Prints, for each launch and index list in the stream,
the ring (allocation, offset) it lands in and its words; ends with the bytes each ring holds per kick.
Layout facts it relies on are in DIFFS.md ("Several draws in one scene").
"""
import argparse
import json
import os
import struct
import sys

PDS_VERT_BASE = 0x0E800000     # vertex PDS execution base (offsets in launches are bytes>>4 from here)
PDS_PIX_BASE = 0x0DC00000      # pixel PDS execution base


class Mem:
    def __init__(self, rec, kick):
        self.m = json.load(open(os.path.join(rec, "manifest.json")))
        self.rec, self.kick = rec, kick
        self.allocs = [a for a in self.m["allocations"] if a["freed_at_bridge_call"] is None]
        self.cache = {}

    def data(self, a):
        if a["index"] not in self.cache:
            p = os.path.join(self.rec, "kick%02d_pre" % self.kick, a["file"])
            self.cache[a["index"]] = open(p, "rb").read()[:a["size"]] if os.path.exists(p) else None
        return self.cache[a["index"]]

    def find(self, addr):
        for a in self.allocs:
            if a["devva"] <= addr < a["devva"] + max(a["size"], 4) and self.data(a) is not None:
                return a
        return None

    def words(self, addr, n):
        a = self.find(addr)
        if a is None:
            return None
        off = addr - a["devva"]
        d = self.data(a)[off:off + 4 * n]
        return list(struct.unpack("<%dI" % (len(d) // 4), d))

    def where(self, addr):
        a = self.find(addr)
        return "#%d+0x%x" % (a["index"], addr - a["devva"]) if a else "?"


def hx(ws):
    return " ".join("%08x" % w for w in ws) if ws else "-"


def decode(mem, vdm_addr, out):
    ws = mem.words(vdm_addr, 4096)
    i, draws = 0, []
    while i < len(ws):
        w = ws[i]
        if w == 0:
            break
        if w == 0xC0000000:
            out("  +0x%03x  c0000000  end of stream" % (4 * i))
            break
        if w >> 16 == 0x81C0 or (w & 0xFFC00000) == 0x81C00000:
            blk = ws[i:i + 6]
            cnt = w & 0x1FFFFF
            pds = PDS_VERT_BASE + ((blk[4] & 0xFFFF) << 4)
            size = (blk[5] & 0xF) << 4
            prog = mem.words(pds, 24)
            out("  +0x%03x  INDEX LIST count %d  index addr 0x%08x (%s)  vertex PDS 0x%08x (%s) data %d bytes" % (
                4 * i, cnt, blk[1], mem.where(blk[1]), pds, mem.where(pds), size))
            out("          words %s" % hx(blk))
            out("          vertex PDS data: %s" % hx(prog[:size // 4]))
            draws.append({"count": cnt, "index": blk[1], "pds": pds, "vtx": prog, "size": size, "at": 4 * i, "blk": blk})
            i += 6
            continue
        if (w & 0xC0000000) == 0x40000000 or (w & 0xC0000000) == 0x60000000:
            w1 = ws[i + 1]
            addr = (PDS_VERT_BASE if (w & 0xFFFF0000) == 0x400C0000 else PDS_VERT_BASE) + ((w & 0xFFFF) << 4)
            dbytes = (w1 >> 26) << 4
            kind = {0x0C02A200: "MTE-copy", 0x0C016100: "vertex-secondary", 0x0C02C200: "terminate A",
                    0x0C02A200 + 0: "MTE-copy"}.get(w1 & 0xFFFFFF00, "?")
            if (w & 0xE0000000) == 0x60000000:
                kind = "terminate PDS"
            data = mem.words(addr, dbytes // 4)
            out("  +0x%03x  LAUNCH %-16s %08x %08x  PDS 0x%08x (%s) data %d bytes" % (
                4 * i, kind, w, w1, addr, mem.where(addr), dbytes))
            out("          data: %s" % hx(data))
            if kind == "MTE-copy" and data:
                src, burst = data[0], data[1]
                n = (burst & 0xFFFF) + 1
                blk = mem.words(src, n)
                out("          state block 0x%08x (%s), %d words: %s" % (src, mem.where(src), n, hx(blk)))
                if blk and blk[0] & 0x40:
                    k = 1 + 2 * (blk[0] & 1)       # ISP words A and B come before the three pixel words
                    for nm, pw in (("secondary", blk[k]), ("primary", blk[k + 2])):
                        pa = PDS_PIX_BASE + ((pw & 0xFFFFFF) << 4)
                        pb = (pw >> 26) << 4
                        out("          pixel %-9s PDS 0x%08x (%s) data %d bytes: %s" % (
                            nm, pa, mem.where(pa), pb, hx(mem.words(pa, 16))))
            if kind == "vertex-secondary" and data:
                src, burst = data[0], data[1]
                n = (burst & 0xFFFF) + 1
                out("          constant block 0x%08x (%s), %d words: %s" % (src, mem.where(src), n, hx(mem.words(src, n))))
            i += 2
            continue
        out("  +0x%03x  %08x  ?" % (4 * i, w))
        i += 1
    return draws


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("rec")
    ap.add_argument("--kick", type=int, default=0)
    a = ap.parse_args()
    mem = Mem(a.rec, a.kick)
    cmd = mem.m["kicks"][a.kick]
    vdm = None
    ccb = [x for x in mem.allocs if x["heap_name"] == "KernelData" and x["size"] == 67208][0]
    cd = mem.data(ccb)
    off = cmd["ccb_offset"] - 0x64
    # TA command: +0x4e4 is the VDM stream pointer (CMDTA offset; shared part follows the first 0x64 bytes)
    vdm = struct.unpack_from("<I", cd, off + 0x4E4)[0]
    print("%s kick %d: VDM stream at 0x%08x (%s)" % (a.rec, a.kick, vdm, mem.where(vdm)))
    decode(mem, vdm, print)


if __name__ == "__main__":
    main()
