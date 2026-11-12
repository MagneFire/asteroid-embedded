#!/usr/bin/env python3
"""Writes src/gpu/sgx_scene_data.[ch], the scene the native SGX530 path
(src/gpu/sgx_draw.c) replays, from the GLES recordings of glrec.py.

    mkreplay.py [DRAW_DIR]        (default build/sgx/draw)

Needs the recordings mix, mix_back, mesh, mix2, quad_tex320, quad_linear and
the tri scenes recorded with the transfer context, which gives them the mix
scenes' addresses: tri_tq, tri_read_tq, tri_color_tq, tri_blend_tq, tri_two_tq,
tri_read2_tq and tri_read_tq_gap (glrec.py --general-gap 0x100000 tri_read_tq).

The scene is the GL context of `mix` (both programs, the mesh and the textured
draws); the transfer context's allocations become holes that keep the
addresses, the one in General holds the native vertex buffer. The image is
mix's memory at its first kick less every word the native code writes: the
rings past the two background objects (every draw is generated), the per-frame
sites, and what the kernel writes. Every value the C code uses is taken from a
recording, and every site is found by comparing two recordings that differ in
one input; the script stops if a comparison shows more or less than it expects.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORKTREE = os.path.normpath(os.path.join(HERE, "..", ".."))
DRAW = sys.argv[1] if len(sys.argv) > 1 else os.path.join(WORKTREE, "build", "sgx", "draw")
OUT_C = os.path.join(WORKTREE, "src", "gpu", "sgx_scene_data.c")
OUT_H = os.path.join(WORKTREE, "src", "gpu", "sgx_scene_data.h")

INDICES = 2048          # the identity index table, one page: vertices a draw may have
CMD_SIZE = 0x540
SHARED = 0x64
PAGE = 4096
PDS_VERT_BASE = 0x0E800000
PDS_PIX_BASE = 0x0DC00000
LAUNCH, LAUNCH_MASK = 0x400C0000, 0xFFFF0000
MTE_WORD = 0x0C02A200
VSEC_WORD = 0x0C016103
END = 0xC0000000


def fail(msg):
    sys.exit("mkreplay: " + msg)


def check(cond, msg):
    if not cond:
        fail(msg)


def u32(b, off):
    return struct.unpack_from("<I", b, off)[0]


class Rec:
    def __init__(self, name):
        self.name = name
        self.dir = os.path.join(DRAW, name)
        path = os.path.join(self.dir, "manifest.json")
        if not os.path.exists(path):
            fail("no recording %s (run tools/sgx/glrec.py)" % self.dir)
        with open(path) as f:
            self.m = json.load(f)
        self.allocs = self.m["allocations"]
        self.kicks = self.m["kicks"]
        self.cache = {}

    def image(self, kick):
        if kick not in self.cache:
            img = {}
            d = os.path.join(self.dir, "kick%02d_pre" % kick)
            for a in self.allocs:
                p = os.path.join(d, a["file"])
                if os.path.exists(p):
                    with open(p, "rb") as f:
                        img[a["index"]] = f.read()
            self.cache[kick] = img
        return self.cache[kick]

    def cmd(self, kick):
        return bytes.fromhex(self.kicks[kick]["cmdta_command_hex"])

    def fields(self, kick):
        return self.kicks[kick]["fields"]

    def at(self, addr):
        for a in self.allocs:
            if a["devva"] <= addr < a["devva"] + max(a["size"], 1):
                return a
        fail("%s: 0x%08x is in no allocation" % (self.name, addr))

    def word(self, kick, addr):
        a = self.at(addr)
        return u32(self.image(kick)[a["index"]], addr - a["devva"])

    def words(self, kick, addr, n):
        return [self.word(kick, addr + 4 * i) for i in range(n)]

    def gl(self):
        return [a for a in self.allocs if a.get("group") != "transfer-context"]


def diff(ra, ka, rb, kb, only=None):
    """Changed words between two images of allocations at the same place, {address: (a, b)}."""
    out = {}
    ia, ib = ra.image(ka), rb.image(kb)
    bplace = {(y["devva"], y["heap_id"]): y for y in rb.allocs}
    for x in ra.gl():
        y = bplace.get((x["devva"], x["heap_id"]))
        if y is None or x["index"] not in ia or y["index"] not in ib:
            continue
        if only is not None and x["devva"] not in only:
            continue
        a, b = ia[x["index"]], ib[y["index"]]
        n = min(len(a), len(b))
        for o in range(0, n & ~3, 4):
            if a[o:o + 4] != b[o:o + 4]:
                out[x["devva"] + o] = (u32(a, o), u32(b, o))
    return out


def immediate(b, off):
    """The 32-bit constant in a USSE instruction (sgx_image.c sgx_set_constant)."""
    w0, w1 = u32(b, off), u32(b, off + 4)
    return (w0 & 0x1fffff) | ((w1 >> 4) & 0x1f) << 21 | ((w1 >> 12) & 0x3f) << 26


def decode(rec, kick, start):
    """The packets of a VDM stream: ("mte", entry, word1), ("vsec", desc, word1), ("index", words), ("end", words)."""
    out, a = [], start
    while True:
        w = rec.word(kick, a)
        if w & 0xFFC00000 == 0x81C00000:
            out.append(("index", a, rec.words(kick, a, 6)))
            a += 24
        elif w & LAUNCH_MASK == LAUNCH and rec.word(kick, a + 4) & 0xFFFFFE00 == MTE_WORD:
            out.append(("mte", a, [w, rec.word(kick, a + 4)]))
            a += 8
        elif w & LAUNCH_MASK == LAUNCH and rec.word(kick, a + 4) == VSEC_WORD:
            out.append(("vsec", a, [w, rec.word(kick, a + 4)]))
            a += 8
        else:
            end = rec.words(kick, a, 7)
            check(end[-1] == END, "%s kick %d: unknown VDM packet %08x at %08x" % (rec.name, kick, w, a))
            out.append(("end", a, end))
            return out


def vaddr(w):
    return PDS_VERT_BASE + ((w & 0xFFFF) << 4)


def paddr(w):
    return PDS_PIX_BASE + ((w & 0xFFFFFF) << 4)


def draws_of(rec, kick, stream, prefix_launches=2):
    """Each draw of a decoded stream: its MTE entry, state block, vertex-secondary, index list."""
    ps = decode(rec, kick, stream)
    i, bg = 0, 0
    while bg < prefix_launches:
        check(ps[i][0] == "mte" and ps[i + 1][0] == "index", "%s: background object %d" % (rec.name, bg))
        i += 2
        bg += 1
    prefix = ps[i][1] - stream
    draws = []
    while ps[i][0] != "end":
        d = {}
        if ps[i][0] == "mte":
            d["mte"] = ps[i]
            entry = vaddr(ps[i][2][0])
            d["entry"] = entry
            d["block_at"] = rec.word(kick, entry)
            d["copy"] = rec.word(kick, entry + 4)
            d["dest"] = rec.word(kick, entry + 16)
            i += 1
        if ps[i][0] == "vsec":
            d["vsec"] = ps[i]
            d["desc"] = vaddr(ps[i][2][0])
            i += 1
        check(ps[i][0] == "index", "%s: draw without an index list" % rec.name)
        d["index"] = ps[i][2]
        d["vprog"] = vaddr(ps[i][2][4])
        i += 1
        draws.append(d)
    return prefix, draws, ps[i]


def main():
    mix, back, mesh, mix2 = Rec("mix"), Rec("mix_back"), Rec("mesh"), Rec("mix2")
    tq, read, color, blend = Rec("tri_tq"), Rec("tri_read_tq"), Rec("tri_color_tq"), Rec("tri_blend_tq")
    two, read2, gap = Rec("tri_two_tq"), Rec("tri_read2_tq"), Rec("tri_read_tq_gap")
    tex320, linear = Rec("quad_tex320"), Rec("quad_linear")
    with open(os.path.join(WORKTREE, "build", "sgx", "init", "manifest.json")) as f:
        init = json.load(f)
    check(len(mix.kicks) == 2 and len(read2.kicks) == 4 and len(mix2.kicks) == 4 and len(read.kicks) == 2,
          "unexpected kick counts")

    by_index = {a["index"]: a for a in mix.allocs}
    img = mix.image(0)
    k0, k1 = mix.fields(0), mix.fields(1)
    cmd0, cmd1 = mix.cmd(0), mix.cmd(1)
    stream_a, stream_b = u32(cmd0, 0x4e4), u32(cmd1, 0x4e4)

    # --- the draws of mix: the mesh, A (TA, 1.0), B (TB, 0.5), P (TA, 0.75)
    prefix_vdm, draws, endp = draws_of(mix, 0, stream_a)
    check(len(draws) == 4, "mix has %d draws" % len(draws))
    m, A, B, P = draws
    vdm = mix.at(stream_a)
    check(vdm["devva"] == stream_a, "the stream starts its ring")
    pv_ring = mix.at(m["block_at"])
    mt_ring = mix.at(m["entry"])
    check(m["entry"] == mt_ring["devva"] + 2 * 0x80, "MTE entries: two background objects first")
    flat_prog = m["vprog"]
    check(mix.at(flat_prog)["size"] == 0x48 and mix.at(flat_prog)["heap_id"] == 7, "the cached flat vertex program")

    def block(d, rec=mix, kick=0):
        n = (d["copy"] & 0xFFFF) + 1
        return rec.words(kick, d["block_at"], n)

    def nwords(d):
        return (d["mte"][2][1] & 0xFF) * 4      # an upper bound: the launch says (n + 3) >> 2

    first_flat = block(m)
    check(len(first_flat) == 25, "the first state block copies 25 words")
    first_flat = first_flat[:18]
    check(first_flat[0] & 0x1F00 == 0x1F00 and first_flat[5] == 0x0C000802, "first flat block")
    ba, bb, bp = block(A), block(B), block(P)
    check(len(ba) == 9 and len(bb) == 4 and len(bp) == 4, "textured state blocks of 9 and 4 words")

    def pix(w):
        return paddr(w)

    # pixel primary programs of the textures, in the pixel code chunk
    pc_chunk = mix.at(pix(ba[5]))
    qa, qb = pix(ba[5]), pix(bb[3])
    check(pix(bp[3]) == qa and qb == qa + 0x40, "texture programs: one per texture, first use first")
    pc_prefix = qa - pc_chunk["devva"]
    prim = mix.words(0, qa, 16)
    prim_b = mix.words(0, qb, 16)
    tex_a, tex_b = prim[10], prim_b[10]
    check([w for i, w in enumerate(prim) if i != 10] == [w for i, w in enumerate(prim_b) if i != 10],
          "texture programs differ in more than the address")
    textures = {tex_a, tex_b}
    size_word, sampling = prim[3], prim[2]
    check(size_word & 0x00FFFFFF == (319 << 12 | 319) and size_word >> 31, "size word of a 320x320 texture")
    tex_format = size_word & 0xFF000000
    prim_t = list(prim)
    prim_t[2] = prim_t[3] = prim_t[10] = 0
    # the linear sampling word: quad_tex320 against quad_linear changes one word, at program +0x08
    dl = diff(tex320, 0, linear, 0)
    check(len(dl) == 1, "quad_linear changes %d words" % len(dl))
    (la, (near, lin)), = dl.items()
    check(near == sampling and tex320.word(0, la + 4) == size_word, "linear sampling word")

    # pixel secondary programs: constants (alpha) and descriptor, in the pixel ring
    pp_ring = mix.at(pix(ba[3]))
    secs = [pix(ba[3]), pix(bb[1]), pix(bp[1])]
    check(secs == [secs[0], secs[0] + 0x80, secs[0] + 0x100], "pixel secondary programs 0x80 apart")
    pp_prefix = secs[0] - 0x40 - pp_ring["devva"]
    sd = mix.words(0, secs[0], 16)
    check(sd[0] == secs[0] - 0x40, "secondary descriptor points at its constants")
    psec_desc = sd[1:3]
    psec_code = sd[12:15]
    check(not any(sd[3:12]) and not any(sd[15:]), "secondary descriptor layout")
    alphas = [mix.words(0, s - 0x40, 16) for s in secs]
    check([c[9] for c in alphas] == [0xff, 0x80, 0xbf], "alpha bytes 1.0, 0.5, 0.75 at constants +0x24")
    check(all(not any(c[:9] + c[10:]) for c in alphas), "constants are zero but the alpha")

    # vertex programs: in the ring for the textured draws, the cached block for the first flat one
    vt = mix.words(0, A["vprog"], 24)
    vf = mix.words(0, flat_prog, 18)
    vb = back.words(0, draws_of(back, 0, stream_a)[1][2]["vprog"], 24)
    check(vt[18:] == [0] * 6 and vf[12:] == vt[12:18] and vb[12:18] == vt[12:18], "vertex program code")
    vprog_code = vt[12:18]
    vprog_tex, vprog_flat = list(vt[:12]), list(vb[:12])
    check(vf[:12] == vb[:12], "flat vertex program in the ring and the cached block")
    qvbo = vt[0]
    check(vt[2] == qvbo + 8 and vf[2] == vf[0] + 8, "stream addresses")
    for v in (vprog_tex, vprog_flat):
        v[0] = v[2] = 0
    check(B["vprog"] == A["vprog"] + 0x120 and mix.word(0, B["vprog"]) == qvbo + 6 * 16, "B's vertices")

    # vertex-secondary: constants and descriptor
    def vsec(d, rec=mix):
        desc = rec.words(0, d["desc"], 16)
        cb = desc[0]
        return desc, rec.words(0, cb, 10)

    da, ca = vsec(A)
    check(da[0] == A["desc"] - 0x40 and ca[0] == da[0] - 4, "vertex-secondary layout")
    vsec_desc, vsec_code = da[1:3], da[12:15]
    check(not any(da[3:12]) and not any(da[15:]) and psec_code == vsec_code, "vertex-secondary descriptor")
    vsec_const = [0] + ca[1:]
    dm, cm = vsec(m)
    stale = [i for i in range(10) if i and cm[i] != vsec_const[i]]
    check(stale == [1, 6, 7, 8, 9] and not any(vsec_const[6:]) and vsec_const[1] == 0,
          "the flat draw's constants differ from the textured draws' in %s" % stale)
    check(dm[1:] == da[1:], "flat and textured descriptors")
    _, cmesh = vsec(draws_of(mesh, 0, u32(mesh.cmd(0), 0x4e4))[1][0], mesh)
    check(cmesh[1:] == vsec_const[1:], "mesh's flat constants are the textured ones")
    check(not P.get("vsec") is None and B.get("vsec") is not None, "a launch at each texture change")

    # the ring layout of a draw
    pv_prefix = m["block_at"] - pv_ring["devva"]
    check(m["desc"] == m["block_at"] + 0xa0 and A["vprog"] == m["desc"] + 0x40, "first draw's ring items")
    check(A["block_at"] == A["vprog"] + 0x60 and A["desc"] == A["block_at"] + 0x80, "textured draw's ring items")
    check(B["block_at"] == B["vprog"] + 0x60 and B["desc"] == B["block_at"] + 0x60, "4-word block's ring items")

    # MTE entries
    ent = {}
    for d, n in ((m, 18), (A, 9), (B, 4)):
        e = mix.words(0, d["entry"], 32)
        check(e[0] == d["block_at"] and not any(e[2:4]) and not any(e[5:12]) and not any(e[20:]), "MTE entry layout")
        check(d["mte"][2][1] & 0xFF == (n + 3) >> 2, "MTE launch word for %d words" % n)
        ent[n] = (e[1], e[4])
        code = e[12:20]
    check(mix.words(0, A["entry"], 32)[12:20] == code, "MTE entry code")
    mte_code = code
    bk = draws_of(back, 0, stream_a)[1]
    check([len(block(d, back)) for d in bk] == [25, 9, 9, 9, 4], "mix_back state blocks")
    flat9, tex9 = block(bk[2], back), block(bk[3], back)
    e9 = back.words(0, bk[2]["entry"], 32)
    check((e9[1], e9[4]) == ent[9], "MTE copy words of a 9-word block")
    tex9_t, tex4_t = list(ba), list(bb)
    check(tex9[:3] == ba[:3] and tex9[4] == ba[4] and tex9[6:] == ba[6:], "textured block after a flat draw")
    tex9_t[3] = tex9_t[5] = 0
    tex4_t[1] = tex4_t[3] = 0
    check(bp[0] == bb[0] and bp[2] == bb[2], "4-word block")
    check(flat9[1] == first_flat[1] and flat9[3:6] == first_flat[3:6], "flat block after a textured draw")

    # VDM packets
    idx = m["index"]
    index_table = idx[1]
    check(index_table == mix.at(index_table)["devva"] and mix.at(index_table)["size"] == 2048, "index table")
    check(idx[0] == 0x81C00000 | 516 and A["index"][0] == 0x81C00006 and P["index"][0] == 0x81C00000 | 192, "counts")
    index_words = [0x81C00000, 0, idx[2], idx[3], idx[4] & 0xFFFF0000, idx[5]]
    check(all(d["index"][2:4] == idx[2:4] and d["index"][5] == idx[5] and d["index"][4] >> 16 == idx[4] >> 16
              and d["index"][1] == index_table for d in draws), "index lists")
    launches = [d["mte"][2][1] for d in draws]
    check([w & ~0xFF for w in launches] == [MTE_WORD] * 4, "MTE launches")
    # launch k of the frame has toggle k & 1, the two background objects being 0 and 1
    check(launches == [MTE_WORD | (k & 1) | (n + 3) >> 2 for k, n in enumerate((18, 9, 4, 4))], "MTE launch words")
    end_block = endp[2]
    check(len(end_block) == 7, "end block")

    # --- the TA and 3D status lists: which entries are ring ends
    def statuses(rec, k):
        f = rec.fields(k)
        return [(s["addr"], s["value"]) for s in f["TAStatus"]], [(s["addr"], s["value"]) for s in f["3DStatus"]]

    ta0, r3d0 = statuses(mix, 0)
    ta1, r3d1 = statuses(mix, 1)
    mta, m3d = statuses(mesh, 0)
    ta_ends = [i for i in range(len(ta0)) if ta0[i][1] != mta[i][1]]
    d3_ends = [i for i in range(len(r3d0)) if r3d0[i][1] != m3d[i][1]]
    check([a for a, _ in ta0] == [a for a, _ in mta] and [a for a, _ in ta0] == [a for a, _ in ta1], "status addresses")
    ta_vdm, ta_pv, ta_mt = ta_ends
    pp_end_i, = d3_ends
    check(ta0[ta_vdm][1] == endp[1] + 28 - stream_a and ta1[ta_vdm][1] == ta0[ta_vdm][1] + 28, "VDM ends")
    check(ta0[ta_pv][1] == P["desc"] + 0x3c - pv_ring["devva"], "PDS vertex ring end")
    check(ta0[ta_mt][1] == P["entry"] + 0x80 - mt_ring["devva"], "MTE ring end")
    check(r3d0[pp_end_i][1] == secs[2] + 0x3c - pp_ring["devva"], "pixel ring end")
    seq_ta = [i for i in range(len(ta0)) if [read2.fields(k)["TAStatus"][i]["value"] for k in range(4)] == [1, 2, 3, 4]]
    seq_3d = [i for i in range(len(r3d0)) if [read2.fields(k)["3DStatus"][i]["value"] for k in range(4)] == [1, 1, 2, 2]]
    check(len(seq_ta) == 1 and len(seq_3d) == 1, "sequence status words")
    seq_ta, seq_3d = seq_ta[0], seq_3d[0]

    # --- allocations: mix's GL context; the transfer context's become holes, textures are the caller's
    tex_allocs = set()
    for t in textures:
        a = mix.at(t)
        tex_allocs.add(a["index"])
        if a["sync_alloc"] is not None:
            tex_allocs.add(a["sync_alloc"])
    check(sorted(tex_allocs) == list(range(len(mix.allocs) - len(tex_allocs), len(mix.allocs))),
          "the textures are allocated last")
    table, open_hole = [], {}

    def kind(a):
        if a["tag"] == "sync":
            return "SYNC"
        if a["tag"] == "HW render context":
            return "CONTEXT"
        check(a["allocator"] != "kernel", "kernel allocation %d of unknown kind" % a["index"])
        return "USER"

    for a in mix.allocs:
        if a["index"] in tex_allocs:
            continue
        check(a["freed_at_bridge_call"] is None, "allocation %d is freed" % a["index"])
        if a.get("group") == "transfer-context":
            h = open_hole.get(a["heap_id"])
            if h is not None:
                h["size"] = a["devva"] + a["size"] - h["addr"]
                continue
            t = dict(index=a["index"], heap=a["heap_id"], size=a["size"], align=max(a["alignment"], 1), flags=0x0b,
                     addr=a["devva"], kind="HOLE")
            open_hole[a["heap_id"]] = t
            table.append(t)
            continue
        open_hole.pop(a["heap_id"], None)
        flags = a["request_attribs"] if kind(a) == "USER" else a["returned_flags"]
        check(flags & ~0x4f == 0, "allocation %d: flags 0x%x" % (a["index"], flags))
        size = a["size"]
        if a["devva"] == index_table:
            size = 2 * INDICES
        table.append(dict(index=a["index"], heap=a["heap_id"], size=size, align=max(a["alignment"], 1), flags=flags,
                          addr=a["devva"], kind=kind(a)))
    verts = [t for t in table if t["kind"] == "HOLE" and t["heap"] == 0]
    check(len(verts) == 1, "one hole in General")
    verts = verts[0]

    prev = {}
    for a in init["allocations"]:
        if not a["freed"]:
            prev[a["heap_id"]] = a["devva"] + a["size"]
    bases = {h["id"]: h["base"] for h in mix.m["heap_table"]}
    for t in table:
        p = prev.get(t["heap"])
        if p is None:
            addr = bases[t["heap"]]
        else:
            al = max(t["align"], PAGE)
            addr = (((p + PAGE - 1) // PAGE * PAGE) + al - 1) // al * al
        check(addr == t["addr"], "allocation %d: policy gives 0x%x, recorded 0x%x" % (t["index"], addr, t["addr"]))
        prev[t["heap"]] = addr + t["size"]
    check(min(t["addr"] for t in table if t["heap"] == 0 and t is not verts) >= verts["addr"] + verts["size"],
          "the vertex buffer comes first in General")

    # --- roles, from the kicks (as tri_read: same command template)
    hwctx = k0["sCommand"]["ui32Data"][1]
    check([t for t in table if t["kind"] == "CONTEXT"][0]["addr"] == hwctx, "HW context")
    pd_off = 8
    ccb = by_index[mix.kicks[0]["ccb_alloc"]]["devva"]
    check(k0["ui32CCBOffset"] == SHARED and k1["ui32CCBOffset"] == CMD_SIZE + SHARED, "CCB offsets")
    hw = img[mix.at(hwctx)["index"]]
    ccb_ctl, pbdesc = u32(hw, 0x10), u32(hw, 0x18)
    details, synclist, dataset = u32(cmd0, 0x4c), u32(cmd0, 0x50), u32(cmd0, 0x54)
    details_access = u32(img[mix.at(details)["index"]], 0x88)
    synclist_access = u32(img[mix.at(synclist)["index"]], 0)
    rt_pending = u32(img[mix.at(dataset)["index"]], 0)
    check(k0["sHWRTDataSetDevAddr"] == dataset, "dataset")
    ta3d_sync = by_index[mix.kicks[1]["sync_objects"]["named"]["ta3d"]]["devva"]
    dst = mix.kicks[0]["sync_objects"]["dst"]
    check(len(dst) == 1 and k0["ui32NumDstSyncObjects"] == 1, "one destination sync object")
    surface_sync = by_index[dst[0]]["devva"]
    check(k0["ui32NumSrcSyncs"] == 0 and k1["ui32NumSrcSyncs"] == 0 and k1["bTADependency"] == 1, "sync use")
    for r in (read, mesh, tq):
        check(r.cmd(0) == cmd0, "%s's first command is mix's" % r.name)
    check(read.cmd(1) == mesh.cmd(1) and u32(read.cmd(1), 0x4e4) == stream_a + 0x84, "last command")

    def cmd_diff(ca, cb):
        return {o: (u32(ca, o), u32(cb, o)) for o in range(0, CMD_SIZE, 4) if ca[o:o + 4] != cb[o:o + 4]}

    check(set(cmd_diff(cmd0, cmd1)) == {0x004, 0x01c, 0x4e0, 0x4e4}, "kick 0 to 1")
    c2, c3 = read2.cmd(2), read2.cmd(3)
    check(set(cmd_diff(read2.cmd(0), c2)) == {0x004, 0x010, 0x058, 0x4e4, 0x4ec}, "frame 2 first kick")
    check(set(cmd_diff(read2.cmd(1), c3)) == {0x010, 0x058, 0x4e4, 0x4ec}, "frame 2 last kick")
    rt1 = (u32(cmd0, 0x58), u32(cmd0, 0x4ec))
    rt0 = (u32(c2, 0x58), u32(c2, 0x4ec))
    check(rt0[0] == dataset + 0x18 and rt1[0] == dataset + 0x18 + 0x34, "RTDATA addresses")
    cmd_words = [(o, u32(cmd0, o)) for o in range(0, CMD_SIZE, 4)
                 if u32(cmd0, o) and o not in (0x004, 0x058, 0x4e4, 0x4ec) and not SHARED <= o < SHARED + 0x468]
    det = img[mix.at(details)["index"]]
    check(u32(det, 0x18) == rt1[1] and u32(det, 0x90) == dataset and u32(det, 0x94) == rt1[0]
          and u32(det, 0xc8) == 0, "details")
    det_changes = {a - details for a in diff(read2, 0, read2, 2) if details <= a < details + 0x410}
    check(det_changes == {0x18, 0x54, 0x94}, "details change in frame 2 at %s" % sorted(map(hex, det_changes)))
    regs3d = list(struct.unpack_from("<34I", det, 0))
    regs3d[0x18 // 4] = 0

    # --- per-frame sites
    def sites(d, n, what):
        check(len(d) == n, "%s: %d words changed, expected %d: %s" % (
            what, len(d), n, ", ".join("%08x" % a for a in sorted(d))))
        return sorted(d)

    clear = sites(diff(tq, 0, color, 0), 6, "tri_color_tq")
    check(all(mix.word(0, a) == 0xff4080bf for a in clear), "clear colour words")
    check([b - a for a, b in zip(clear, clear[1:])] == [16] * 5, "clear colour stride")

    vbo_tri = read.at(read.word(0, flat_prog))
    d = diff(read, 0, two, 0)
    d = {a: v for a, v in d.items() if not vbo_tri["devva"] <= a < vbo_tri["devva"] + vbo_tri["size"]}
    count = sites(d, 1, "tri_two_tq index count")[0]
    check(d[count] == (0x81c00003, 0x81c00006) and count == stream_a + prefix_vdm + 16, "index count word")

    bl = diff(tq, 0, blend, 0)
    sites(bl, 4, "tri_blend_tq")
    bl_state = [a for a in bl if mix.at(a)["heap_id"] == 7]
    bl_code = sorted(a for a in bl if mix.at(a)["heap_id"] == 4)
    check(bl_state == [m["block_at"] + 4] and len(bl_code) == 3 and bl_code == [bl_code[0] + 4 * i for i in range(3)],
          "blend sites")
    isp = [bl[bl_state[0]][0], bl[bl_state[0]][1]]
    check(isp == [first_flat[1], ba[1]] and isp[0] == flat9[1], "the ISP word: blend off in flat blocks, on in textured")
    blend_off = [bl[a][0] for a in bl_code]
    blend_on = [bl[a][1] for a in bl_code]

    dd = diff(read, 0, read, 1)
    term = sorted(a for a in dd if mix.at(a)["heap_id"] not in (3, 8))
    check(len(term) == 7 and term == [stream_a + 0x84 + 4 * i for i in range(7)], "end-of-scene stream")
    check([dd[a][1] for a in term] == end_block and read.words(0, stream_a + 0x68, 7) == end_block, "end block")

    launch = stream_a + 4
    w = mix.word(0, launch)
    check(mix.word(0, stream_a) >> 28 == 4 and w == MTE_WORD | 2, "first background launch")
    check(read2.word(2, u32(c2, 0x4e4) + 4) == w | 1, "the MTE toggle in tri_read2_tq's frame 2")
    check(mix2.word(2, u32(mix2.cmd(2), 0x4e4) + 4) == w, "no toggle in mix2's frame 2 (six launches a frame)")
    bg2 = mix.word(0, stream_a + 0x24)
    check(bg2 & 1 and read2.word(2, u32(c2, 0x4e4) + 0x24) == bg2, "second background launch")

    # render target
    pbuf = by_index[77]
    check(pbuf["size"] == 320 * 320 * 4 and pbuf["heap_id"] == 0 and pbuf["devva"] == read.allocs[77]["devva"], "pbuffer")
    G = gap.allocs[77]["devva"] - pbuf["devva"]
    pbe, bgtex, rest = [], [], []
    rimg = read.image(0)
    for k in (0, 1):
        for x, y in zip(read.gl(), gap.gl()):
            if x["index"] not in read.image(k):
                continue
            a, b = read.image(k)[x["index"]], gap.image(k)[y["index"]]
            for o in range(0, len(a) & ~3, 4):
                if a[o:o + 4] == b[o:o + 4]:
                    continue
                u, v = u32(a, o), u32(b, o)
                if x["heap_id"] == 4:
                    i = o & ~7
                    u, v = immediate(rimg[x["index"]], i), immediate(gap.image(0)[y["index"]], i)
                    if pbuf["devva"] <= u < pbuf["devva"] + pbuf["size"] and v - u == G:
                        if x["devva"] + i not in pbe:
                            pbe.append(x["devva"] + i)
                        continue
                elif pbuf["devva"] <= u < pbuf["devva"] + pbuf["size"] and v - u == G:
                    if x["devva"] + o not in bgtex:
                        bgtex.append(x["devva"] + o)
                    continue
                if v - u != G:
                    rest.append((x["index"], o))
    check(not rest, "words that move with the General heap by another amount: %s" % rest)
    check(len(pbe) == 2 and len(bgtex) == 1, "target sites %s %s" % (pbe, bgtex))
    for p in pbe:
        check(immediate(img[mix.at(p)["index"]], p - mix.at(p)["devva"]) == pbuf["devva"], "PBE")
        check(u32(img[mix.at(p)["index"]], p - mix.at(p)["devva"]) >> 21 == 3, "tile-store register")
    check(mix.word(0, bgtex[0]) == pbuf["devva"], "background texture word")
    check(pp_ring["devva"] <= bgtex[0] < pp_ring["devva"] + pp_prefix, "the background texture is in the pixel prefix")

    # --- the generated tables
    links = by_index[3]
    lb = img[3]
    pages = by_index[2]["size"] // PAGE
    first_page = (by_index[2]["devva"] - 0x0b800000) >> 12
    for i in range(pages):
        prev_id = 0xffff if i == 0 else first_page + i - 1
        next_id = 0xffff if i == pages - 1 else first_page + i + 1
        check(u32(lb, 4 * (first_page + i)) == next_id | prev_id << 16, "page link %d" % i)
    check(u32(img[0], 0x10) == links["devva"], "page link table address")
    check(img[mix.at(index_table)["index"]] == struct.pack("<1024H", *range(1024)), "identity indices")

    # --- the textured program's code: in every mix scene, absent where only the flat one was compiled
    code_d = diff(mesh, 0, mix, 0)
    usse = sorted(a for a in code_d if mix.at(a)["heap_id"] in (4, 5))
    check(all(code_d[a][0] == 0 for a in usse), "textured code over other code")
    tex_code = []
    for a in usse:
        if tex_code and a == tex_code[-1][0] + 4 * tex_code[-1][1]:
            tex_code[-1][1] += 1
        else:
            tex_code.append([a, 1])
    check([n for _, n in tex_code] == [4, 10], "textured code: %s" % tex_code)

    # --- the image: mix at its first kick less what the native code writes
    native = set()

    def own(addr, n=1):
        for i in range(n):
            native.add(addr + 4 * i)

    vbos = {mix.at(qvbo)["devva"], mix.at(vf[0])["devva"]}
    for t in table:
        a = by_index[t["index"]]
        if t["kind"] in ("SYNC", "HOLE") or t["addr"] in (ccb, ccb_ctl, rt_pending, details_access, synclist_access,
                                                         index_table, links["devva"]) or t["addr"] in vbos:
            own(t["addr"], (a["size"] + 3) // 4)
    own(hwctx + pd_off)
    own(details, 0x88 // 4)
    own(details + 0x90, 2)
    own(details + 0xc8)
    own(synclist + 4, 7)
    for ring, pre in ((vdm, prefix_vdm), (pv_ring, pv_prefix), (pp_ring, pp_prefix), (pc_chunk, pc_prefix)):
        own(ring["devva"] + pre, (ring["size"] - pre) // 4)
    # every MTE-copy entry has its code from the start; the rest is per draw
    mt_code = mix.words(0, mt_ring["devva"], mt_ring["size"] // 4)
    for e in range(0x100, mt_ring["size"], 0x80):
        check(mt_code[(e + 0x30) // 4:(e + 0x50) // 4] == mte_code, "MTE entry code at +0x%x" % e)
        own(mt_ring["devva"] + e, 12)
        own(mt_ring["devva"] + e + 0x50, 12)
    own(flat_prog)
    own(flat_prog + 8)
    for a in clear + [launch] + bl_code + bgtex:
        own(a)

    runs, words = [], []
    for t in table:
        idx_ = t["index"]
        if idx_ not in img or t["kind"] == "HOLE":
            continue
        b = bytearray(img[idx_])
        for o in range(0, len(b), 4):
            if t["addr"] + o in native:
                b[o:o + 4] = bytes(4)
        n = len(b) // 4
        ws = struct.unpack("<%dI" % n, bytes(b[:4 * n]))
        i = 0
        alloc_runs = []
        while i < n:
            if not ws[i]:
                i += 1
                continue
            j = i
            while j < n and (ws[j] or any(ws[j:j + 3])):
                j += 1
            while not ws[j - 1]:
                j -= 1
            alloc_runs.append((t["addr"] + 4 * i, list(ws[i:j])))
            i = j
        k = 0
        while k < len(alloc_runs):
            addr, data = alloc_runs[k]
            times, step = 1, 0
            if k + 1 < len(alloc_runs) and alloc_runs[k + 1][1] == data:
                step = alloc_runs[k + 1][0] - addr
                while (k + times < len(alloc_runs) and alloc_runs[k + times][1] == data
                       and alloc_runs[k + times][0] == addr + times * step):
                    times += 1
            check(len(data) < 65536 and times < 65536 and step < 65536, "run too long")
            runs.append((addr, len(data), times, step, len(words)))
            words.extend(data)
            k += times
    check(len(words) < 65536, "too many words")
    back_img = {t["index"]: bytearray(len(img[t["index"]])) for t in table if t["index"] in img and t["kind"] != "HOLE"}
    for addr, n, times, step, at in runs:
        a = mix.at(addr)
        for k in range(times):
            o = addr + k * step - a["devva"]
            back_img[a["index"]][o:o + 4 * n] = struct.pack("<%dI" % n, *words[at:at + n])
    for t in table:
        if t["index"] in back_img:
            for o in range(0, len(back_img[t["index"]]) & ~3, 4):
                want = 0 if t["addr"] + o in native else u32(img[t["index"]], o)
                check(u32(back_img[t["index"]], o) == want, "run data at 0x%08x" % (t["addr"] + o))

    # the image is every recording's too, where the native code does not write
    tex_words = {a + 4 * i for a, n in tex_code for i in range(n)}
    for r in (mesh, tq, read, Rec("mix_minus"), Rec("mix_swap"), back):
        own_r = set(native)
        if r.name in ("mesh", "tri_tq", "tri_read_tq"):
            for x in r.gl():
                if x["heap_id"] == 0 and x["devva"] >= min(vbos):
                    own_r |= {x["devva"] + o for o in range(0, x["size"], 4)}
        bad = [a for a in diff(mix, 0, r, 0) if a not in own_r and a not in tex_words]
        check(not bad, "%s differs from the image at %s" % (r.name, " ".join("%08x" % a for a in bad[:8])))

    data_bytes = 12 * len(runs) + 4 * len(words)
    write_h(locals())
    write_c(locals())
    print("mkreplay: %d allocations, %d runs, %d words: %d bytes of image data" % (
        len(table), len(runs), len(words), data_bytes))


def write_h(v):
    rt0, rt1 = v["rt0"], v["rt1"]
    lines = [
        "/* Generated by tools/sgx/mkreplay.py from the GLES recordings in",
        " * build/sgx/draw; do not edit. */",
        "#ifndef SGX_SCENE_DATA_H",
        "#define SGX_SCENE_DATA_H",
        "#include <stdint.h>",
        "",
        "enum { SGX_SCENE_USER, SGX_SCENE_SYNC, SGX_SCENE_CONTEXT, SGX_SCENE_HOLE };",
        "",
        "struct sgx_scene_alloc {",
        "\tuint32_t addr, size;",
        "\tuint16_t align;",
        "\tuint8_t heap, flags, kind;",
        "};",
        "",
        "/* words from data, repeated times over, step bytes apart */",
        "struct sgx_scene_run {",
        "\tuint32_t addr;",
        "\tuint16_t words, times, step, data;",
        "};",
        "",
        "struct sgx_scene_status {",
        "\tuint32_t addr, value;",
        "};",
        "",
        "struct sgx_scene_word {",
        "\tuint16_t off;",
        "\tuint32_t value;",
        "};",
        "",
    ]

    def d(name, val):
        lines.append("#define SGX_SCENE_%-20s 0x%08x" % (name, val))

    d("ALLOCS", len(v["table"]))
    d("RUNS", len(v["runs"]))
    d("CMD_WORDS", len(v["cmd_words"]))
    d("TA_STATUS", len(v["ta0"]))
    d("3D_STATUS", len(v["r3d0"]))
    d("TA_SEQ", v["seq_ta"])
    d("3D_SEQ", v["seq_3d"])
    d("TA_VDM", v["ta_vdm"])
    d("TA_PV", v["ta_pv"])
    d("TA_MT", v["ta_mt"])
    d("3D_PP", v["pp_end_i"])
    lines.append("")
    d("CCB", v["ccb"])
    d("CCB_CTL", v["ccb_ctl"])
    d("HW_CONTEXT", v["hwctx"])
    d("PD_OFFSET", v["pd_off"])
    d("PB_DESC", v["pbdesc"])
    d("PB_LINKS", v["links"]["devva"])
    d("PB_FIRST_PAGE", v["first_page"])
    d("PB_PAGES", v["pages"])
    d("DETAILS", v["details"])
    d("DETAILS_ACCESS", v["details_access"])
    d("SYNC_LIST", v["synclist"])
    d("SYNC_LIST_ACCESS", v["synclist_access"])
    d("DATASET", v["dataset"])
    d("RT_PENDING", v["rt_pending"])
    d("RTDATA0", rt0[0])
    d("REGIONS0", rt0[1])
    d("RTDATA1", rt1[0])
    d("REGIONS1", rt1[1])
    d("TA3D_SYNC", v["ta3d_sync"])
    d("SURFACE_SYNC", v["surface_sync"])
    lines.append("")
    d("PBUFFER", v["pbuf"]["devva"])
    d("VERTICES", v["verts"]["addr"])
    d("VERTICES_SIZE", v["verts"]["size"])
    d("INDICES", v["index_table"])
    d("INDEX_TABLE", INDICES)
    d("FLAT_PROGRAM", v["flat_prog"])
    d("CLEAR", v["clear"][0])
    d("CLEARS", len(v["clear"]))
    d("CLEAR_STEP", v["clear"][1] - v["clear"][0])
    d("MTE_LAUNCH", v["launch"])
    d("BLEND_CODE", v["bl_code"][0])
    d("TILE_STORE0", v["pbe"][0])
    d("TILE_STORE1", v["pbe"][1])
    d("BG_TEXTURE", v["bgtex"][0])
    d("TEX_FS_CODE", v["tex_code"][0][0])
    d("TEX_FS_WORDS", v["tex_code"][0][1])
    d("TEX_VS_CODE", v["tex_code"][1][0])
    d("TEX_VS_WORDS", v["tex_code"][1][1])
    lines.append("")
    lines.append("/* The rings a frame is written into, from frame one's place: the")
    lines.append(" * part before the first draw (the clear) is in the image. */")
    for name, ring, pre in (("VDM", v["vdm"], v["prefix_vdm"]), ("PV", v["pv_ring"], v["pv_prefix"]),
                            ("MT", v["mt_ring"], 0x100), ("PP", v["pp_ring"], v["pp_prefix"]),
                            ("PC", v["pc_chunk"], v["pc_prefix"])):
        d(name, ring["devva"])
        d(name + "_SIZE", ring["size"])
        d(name + "_PREFIX", pre)
    d("BG_LAUNCHES", 2)
    d("VERTEX_BASE", PDS_VERT_BASE)
    d("PIXEL_BASE", PDS_PIX_BASE)
    d("LAUNCH", LAUNCH)
    d("MTE_WORD", MTE_WORD)
    d("VSEC_WORD", VSEC_WORD)
    d("PROGRAM_WORD", 0x0c000000)
    d("TEX_FORMAT", v["tex_format"])
    lines.append("")
    d("STREAM_FIRST", v["stream_a"])
    d("A_FIRST_EVER", u32(v["cmd0"], 0x004))
    d("A_FIRST", u32(v["c2"], 0x004))
    d("A_LAST", u32(v["cmd1"], 0x004))
    d("B_LAST", u32(v["cmd1"], 0x01c))
    d("END_LAST", u32(v["cmd1"], 0x4e0))
    lines += [
        "",
        "extern const struct sgx_scene_alloc sgx_scene_allocs[SGX_SCENE_ALLOCS];",
        "extern const struct sgx_scene_run sgx_scene_runs[SGX_SCENE_RUNS];",
        "extern const uint32_t sgx_scene_words[];",
        "extern const struct sgx_scene_word sgx_scene_cmd[SGX_SCENE_CMD_WORDS];",
        "extern const uint32_t sgx_scene_regs3d[34];",
        "extern const uint32_t sgx_scene_end[7];",
        "extern const uint32_t sgx_scene_blend[2][3];",
        "extern const uint32_t sgx_scene_isp[2];",
        "extern const struct sgx_scene_status sgx_scene_ta_status[2][SGX_SCENE_TA_STATUS];",
        "extern const struct sgx_scene_status sgx_scene_3d_status[2][SGX_SCENE_3D_STATUS];",
        "",
        "/* State blocks: the first draw's (flat, with the viewport), a flat",
        " * one after a textured draw, a textured one after a flat draw (pixel",
        " * programs in words 3 and 5) and after a textured one (1 and 3). */",
        "extern const uint32_t sgx_scene_first_flat[18];",
        "extern const uint32_t sgx_scene_flat_block[9];",
        "extern const uint32_t sgx_scene_tex_block9[9];",
        "extern const uint32_t sgx_scene_tex_block4[4];",
        "/* MTE-copy entry: copy and destination words by block size, code. */",
        "extern const uint32_t sgx_scene_mte_copy[3][3];",
        "extern const uint32_t sgx_scene_mte_code[8];",
        "/* Vertex programs: data (stream addresses in words 0 and 2), code. */",
        "extern const uint32_t sgx_scene_vprog_flat[12];",
        "extern const uint32_t sgx_scene_vprog_tex[12];",
        "extern const uint32_t sgx_scene_vprog_code[6];",
        "/* Vertex-secondary constants and the descriptor of both secondary",
        " * programs (address first), and their code. */",
        "extern const uint32_t sgx_scene_vsec_const[10];",
        "extern const uint32_t sgx_scene_vsec_desc[2];",
        "extern const uint32_t sgx_scene_psec_desc[2];",
        "extern const uint32_t sgx_scene_sec_code[3];",
        "/* A texture's pixel program: sampling in word 2, size in 3, address",
        " * in 10; sampling nearest and linear. */",
        "extern const uint32_t sgx_scene_tex_program[16];",
        "extern const uint32_t sgx_scene_sampling[2];",
        "extern const uint32_t sgx_scene_index_list[6];",
        "#endif",
    ]
    with open(OUT_H, "w") as f:
        f.write("\n".join(lines) + "\n")


def hexwords(ws, per=6):
    return ["\t" + " ".join("0x%08x," % w for w in ws[i:i + per]) for i in range(0, len(ws), per)]


def write_c(v):
    kinds = {"USER": "SGX_SCENE_USER", "SYNC": "SGX_SCENE_SYNC", "CONTEXT": "SGX_SCENE_CONTEXT", "HOLE": "SGX_SCENE_HOLE"}
    lines = [
        "/* Generated by tools/sgx/mkreplay.py from the GLES recordings in",
        " * build/sgx/draw; do not edit. */",
        "#include \"sgx_scene_data.h\"",
        "",
        "const struct sgx_scene_alloc sgx_scene_allocs[SGX_SCENE_ALLOCS] = {",
    ]
    for t in v["table"]:
        lines.append("\t{ 0x%08x, 0x%06x, 0x%04x, %2d, 0x%02x, %s }," % (
            t["addr"], t["size"], t["align"], t["heap"], t["flags"], kinds[t["kind"]]))
    lines += ["};", "", "const struct sgx_scene_run sgx_scene_runs[SGX_SCENE_RUNS] = {"]
    for r in v["runs"]:
        lines.append("\t{ 0x%08x, %4d, %3d, 0x%03x, %4d }," % r)
    lines += ["};", "", "const uint32_t sgx_scene_words[] = {"] + hexwords(v["words"]) + ["};", ""]
    lines += ["const struct sgx_scene_word sgx_scene_cmd[SGX_SCENE_CMD_WORDS] = {"]
    for o, w in v["cmd_words"]:
        lines.append("\t{ 0x%03x, 0x%08x }," % (o, w))
    lines += ["};", ""]

    def arr(decl, ws, per=6):
        lines.extend(["const uint32_t %s = {" % decl] + hexwords(ws, per) + ["};", ""])

    arr("sgx_scene_regs3d[34]", v["regs3d"])
    arr("sgx_scene_end[7]", v["end_block"], 7)
    lines += ["const uint32_t sgx_scene_blend[2][3] = {",
              "\t{ " + ", ".join("0x%08x" % w for w in v["blend_off"]) + " },",
              "\t{ " + ", ".join("0x%08x" % w for w in v["blend_on"]) + " },", "};", ""]
    arr("sgx_scene_isp[2]", v["isp"])
    for name, lists, n in (("ta", (v["ta0"], v["ta1"]), "SGX_SCENE_TA_STATUS"),
                           ("3d", (v["r3d0"], v["r3d1"]), "SGX_SCENE_3D_STATUS")):
        lines.append("const struct sgx_scene_status sgx_scene_%s_status[2][%s] = {" % (name, n))
        for lst in lists:
            lines.append("\t{ " + ", ".join("{ 0x%08x, %d }" % s for s in lst) + " },")
        lines += ["};", ""]
    arr("sgx_scene_first_flat[18]", v["first_flat"])
    arr("sgx_scene_flat_block[9]", v["flat9"])
    arr("sgx_scene_tex_block9[9]", v["tex9_t"])
    arr("sgx_scene_tex_block4[4]", v["tex4_t"])
    ent = v["ent"]
    lines += ["const uint32_t sgx_scene_mte_copy[3][3] = {"]
    for n in (4, 9, 18):
        lines.append("\t{ %d, 0x%08x, 0x%08x }," % ((n,) + ent[n]))
    lines += ["};", ""]
    arr("sgx_scene_mte_code[8]", v["mte_code"], 4)
    arr("sgx_scene_vprog_flat[12]", v["vprog_flat"])
    arr("sgx_scene_vprog_tex[12]", v["vprog_tex"])
    arr("sgx_scene_vprog_code[6]", v["vprog_code"])
    arr("sgx_scene_vsec_const[10]", v["vsec_const"], 5)
    arr("sgx_scene_vsec_desc[2]", v["vsec_desc"])
    arr("sgx_scene_psec_desc[2]", v["psec_desc"])
    arr("sgx_scene_sec_code[3]", v["vsec_code"])
    arr("sgx_scene_tex_program[16]", v["prim_t"], 4)
    arr("sgx_scene_sampling[2]", [v["near"], v["lin"]])
    arr("sgx_scene_index_list[6]", v["index_words"])
    with open(OUT_C, "w") as f:
        f.write("\n".join(lines).rstrip("\n") + "\n")


if __name__ == "__main__":
    main()
