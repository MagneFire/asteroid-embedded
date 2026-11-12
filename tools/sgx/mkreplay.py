#!/usr/bin/env python3
"""Writes src/gpu/sgx_scene_data.[ch], the scene the native SGX530 triangle path
(src/gpu/sgx_draw.c) replays, from the GLES recordings of glrec.py.

    mkreplay.py [DRAW_DIR]        (default build/sgx/draw)

Needs the recordings tri, tri_read, tri_color, tri_moved, tri_blend, tri_two,
tri_read2 and tri_read_gap (glrec.py --general-gap 0x100000 tri_read).

Every value the C code uses is taken from a recording, and every place the
native code writes per frame is found by comparing two recordings that differ
in one input; the script stops if a comparison shows more or less than it
expects. The image is tri_read's memory at its first kick, less every word the
native code writes itself (per frame, or as the kernel driver would); runs of
non-zero words are stored, a run repeated at a constant step once.
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

MAX_TRIS = 56           # the vertex buffer stays within its recorded page
VERTEX_BYTES = 24       # x, y, r, g, b, a
CMD_SIZE = 0x540
SHARED = 0x64
PAGE = 4096


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


def diff(ra, ka, rb, kb, skip=()):
    """Changed words between two images, as {address: (a, b)}."""
    out = {}
    ia, ib = ra.image(ka), rb.image(kb)
    for x, y in zip(ra.allocs, rb.allocs):
        check(x["devva"] == y["devva"] and x["heap_id"] == y["heap_id"],
              "%s and %s place allocation %d differently" % (ra.name, rb.name, x["index"]))
        if x["index"] in skip or x["index"] not in ia:
            continue
        a, b = ia[x["index"]], ib[y["index"]]
        n = min(len(a), len(b))
        for o in range(0, n, 4):
            if a[o:o + 4] != b[o:o + 4]:
                out[x["devva"] + o] = (u32(a, o), u32(b, o))
        for o in range(n, max(len(a), len(b)), 4):
            src = a if len(a) > n else b
            if u32(src, o):
                out[x["devva"] + o] = (u32(a, o) if len(a) > o else 0, u32(b, o) if len(b) > o else 0)
    return out


def cmd_diff(ca, cb):
    return {o: (u32(ca, o), u32(cb, o)) for o in range(0, CMD_SIZE, 4) if ca[o:o + 4] != cb[o:o + 4]}


def immediate(b, off):
    """The 32-bit constant in a USSE instruction (sgx_image.c use_constant)."""
    w0, w1 = u32(b, off), u32(b, off + 4)
    return (w0 & 0x1fffff) | ((w1 >> 4) & 0x1f) << 21 | ((w1 >> 12) & 0x3f) << 26


def main():
    tri, base, color, moved = Rec("tri"), Rec("tri_read"), Rec("tri_color"), Rec("tri_moved")
    blend, two, read2, gap = Rec("tri_blend"), Rec("tri_two"), Rec("tri_read2"), Rec("tri_read_gap")
    with open(os.path.join(WORKTREE, "build", "sgx", "init", "manifest.json")) as f:
        init = json.load(f)

    check(not diff(tri, 0, base, 0), "tri and tri_read differ at kick 0")
    check(len(base.kicks) == 2 and len(read2.kicks) == 4 and len(two.kicks) == 2, "unexpected kick counts")
    allocs = base.allocs
    by_index = {a["index"]: a for a in allocs}
    img = base.image(0)

    # --- allocations
    def kind(a):
        if a["tag"] == "sync":
            return "SYNC"
        if a["tag"] == "HW render context":
            return "CONTEXT"
        check(a["allocator"] != "kernel", "kernel allocation %d of unknown kind" % a["index"])
        return "USER"

    d = diff(base, 0, moved, 0)
    check(len({base.at(x)["index"] for x in d}) == 1, "tri_moved changes more than the vertex buffer")
    vbo = base.at(next(iter(d)))
    vbo_addr = vbo["devva"]
    check(vbo["size"] == (3 * VERTEX_BYTES + 31) // 32 * 32, "vertex buffer is not three vertices")

    table = []
    for a in allocs:
        check(a["freed_at_bridge_call"] is None, "allocation %d is freed" % a["index"])
        flags = a["request_attribs"] if kind(a) == "USER" else a["returned_flags"]
        check(flags & ~0x4f == 0, "allocation %d: flags 0x%x" % (a["index"], flags))
        size = a["size"]
        if a["devva"] == vbo_addr:
            size = MAX_TRIS * 3 * VERTEX_BYTES
        table.append(dict(index=a["index"], heap=a["heap_id"], size=size, align=max(a["alignment"], 1),
                          flags=flags, addr=a["devva"], kind=kind(a)))

    # the vendor's placement, continued from the init recording, gives the recorded addresses
    prev = {}
    for a in init["allocations"]:
        if not a["freed"]:
            prev[a["heap_id"]] = a["devva"] + a["size"]
    bases = {h["id"]: h["base"] for h in base.m["heap_table"]}
    for t in table:
        p = prev.get(t["heap"])
        if p is None:
            addr = bases[t["heap"]]
        else:
            al = max(t["align"], PAGE)
            addr = (((p + PAGE - 1) // PAGE * PAGE) + al - 1) // al * al
        check(addr == t["addr"], "allocation %d: policy gives 0x%x, recorded 0x%x" % (t["index"], addr, t["addr"]))
        prev[t["heap"]] = addr + t["size"]

    def alloc_of(addr):
        return base.at(addr)

    # --- roles, from the kicks
    k0, k1 = base.fields(0), base.fields(1)
    cmd0, cmd1 = base.cmd(0), base.cmd(1)
    hwctx = k0["sCommand"]["ui32Data"][1]
    check(by_index[[t for t in table if t["kind"] == "CONTEXT"][0]["index"]]["devva"] == hwctx, "HW context")
    reg = [c for c in base.m["bridge_calls"] if c.get("name") == "SGX_REGISTER_HW_RENDER_CONTEXT"]
    pd_off = reg[0]["decoded"]["pd_patch_offset"] if reg and "decoded" in reg[0] else 8
    check(pd_off == 8, "PD patch offset %d" % pd_off)
    ccb = by_index[base.kicks[0]["ccb_alloc"]]["devva"]
    check(k0["ui32CCBOffset"] == SHARED and k1["ui32CCBOffset"] == CMD_SIZE + SHARED, "CCB offsets")
    ccb_ctl = u32(img[base.at(hwctx)["index"]], 0x10)
    pbdesc = u32(img[base.at(hwctx)["index"]], 0x18)
    details, synclist, dataset = u32(cmd0, 0x4c), u32(cmd0, 0x50), u32(cmd0, 0x54)
    details_access = u32(img[alloc_of(details)["index"]], 0x88)
    synclist_access = u32(img[alloc_of(synclist)["index"]], 0)
    rt_pending = u32(img[alloc_of(dataset)["index"]], 0)
    check(k0["sHWRTDataSetDevAddr"] == dataset, "dataset")
    ta3d_sync = by_index[base.kicks[1]["sync_objects"]["named"]["ta3d"]]["devva"]
    dst = base.kicks[0]["sync_objects"]["dst"]
    check(len(dst) == 1 and k0["ui32NumDstSyncObjects"] == 1, "one destination sync object")
    surface_sync = by_index[dst[0]]["devva"]
    check(k1["bTADependency"] == 1 and k0["hTA3DSyncInfo"] == 0 and k1["ui32NumDstSyncObjects"] == 1, "sync use")
    check(k0["ui32NumSrcSyncs"] == 0 and k1["ui32NumSrcSyncs"] == 0 and not k0["hTASyncInfo"] and not k0["h3DSyncInfo"]
          and not k1["hTASyncInfo"] and not k1["h3DSyncInfo"], "unexpected sync objects")
    check(k0["bFirstKickOrResume"] == 1 and k0["bLastInScene"] == 0 and k1["bFirstKickOrResume"] == 0
          and k1["bLastInScene"] == 1, "first/last")
    check(k0["ui32FrameNum"] == 0 and all(read2.fields(i)["ui32FrameNum"] == 0 for i in range(4)), "frame number")

    # status lists; the sequence entries are the ones that count kicks and scenes in tri_read2
    ta = [[(s["addr"], s["value"]) for s in base.fields(k)["TAStatus"]] for k in (0, 1)]
    r3d = [[(s["addr"], s["value"]) for s in base.fields(k)["3DStatus"]] for k in (0, 1)]
    check(len(ta[0]) == len(ta[1]) and len(r3d[0]) == len(r3d[1]), "status counts")
    check([a for a, _ in ta[0]] == [a for a, _ in ta[1]] and [a for a, _ in r3d[0]] == [a for a, _ in r3d[1]],
          "status addresses")
    seq_ta = [i for i in range(len(ta[0]))
              if [read2.fields(k)["TAStatus"][i]["value"] for k in range(4)] == [1, 2, 3, 4]]
    seq_3d = [i for i in range(len(r3d[0]))
              if [read2.fields(k)["3DStatus"][i]["value"] for k in range(4)] == [1, 1, 2, 2]]
    check(len(seq_ta) == 1 and len(seq_3d) == 1, "sequence status words")
    seq_ta, seq_3d = seq_ta[0], seq_3d[0]

    # the command, and what changes per kick
    d01 = cmd_diff(cmd0, cmd1)
    check(set(d01) == {0x004, 0x01c, 0x4e0, 0x4e4}, "kick 0 to 1 changes %s" % sorted(map(hex, d01)))
    c2, c3 = read2.cmd(2), read2.cmd(3)
    d02, d13 = cmd_diff(read2.cmd(0), c2), cmd_diff(read2.cmd(1), c3)
    check(read2.cmd(0) == cmd0 and read2.cmd(1) == cmd1, "tri_read2 frame 1 is not tri_read")
    check(set(d02) == {0x004, 0x010, 0x058, 0x4e4, 0x4ec}, "frame 2 first kick changes %s" % sorted(map(hex, d02)))
    check(set(d13) == {0x010, 0x058, 0x4e4, 0x4ec}, "frame 2 last kick changes %s" % sorted(map(hex, d13)))
    rt1 = (u32(cmd0, 0x58), u32(cmd0, 0x4ec))
    rt0 = (u32(c2, 0x58), u32(c2, 0x4ec))
    check(rt0[0] == dataset + 0x18 and rt1[0] == dataset + 0x18 + 0x34, "RTDATA addresses")
    check(u32(c3, 0x58) == rt0[0] and u32(c3, 0x4ec) == rt0[1], "frame 2 kicks use one RT")
    cmd_words = [(o, u32(cmd0, o)) for o in range(0, CMD_SIZE, 4)
                 if u32(cmd0, o) and o not in (0x004, 0x058, 0x4e4, 0x4ec) and not SHARED <= o < SHARED + 0x468]
    check(all(u32(cmd0, o) == 0 for o in range(SHARED, SHARED + 0x468, 4)), "user space writes the shared part")
    stream_a, stream_b = u32(cmd0, 0x4e4), u32(cmd1, 0x4e4)

    # details: the 3D register block and the words a new scene sets
    det = img[alloc_of(details)["index"]]
    check(u32(det, 0x18) == rt1[1] and u32(det, 0x90) == dataset and u32(det, 0x94) == rt1[0]
          and u32(det, 0xc8) == 0, "details")
    dd = diff(read2, 0, read2, 2)
    det_changes = {a - details for a in dd if details <= a < details + 0x410}
    check(det_changes == {0x18, 0x54, 0x94}, "details change in frame 2 at %s" % sorted(map(hex, det_changes)))
    regs3d = list(struct.unpack_from("<34I", det, 0))
    regs3d[0x18 // 4] = 0

    # --- per-frame sites
    def sites(d, n, what):
        check(len(d) == n, "%s: %d words changed, expected %d: %s" % (
            what, len(d), n, ", ".join("%08x" % a for a in sorted(d))))
        return sorted(d)

    clear = sites(diff(base, 0, color, 0), 6, "tri_color")
    check(all(base.word(0, a) == 0xff4080bf for a in clear), "clear colour words")
    check([b - a for a, b in zip(clear, clear[1:])] == [16] * 5, "clear colour stride")

    d = diff(base, 0, two, 0)
    count = [a for a in d if not vbo_addr <= a < vbo_addr + 0x1000]
    count = sites({a: d[a] for a in count}, 1, "tri_two index count")[0]
    check(d[count] == (0x81c00003, 0x81c00006), "index count word")
    check(not diff(base, 1, two, 1, skip=(vbo["index"],)).keys() - {count}, "tri_two kick 1")

    bl = diff(tri, 0, blend, 0)
    sites(bl, 4, "tri_blend")
    bl_state = [a for a in bl if base.at(a)["heap_id"] == 7]
    bl_code = sorted(a for a in bl if base.at(a)["heap_id"] == 4)
    check(len(bl_state) == 1 and len(bl_code) == 3 and bl_code == [bl_code[0] + 4 * i for i in range(3)], "blend sites")
    bl_state = bl_state[0]
    blend_off = [bl[bl_state][0]] + [bl[a][0] for a in bl_code]
    blend_on = [bl[bl_state][1]] + [bl[a][1] for a in bl_code]

    d = diff(base, 0, base, 1)
    kernel_heaps = {3, 8}
    term = sorted(a for a in d if base.at(a)["heap_id"] not in kernel_heaps)
    check(len(term) == 7 and term == [stream_b + 4 * i for i in range(7)], "end-of-scene stream")
    term_words = [d[a][1] for a in term]
    check(term_words[-1] == 0xc0000000, "end-of-scene stream ends")

    vdm = base.at(stream_a)
    check(vdm["devva"] == stream_a, "the stream starts its ring")
    launch = stream_a + 4
    w = base.word(0, launch)
    check(base.word(0, stream_a) >> 28 == 4 and w & 0x28200 == 0x28200 and not w & 1, "first MTE launch")
    stream2 = u32(c2, 0x4e4)
    check(read2.word(2, stream2 + 4) == w | 1, "the MTE toggle in frame 2")
    for off in (0x24, 0x44):
        x = base.word(0, stream_a + off)
        check(x & 0x28200 == 0x28200 and x & 1 and read2.word(2, stream2 + off) == x, "MTE launch +0x%x" % off)

    # render target: what moves with it
    pbuf = by_index[10]
    check(pbuf["devva"] == 0x1000 and pbuf["size"] == 320 * 320 * 4, "pbuffer")
    gap_d = {}
    G = gap.allocs[10]["devva"] - pbuf["devva"]
    for k in (0, 1):
        for x, y in zip(base.allocs, gap.allocs):
            if x["index"] not in base.image(k):
                continue
            a, b = base.image(k)[x["index"]], gap.image(k)[y["index"]]
            for o in range(0, len(a), 4):
                if a[o:o + 4] != b[o:o + 4]:
                    gap_d.setdefault((x["index"], o), (u32(a, o), u32(b, o)))
    def in_target(x):
        return pbuf["devva"] <= x < pbuf["devva"] + pbuf["size"]

    pbe, bgtex, rest = [], [], []
    for (idx, o), (u, v) in sorted(gap_d.items()):
        a = by_index[idx]
        if a["heap_id"] == 4:
            # a USSE instruction: the address is its immediate
            i = o & ~7
            u, v = immediate(img[idx], i), immediate(gap.image(0)[idx], i)
            if in_target(u) and v - u == G:
                check(u32(img[idx], i) >> 21 == 3, "tile-store register")
                if a["devva"] + i not in pbe:
                    pbe.append(a["devva"] + i)
                continue
        elif in_target(u) and v - u == G:
            bgtex.append(a["devva"] + o)
            continue
        if v - u != G:
            rest.append((idx, o))
    check(not rest, "words that move with the General heap by another amount: %s" % rest)
    check(len(pbe) == 2 and len(bgtex) == 1, "target sites %s %s" % (pbe, bgtex))
    check(all(immediate(img[base.at(p)["index"]], p - base.at(p)["devva"]) == pbuf["devva"] for p in pbe), "PBE")
    check(base.word(0, bgtex[0]) == pbuf["devva"], "background texture word")
    tile = base.at(pbe[0])
    for p in pbe:
        o = p - tile["devva"]
        check(immediate(img[tile["index"]], o - 0x10) & 0xf0000000 == 0, "PBE format nibble")

    # --- the generated tables
    links = by_index[3]
    lb = img[3]
    pages = by_index[2]["size"] // PAGE
    first_page = (by_index[2]["devva"] - 0x0b800000) >> 12
    for i in range(pages):
        prev_id = 0xffff if i == 0 else first_page + i - 1
        next_id = 0xffff if i == pages - 1 else first_page + i + 1
        check(u32(lb, 4 * (first_page + i)) == next_id | prev_id << 16, "page link %d" % i)
    check(not any(lb[4 * (first_page + pages):]) and not any(lb[:4 * first_page]), "page link table")
    check(u32(img[0], 0x10) == links["devva"], "page link table address")
    ib = by_index[78]
    check(ib["size"] == 2048 and img[78] == struct.pack("<1024H", *range(1024)), "identity indices")

    # --- base image minus what the native code writes
    native = set()

    def own(addr, n=1):
        for i in range(n):
            native.add(addr + 4 * i)

    for t in table:
        if t["kind"] == "SYNC" or t["addr"] in (ccb, ccb_ctl, rt_pending, details_access, synclist_access,
                                                 vbo_addr, ib["devva"], links["devva"]):
            own(t["addr"], (by_index[t["index"]]["size"] + 3) // 4)
    own(hwctx + pd_off)
    own(details, 0x88 // 4)
    own(details + 0x90, 2)
    own(details + 0xc8)
    own(synclist + 4, 7)
    for a in clear + [count, launch, bl_state] + bl_code + term + bgtex:
        own(a)

    runs, words = [], []
    for t in table:
        idx = t["index"]
        if idx not in img:
            continue
        b = bytearray(img[idx])
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
        # copies of one run at a constant step become one run
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
    # the runs give back the image less the native words, and nothing else
    back = {t["index"]: bytearray(len(img[t["index"]])) for t in table if t["index"] in img}
    for addr, n, times, step, at in runs:
        a = base.at(addr)
        for k in range(times):
            o = addr + k * step - a["devva"]
            back[a["index"]][o:o + 4 * n] = struct.pack("<%dI" % n, *words[at:at + n])
    for t in table:
        if t["index"] in back:
            for o in range(0, len(back[t["index"]]) & ~3, 4):
                want = 0 if t["addr"] + o in native else u32(img[t["index"]], o)
                check(u32(back[t["index"]], o) == want, "run data at 0x%08x" % (t["addr"] + o))

    data_bytes = 12 * len(runs) + 4 * len(words)
    write_h(locals())
    write_c(table, runs, words, cmd_words, regs3d, term_words, blend_off, blend_on, ta, r3d)
    nz = sum(1 for t in table if t["index"] in img for i in range(0, len(img[t["index"]]), 64)
             if any(img[t["index"]][i:i + 64]))
    print("mkreplay: %d allocations, %d runs, %d words: %d bytes of image data "
          "(%d bytes of tables in all); the image has %d non-zero 64-byte blocks (%d KiB)" % (
              len(table), len(runs), len(words), data_bytes, data_bytes + 16 * len(table) + 8 * len(cmd_words)
              + 4 * (34 + 7 + 8) + 8 * 2 * (len(ta[0]) + len(r3d[0])), nz, nz * 64 // 1024))


def write_h(v):
    rt0, rt1 = v["rt0"], v["rt1"]
    lines = [
        "/* Generated by tools/sgx/mkreplay.py from the GLES recordings in",
        " * build/sgx/draw; do not edit. */",
        "#ifndef SGX_SCENE_DATA_H",
        "#define SGX_SCENE_DATA_H",
        "#include <stdint.h>",
        "",
        "enum { SGX_SCENE_USER, SGX_SCENE_SYNC, SGX_SCENE_CONTEXT };",
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
    d("TA_STATUS", len(v["ta"][0]))
    d("3D_STATUS", len(v["r3d"][0]))
    d("TA_SEQ", v["seq_ta"])
    d("3D_SEQ", v["seq_3d"])
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
    d("VERTICES", v["vbo_addr"])
    d("INDICES", v["ib"]["devva"])
    d("INDEX_TABLE", v["ib"]["size"] // 2)
    d("INDEX_COUNT", v["count"])
    d("INDEX_LIST", 0x81c00000)
    d("CLEAR", v["clear"][0])
    d("CLEARS", len(v["clear"]))
    d("CLEAR_STEP", v["clear"][1] - v["clear"][0])
    d("MTE_LAUNCH", v["launch"])
    d("MTE_WORD", v["w"])
    d("BLEND_STATE", v["bl_state"])
    d("BLEND_CODE", v["bl_code"][0])
    d("TILE_STORE0", v["pbe"][0])
    d("TILE_STORE1", v["pbe"][1])
    d("BG_TEXTURE", v["bgtex"][0])
    d("TERMINATE", v["term"][0])
    lines.append("")
    d("STREAM_FIRST", v["stream_a"])
    d("STREAM_LAST", v["stream_b"])
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
        "extern const uint32_t sgx_scene_terminate[7];",
        "extern const uint32_t sgx_scene_blend[2][4];",
        "extern const struct sgx_scene_status sgx_scene_ta_status[2][SGX_SCENE_TA_STATUS];",
        "extern const struct sgx_scene_status sgx_scene_3d_status[2][SGX_SCENE_3D_STATUS];",
        "#endif",
    ]
    with open(OUT_H, "w") as f:
        f.write("\n".join(lines) + "\n")


def hexwords(ws, per=6):
    out = []
    for i in range(0, len(ws), per):
        out.append("\t" + " ".join("0x%08x," % w for w in ws[i:i + per]))
    return out


def write_c(table, runs, words, cmd_words, regs3d, term, blend_off, blend_on, ta, r3d):
    kinds = {"USER": "SGX_SCENE_USER", "SYNC": "SGX_SCENE_SYNC", "CONTEXT": "SGX_SCENE_CONTEXT"}
    lines = [
        "/* Generated by tools/sgx/mkreplay.py from the GLES recordings in",
        " * build/sgx/draw; do not edit. */",
        "#include \"sgx_scene_data.h\"",
        "",
        "const struct sgx_scene_alloc sgx_scene_allocs[SGX_SCENE_ALLOCS] = {",
    ]
    for t in table:
        lines.append("\t{ 0x%08x, 0x%06x, 0x%04x, %2d, 0x%02x, %s }," % (
            t["addr"], t["size"], t["align"], t["heap"], t["flags"], kinds[t["kind"]]))
    lines += ["};", "", "const struct sgx_scene_run sgx_scene_runs[SGX_SCENE_RUNS] = {"]
    for r in runs:
        lines.append("\t{ 0x%08x, %4d, %3d, 0x%03x, %4d }," % r)
    lines += ["};", "", "const uint32_t sgx_scene_words[] = {"] + hexwords(words) + ["};", ""]
    lines += ["const struct sgx_scene_word sgx_scene_cmd[SGX_SCENE_CMD_WORDS] = {"]
    for o, w in cmd_words:
        lines.append("\t{ 0x%03x, 0x%08x }," % (o, w))
    lines += ["};", "", "const uint32_t sgx_scene_regs3d[34] = {"] + hexwords(regs3d) + ["};", ""]
    lines += ["const uint32_t sgx_scene_terminate[7] = {"] + hexwords(term, 7) + ["};", ""]
    lines += ["const uint32_t sgx_scene_blend[2][4] = {",
              "\t{ " + ", ".join("0x%08x" % w for w in blend_off) + " },",
              "\t{ " + ", ".join("0x%08x" % w for w in blend_on) + " },", "};", ""]
    for name, lists, n in (("ta", ta, "SGX_SCENE_TA_STATUS"), ("3d", r3d, "SGX_SCENE_3D_STATUS")):
        lines.append("const struct sgx_scene_status sgx_scene_%s_status[2][%s] = {" % (name, n))
        for lst in lists:
            lines.append("\t{ " + ", ".join("{ 0x%08x, %d }" % s for s in lst) + " },")
        lines += ["};", ""]
    with open(OUT_C, "w") as f:
        f.write("\n".join(lines).rstrip("\n") + "\n")


if __name__ == "__main__":
    main()
