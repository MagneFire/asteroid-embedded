#!/usr/bin/env python3
"""Check the output of srvinit_emu.py against the disassembly-derived facts in
asteroid-embedded-notes/hw/minnow/sgx/init-facts.md.

Usage: check_init.py LIBSRV_INIT_SO [OUTDIR]

Prints PASS/FAIL/INFO per item; exit status 1 if any item FAILs.  INFO lines
report things the facts file does not cover.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from srvinit_emu import place_allocation, HEAPS  # noqa: E402

KERNEL_DATA, KERNEL_CODE = 3, 2
F_RW, F_FULL, F_FULL_MP, F_CODE, F_STENCIL = 0x4F, 0x4F, 0x2004F, 0x9, 0x0800002B

# (label, heap, size, alignment, flags) in allocation order, from init-facts.md
EXPECTED_ALLOCS = [
    ("Kernel CCB", KERNEL_DATA, 0x2000, 0x1000, F_FULL),
    ("CCB control", KERNEL_DATA, 8, 0x1000, F_FULL),
    ("Event kicker", KERNEL_DATA, 4, 16, F_FULL),
    ("TA/3D control", KERNEL_DATA, 0x120, 32, F_FULL_MP),
    ("HOST_CTL", KERNEL_DATA, 0x4C, 32, F_FULL_MP),
    ("Misc info", KERNEL_DATA, 0x54, 32, F_FULL_MP),
    ("Microkernel trace buffer", KERNEL_DATA, 0x2008, 0x10, F_FULL),
    ("HWPerf buffer", KERNEL_DATA, 0x640C, 32, F_FULL),
    ("TA signature buffer", KERNEL_DATA, 0x314, 32, F_FULL),
    ("3D signature buffer", KERNEL_DATA, 0x724, 32, F_FULL),
    ("SGX ukernel", KERNEL_CODE, 0x6890, 0x8000, F_CODE),
    ("SGX TA", KERNEL_CODE, 0x6790, 0x8000, F_CODE),
    ("SGX SPM", KERNEL_CODE, 0x5990, 0x8000, F_CODE),
    ("SGX 3D", KERNEL_CODE, 0x7130, 0x8000, F_CODE),
    ("PDS program 1", KERNEL_CODE, 0x38, 16, F_CODE),
    ("PDS program 2", KERNEL_CODE, 0x48, 16, F_CODE),
    ("PDS program 3", KERNEL_CODE, 0x38, 16, F_CODE),
    ("PDS program 4", KERNEL_CODE, 0x110, 16, F_CODE),
    ("PDS program 5", KERNEL_CODE, 0x78, 16, F_CODE),
    ("Dummy stencil load", KERNEL_DATA, 0x40000, 0x1000, F_STENCIL),
    ("Dummy stencil store", KERNEL_DATA, 0x40000, 0x1000, F_STENCIL),
]
# allocation index -> .rodata source offset
PROGRAM_SRC = {10: 0x2898, 11: 0x9120, 12: 0xF8A8, 13: 0x15230,
               14: 0x1C358, 15: 0x1C390, 16: 0x1C3D8, 17: 0x1C410, 18: 0x1C520}
UKERNEL, TA, SPM, D3 = 10, 11, 12, 13
CTRL, HOSTCTL, CCBCTL, TRACE, CCB, KICKER = 3, 4, 1, 6, 0, 2
HWPERF, TASIG, D3SIG = 7, 8, 9
PDS = {1: 14, 2: 15, 3: 16, 4: 17, 5: 18}
PDS_CONSTANTS = {0x04000000, 1, 0x100, 0x200, 0x01E0003F, 0x0004000F, 0x00120080}

results = []


def report(status, name, detail=""):
    results.append(status)
    print("%-5s %s%s" % (status, name, (" -- " + detail) if detail else ""))


def check(name, ok, detail=""):
    report("PASS" if ok else "FAIL", name, "" if ok else detail)


def info(name, detail):
    report("INFO", name, detail)


def decode_imm(prog, off):
    """Immediate of the 64-bit USE instruction at off (encoding from init-facts.md)."""
    w0, w1 = struct.unpack_from("<II", prog, off)
    return (w0 & 0x1FFFFF) | (((w1 >> 4) & 0x1F) << 21) | (((w1 >> 12) & 0x3F) << 26)


def words(b):
    return struct.unpack("<%dI" % (len(b) // 4), b[:len(b) // 4 * 4])


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    lib = open(sys.argv[1], "rb").read()
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.normpath(os.path.join(here, "..", "..", "build", "sgx", "init"))
    m = json.load(open(os.path.join(out, "manifest.json")))
    allocs = m["allocations"]
    data = [open(os.path.join(out, a["file"]), "rb").read() for a in allocs]
    dev = [a["devva"] for a in allocs]
    heap_base = {h["id"]: h["base"] for h in m["heap_table"]}
    pds_base = heap_base[6]
    ini = m["devinitpart2"]

    check("SrvInit returned success", m["srvinit_return"] == 0, str(m["srvinit_return"]))
    check("heap table equals structures.md 1.5",
          [(h["id"], h["base"], h["size"]) for h in m["heap_table"]] == [(h[0], h[2], h[3]) for h in HEAPS])

    # ---- allocation list -------------------------------------------------------
    check("allocation count is 21", len(allocs) == 21, str(len(allocs)))
    prev_end = {}
    for i, (label, heap, size, align, flags) in enumerate(EXPECTED_ALLOCS):
        if i >= len(allocs):
            break
        a = allocs[i]
        got = (a["heap_id"], a["size"], a["alignment"], a["flags"])
        ok = got == (heap, size, align, flags)
        check("alloc #%d %s: heap/size/align/flags" % (i, label), ok,
              "facts (%d, 0x%x, 0x%x, 0x%x) emulation (%d, 0x%x, 0x%x, 0x%x)"
              % ((heap, size, align, flags) + got))
        want = place_allocation(heap_base[a["heap_id"]], prev_end.get(a["heap_id"]), a["size"], a["alignment"])
        prev_end[a["heap_id"]] = a["devva"] + a["size"]
        check("alloc #%d %s: devVA follows policy (0x%08x)" % (i, label, a["devva"]), a["devva"] == want)

    # ---- microkernel programs ---------------------------------------------------
    pds2 = (dev[PDS[2]] - pds_base) >> 4
    pds3 = (dev[PDS[3]] - pds_base) >> 4
    pds4 = (dev[PDS[4]] - pds_base) & ~15
    pds5 = (dev[PDS[5]] - pds_base) >> 4
    sites = {
        UKERNEL: {0x2260: 0x41801000, 0x2270: pds2, 0x2278: dev[CTRL], 0x22B0: 0x41800900,
                  0x22C0: pds3, 0x2310: pds4, 0x2320: 7, 0x5328: 0x42000900, 0x5340: pds5},
        TA: {0x5228: 0x42000900, 0x5240: pds5},
        SPM: {0x4428: 0x42000900, 0x4440: pds5},
        D3: {0x6728: 0x42000900, 0x6740: pds5},
    }
    for idx, expect in sites.items():
        label, _, size = EXPECTED_ALLOCS[idx][0], 0, EXPECTED_ALLOCS[idx][2]
        src = lib[PROGRAM_SRC[idx]:PROGRAM_SRC[idx] + size]
        prog = data[idx]
        diff_slots = [o for o in range(0, size, 8) if prog[o:o + 8] != src[o:o + 8]]
        check("%s: copy equals .rodata except at the patch sites" % label,
              diff_slots == sorted(expect), "differing 8-byte slots: %s" % [hex(o) for o in diff_slots])
        check("%s: tail beyond the last slot is verbatim" % label, len(prog) == size)
        for off, want in sorted(expect.items()):
            got = decode_imm(prog, off)
            check("%s +0x%x immediate = 0x%08x" % (label, off, want), got == want, "decoded 0x%08x" % got)
        placeholders = [decode_imm(src, off) & 0xFFFF for off in expect]
        check("%s: all sites held the 0xBEEF placeholder" % label, all(p == 0xBEEF for p in placeholders),
              str([hex(p) for p in placeholders]))

    # ---- PDS programs ------------------------------------------------------------
    pds_patched = {}
    for n, idx in PDS.items():
        size = EXPECTED_ALLOCS[idx][2]
        src = lib[PROGRAM_SRC[idx]:PROGRAM_SRC[idx] + size]
        prog = data[idx]
        sw, pw = words(src), words(prog)
        patched = [(4 * k, pw[k]) for k in range(len(pw)) if pw[k] != sw[k]]
        pds_patched[n] = patched
        check("PDS program %d: size 0x%x and template preserved (only zero template words patched)" % (n, size),
              len(prog) == size and all(sw[o // 4] == 0 for o, _ in patched))
        odd = [(hex(o), hex(v)) for o, v in patched
               if v not in PDS_CONSTANTS and (v & 0xF) != 0xF and v != dev[CTRL]]
        check("PDS program %d: patched words are DOUTU words, the listed constants or the TA/3D address" % n,
              not odd, str(odd))
        info("PDS program %d patched words" % n, ", ".join("+0x%x=0x%08x" % (o, v) for o, v in patched))
    check("PDS program 2 +0 holds the TA/3D control address", pds_patched[2][0] == (0, dev[CTRL]),
          str(pds_patched[2][:1]))
    entries = {1: 0x2260, 2: 0x22E0, 3: 0x22F0}
    for n, ent in entries.items():
        doutu = [v for o, v in pds_patched[n] if v != dev[CTRL] and v & 0x200000 and (v & 0xF) == 0xF
                 and ((v >> 4) & 0xFFFF) == ent]
        check("PDS program %d has a DOUTU entering ukernel +0x%x (0x%08x)" % (n, ent, 0x20000F | (ent << 4)),
              len(doutu) == 1, str(doutu))
    check("PDS program 4 has a DOUTU entering ukernel +0x80", 0x0020080F in [v for _, v in pds_patched[4]])

    # ---- TA/3D control block --------------------------------------------------------
    cw = dict(enumerate(words(data[CTRL])))
    w = lambda off: cw[off // 4]
    kd_base = heap_base[KERNEL_DATA]
    expect_ctrl = {
        0x00: dev[CTRL], 0x04: dev[HOSTCTL], 0x08: dev[CCBCTL], 0x18: 2, 0x80: dev[TRACE],
        0x88: 0x120F, 0x8C: 0, 0x98: dev[CCB], 0x9C: m["page_directory_phys_addr"], 0xDC: kd_base,
        0xE0: (dev[19] - kd_base) | 0x80000000, 0xE4: (dev[20] - kd_base) | 0x80000000,
        0xE8: dev[HWPERF], 0xEC: dev[TASIG], 0xF0: dev[D3SIG],
    }
    for off, want in sorted(expect_ctrl.items()):
        check("TA/3D control +0x%02x = 0x%08x" % (off, want), w(off) == want, "got 0x%08x" % w(off))
    extra = {hex(4 * k): hex(v) for k, v in cw.items() if v and 4 * k not in expect_ctrl}
    check("TA/3D control: no other non-zero words", not extra, str(extra))
    info("TA/3D control +0x88 default", "MKTraceProfile apphint default observed as 0x120f; HWPerfFlags default 0")

    # ---- other buffers ----------------------------------------------------------------
    for idx in (CCB, CCBCTL, KICKER, HOSTCTL, 5, TRACE, HWPERF):
        check("alloc #%d %s is all zero" % (idx, EXPECTED_ALLOCS[idx][0]), not any(data[idx]))
    for idx in (TASIG, D3SIG):
        nz = {"+0x%x" % (4 * k): "0x%x" % v for k, v in enumerate(words(data[idx])) if v}
        info("alloc #%d %s header words written by init (not in facts)" % (idx, EXPECTED_ALLOCS[idx][0]), str(nz))
    check("alloc #0..2 are never written by the library (kernel clears them)",
          all(allocs[i]["never_written_ranges"] == [[0, allocs[i]["size"]]] for i in (0, 1, 2)))

    # ---- scripts ---------------------------------------------------------------------------
    sc = ini["sScripts"]
    flat = lambda lst: [(e["op"], e["offset"], e["value"]) for e in lst]
    W, H = "WRITE", ("HALT", 0, 0)
    part1 = [(W, 0xA58, 0), (W, 0xACC, 0), H]
    check("script part 1", flat(sc["asInitCommandsPart1"]) == part1, str(flat(sc["asInitCommandsPart1"])))
    pix, vtx, kc = heap_base[4], heap_base[5], heap_base[2]
    part2 = [(W, 0x130, 0x4000), (W, 0x110, 0), (W, 0x13C, 0), (W, 0xA7C, 0), (W, 0xA80, 0),
             (W, 0xA74, (0x3F << 16) | 0x8200), (W, 0x4E8, 0), (W, 0xA78, 0), (W, 0xABC, 0x40),
             (W, 0x804, 0xFFFF), (W, 0xA00, 0x7C000)]
    part2 += [(W, 0xA0C + 4 * n, ((pix + n * 0x80000) >> 8) | 0x01000000) for n in range(10)]
    part2 += [(W, 0xA0C + 4 * (10 + n), (vtx + n * 0x80000) >> 8) for n in range(4)]
    part2 += [(W, 0xA48, (kc >> 8) | 0x03000000), (W, 0xAB8, pds_base), (W, 0xB30, 0x100),
              (W, 0xAC4, dev[KICKER]), (W, 0xA68, (dev[PDS[1]] - pds_base) & ~15), (W, 0xA6C, 3),
              (W, 0xA70, 0x00013D01), (W, 0xA58, 0x2204F00F), H]
    got2 = flat(sc["asInitCommandsPart2"])
    check("script part 2: %d entries" % len(part2), len(got2) == len(part2), "got %d" % len(got2))
    for i, (g, e) in enumerate(zip(got2, part2)):
        check("script part 2 [%d] reg 0x%03x = 0x%08x" % (i, e[1], e[2]), g == e, "got %s" % (g,))
    deinit = [(W, r, 0) for r in (0xA58, 0xA68, 0xA6C, 0xA70, 0xA48, 0x130, 0x110, 0x000)] + [H]
    check("deinit script", flat(sc["asDeinitCommands"]) == deinit, str(flat(sc["asDeinitCommands"])))
    dbg = sc["asSGXREGDebugCommandsSlave"]
    info("debug (PRINT) script, not in facts",
         "%d PRINT regs: %s" % (len(dbg) - 1, " ".join("0x%x" % e["offset"] for e in dbg if e["op"] == "PRINT")))
    check("no stray script entries after HALT",
          all(not e["op"].startswith("AFTER_HALT") for k in sc for e in sc[k]))

    # ---- values handed to the kernel -----------------------------------------------------------
    kick = ini["aui32HostKickAddr"]
    want_kick = {"TA": 0x1EC, "TRANSFER": 0x1CD, "POWER": 0x18D, "CONTEXTSUSPEND": 0x1AB, "CLEANUP": 0x21D,
                 "GETMISCINFO": 0x40E, "PROCESS_QUEUES": 0x3B1, "SETHWPERFSTATUS": 0x3B7}
    for k, v in want_kick.items():
        check("host kick address %s = 0x%x" % (k, v), kick[k] == v, "got 0x%x" % kick[k])
    unset = [k for k in kick if k not in want_kick]
    info("host kick entries the library does not set (%s)" % ", ".join(unset),
         "values come from uninitialised memory on the device; fields listed in "
         "init_info_fields_depending_on_uninitialised_memory")
    sizes = {"CMDTA": 1304, "CMDTA_SHARED": 1128, "TRANSFERCMD": 1204, "TRANSFERCMD_SHARED": 268,
             "3DREGISTERS": 136, "HWPBDESC": 68, "HWRENDERCONTEXT": 64, "HWRENDERDETAILS": 1040,
             "HWRTDATA": 52, "HWRTDATASET": 76, "HWTRANSFERCONTEXT": 228, "HOST_CTL": 76, "COMMAND": 32}
    check("13 struct sizes", ini["sSGXStructSizes"] == sizes, str(ini["sSGXStructSizes"]))
    check("build options 0x0016801C", ini["ui32ClientBuildOptions"] == 0x0016801C)
    check("EDM task regs 0xB10 / 0xB1C", (ini["ui32EDMTaskReg0"], ini["ui32EDMTaskReg1"]) == (0xB10, 0xB1C))
    check("clock gate ctl 0x00222220, status reg 0x004, mask 0xFFFFFFFF",
          (ini["ui32ClkGateCtl"], ini["ui32ClkGateStatusReg"], ini["ui32ClkGateStatusMask"])
          == (0x222220, 4, 0xFFFFFFFF))
    check("cache control 2", ini["ui32CacheControl"] == 2)
    dd = ini["asInitDevData"]
    want_dd = {4: 2, 6: 0x180, 7: 0x640, 8: pds_base, 9: pix, 10: dev[19], 11: dev[20]}
    for i, v in sorted(want_dd.items()):
        check("asInitDevData[%d] = 0x%x" % (i, v), dd[i] == v, "got 0x%x" % dd[i])
    check("asInitDevData: all other entries zero", all(v == 0 for i, v in enumerate(dd) if i not in want_dd))
    handles = {"hKernelCCBMemInfo": CCB, "hKernelCCBCtlMemInfo": CCBCTL, "hKernelCCBEventKickerMemInfo": KICKER,
               "hKernelSGXHostCtlMemInfo": HOSTCTL, "hKernelSGXTA3DCtlMemInfo": CTRL, "hKernelSGXMiscMemInfo": 5,
               "hKernelHWPerfCBMemInfo": HWPERF, "hKernelTASigBufferMemInfo": TASIG,
               "hKernel3DSigBufferMemInfo": D3SIG, "hKernelEDMStatusBufferMemInfo": TRACE}
    for k, v in handles.items():
        check("%s -> allocation #%d" % (k, v), ini[k] == v, "got %s" % ini[k])
    info("asInitMemHandles (allocation indices)", str(ini["asInitMemHandles"]))
    info("init struct fields on uninitialised memory", str(m["init_info_fields_depending_on_uninitialised_memory"]))
    info("image bytes depending on host pointers",
         str([a["index"] for a in allocs if a["host_pointer_dependent_ranges"]]) or "none")

    fails = results.count("FAIL")
    print("\n%d PASS, %d FAIL, %d INFO" % (results.count("PASS"), fails, results.count("INFO")))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
