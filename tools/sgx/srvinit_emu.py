#!/usr/bin/env python3
"""Run the SGX530 init server library (libsrv_init.so, function SrvInit) under
Unicorn and record the device memory image it builds.

Usage: srvinit_emu.py LIBSRV_INIT_SO [OUTDIR] [--pd-addr 0x8F000000]

Everything the library imports (PVRSRV*/SGX* from libsrv_um plus four libc
hooks) is implemented here at the API level; the library itself runs
unmodified.  See README.md for what is stubbed and why.
"""
import argparse
import json
import os
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_PROT_ALL,
                     UC_HOOK_CODE, UC_HOOK_MEM_READ)
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC)

PAGE = 4096
LIB_BASE = 0x10000000
STUB_BASE = 0x20000000
SENTINEL = 0x20800000
STRUCT_BASE = 0x2F000000   # small structures handed to the library
STRUCT_SIZE = 0x00100000
HEAP_BASE = 0x30000000     # PVRSRVAllocUserModeMem
HEAP_SIZE = 0x01000000
DEVBUF_BASE = 0x40000000   # CPU side of device allocations
DEVBUF_SIZE = 0x10000000
STACK_TOP = 0x7F100000
STACK_SIZE = 0x00100000

# Fake kernel handles; the library only passes them around.
H_DEVCOOKIE = 0xC0DE0001
H_HEAP_BASE = 0xE0000000      # + 0x40 * heap index
H_MEMINFO_BASE = 0xD0000000   # + 0x100 * allocation index

PVRSRV_OK = 0
PVRSRV_ERROR_OUT_OF_MEMORY = 1
SGX_BUILD_OPTIONS = 0x0016801C

MEM_NOUSERVIRTUAL = 1 << 27
KNOWN_ALLOC_FLAGS = 0x0800_0000 | 0x0002_0000 | 0xFFF  # low 12 bits + MULTI_PROCESS + NOUSERVIRTUAL

# --- heap table: sgxconfig.h (28-bit address space, SUPPORT_LARGE_GENERAL_HEAP,
# SUPPORT_PERCONTEXT_PB) in the creation order of sgxinit.c SGXRegisterDevice.
WC, RAM, SINGLE, MULTI = 1 << 14, 1 << 9, 1 << 16, 1 << 17
HEAPS = [
    # id, name, base, size, attribs
    (0, "General", 0x00001000, 0x0B7FE000, WC | RAM | SINGLE),
    (1, "TA Data", 0x0C800000, 0x00FFF000, WC | RAM | MULTI),
    (2, "Kernel Code", 0x0E400000, 0x0007F000, WC | RAM | MULTI),
    (3, "KernelData", 0x0F000000, 0x003FF000, WC | RAM | MULTI),
    (4, "PixelShaderUSSE", 0x0F400000, 0x004FF000, WC | RAM | SINGLE),
    (5, "VertexShaderUSSE", 0x0FC00000, 0x001FF000, WC | RAM | SINGLE),
    (6, "PDSPixelCodeData", 0x0DC00000, 0x007FF000, WC | RAM | SINGLE),
    (7, "PDSVertexCodeData", 0x0E800000, 0x007FF000, WC | RAM | SINGLE),
    (8, "CacheCoherent", 0x0D800000, 0x003FF000, WC | RAM | MULTI),
    (9, "Shared 3DParameters", 0x0B800000, 0, WC | RAM | MULTI),
    (10, "Percontext 3DParameters", 0x0B800000, 0x00FFF000, WC | RAM | SINGLE),
]
PVRSRV_DEVICE_TYPE_SGX = 7
HEAP_ID = lambda idx: (PVRSRV_DEVICE_TYPE_SGX << 24) | idx

CMD_NAMES = ["TA", "TRANSFER", "2D", "POWER", "CONTEXTSUSPEND", "CLEANUP",
             "GETMISCINFO", "PROCESS_QUEUES", "DATABREAKPOINT",
             "SETHWPERFSTATUS", "FLUSHPDCACHE"]
SIZE_NAMES = ["CMDTA", "CMDTA_SHARED", "TRANSFERCMD", "TRANSFERCMD_SHARED",
              "3DREGISTERS", "HWPBDESC", "HWRENDERCONTEXT", "HWRENDERDETAILS",
              "HWRTDATA", "HWRTDATASET", "HWTRANSFERCONTEXT", "HOST_CTL",
              "COMMAND"]
OP_NAMES = {0: "ILLEGAL", 1: "WRITE", 2: "READ", 3: "PRINT", 4: "HALT"}

MEMINFO_SIZE = 0x38   # PVRSRV_CLIENT_MEM_INFO as allocated by libsrv_um (with SUPPORT_MEMINFO_IDS)
MI_LINADDR, MI_LINADDRKM, MI_DEVVADDR, MI_FLAGS, MI_CLIENTFLAGS = 0, 4, 8, 0xC, 0x10
MI_ALLOCSIZE, MI_SYNC, MI_MAPPING, MI_KERNELMEMINFO = 0x14, 0x18, 0x1C, 0x20

CLIENT_INIT_READ = 0xAB0   # bytes of the client-side init struct that are decoded


def round_up(v, a):
    return (v + a - 1) & ~(a - 1)


# ---------------------------------------------------------------------------
# Allocation policy.  The one function a native implementation has to copy.
# ---------------------------------------------------------------------------
def place_allocation(heap_base, prev_end, size, alignment):
    """Device virtual address of the next allocation in a heap.

    prev_end is None for the first allocation of the heap (placed at the heap
    base), else previous_address + previous_requested_size.  Later allocations
    go to the lowest address >= prev_end that is 4 KiB aligned and aligned to
    the requested alignment (minimum 4 KiB).  Freed space is never reused.
    """
    if prev_end is None:
        return heap_base
    align = max(alignment, PAGE)
    assert align & (align - 1) == 0, "alignment not a power of two"
    return round_up(round_up(prev_end, PAGE), align)


class EmuError(Exception):
    pass


class Alloc:
    def __init__(self, index, heap, size, align, flags, devva, buf, site, handle):
        self.index, self.heap, self.size, self.align = index, heap, size, align
        self.flags, self.devva, self.buf, self.site = flags, devva, buf, site
        self.meminfo = 0
        self.freed = False
        self.handle = handle


class Emu:
    def __init__(self, lib_path, pd_addr, fill, shifted=False):
        self.pd_addr = pd_addr
        self.fill = fill
        # shifted=True moves every host-side address and fake handle, to find
        # output bytes that depend on them
        d = 0x08000000 if shifted else 0
        self.struct_base = STRUCT_BASE - d
        self.heap_base = HEAP_BASE + d
        self.devbuf_base = DEVBUF_BASE + d
        self.stack_top = STACK_TOP - d
        self.h_heap = H_HEAP_BASE + (0x01000000 if shifted else 0)
        self.h_meminfo = H_MEMINFO_BASE + (0x01000000 if shifted else 0)
        self.km_base = 0xC0000000 + (0x05000000 if shifted else 0)
        self.lib_path = lib_path
        self.calls = []
        self.allocs = []
        self.heap_prev_end = {}
        self.stub_names = {}      # address -> import name
        self.struct_regs = []     # (start, size, label) for read tracing
        self.struct_reads = set()
        self.struct_next = self.struct_base
        self.heap_next = self.heap_base
        self.dev_next = self.devbuf_base
        self.dbg_errors = []
        self.part2 = None         # snapshot at SGXDevInitPart2
        self.frees_after_part2 = False
        self.apphints = []
        self.error = None
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.load_elf()

    # --- loading -----------------------------------------------------------
    def load_elf(self):
        uc = self.uc
        with open(self.lib_path, "rb") as f:
            data = f.read()
        import io
        elf = ELFFile(io.BytesIO(data))
        if elf.header["e_machine"] != "EM_ARM" or elf.elfclass != 32:
            raise EmuError("not an ARM ELF32")
        top = 0
        for seg in elf.iter_segments():
            if seg["p_type"] == "PT_LOAD":
                top = max(top, seg["p_vaddr"] + seg["p_memsz"])
        uc.mem_map(LIB_BASE, round_up(top, PAGE), UC_PROT_ALL)
        for seg in elf.iter_segments():
            if seg["p_type"] == "PT_LOAD":
                uc.mem_write(LIB_BASE + seg["p_vaddr"], seg.data())
        # stubs
        dynsym = elf.get_section_by_name(".dynsym")
        stub_page = round_up(0x1000, PAGE)
        uc.mem_map(STUB_BASE, stub_page, UC_PROT_ALL)
        uc.mem_map(SENTINEL & ~(PAGE - 1), PAGE, UC_PROT_ALL)
        uc.mem_write(SENTINEL, b"\x70\x47")  # bx lr (never executed; until= stops first)
        self.stub_addr = {}

        def stub_for(name):
            if name not in self.stub_addr:
                a = STUB_BASE + 4 * len(self.stub_addr)
                self.stub_addr[name] = a
                self.stub_names[a] = name
                uc.mem_write(a, b"\x70\x47")  # Thumb bx lr
            return self.stub_addr[name]

        for secname in (".rel.dyn", ".rel.plt"):
            sec = elf.get_section_by_name(secname)
            if sec is None:
                continue
            for r in sec.iter_relocations():
                typ = r["r_info_type"]
                where = LIB_BASE + r["r_offset"]
                sym = dynsym.get_symbol(r["r_info_sym"]) if r["r_info_sym"] else None
                if typ == 23:    # R_ARM_RELATIVE
                    v = struct.unpack("<I", uc.mem_read(where, 4))[0]
                    uc.mem_write(where, struct.pack("<I", (v + LIB_BASE) & 0xFFFFFFFF))
                elif typ in (21, 22):   # GLOB_DAT, JUMP_SLOT
                    uc.mem_write(where, struct.pack("<I", stub_for(sym.name) | 1))
                elif typ == 2:   # ABS32
                    v = struct.unpack("<I", uc.mem_read(where, 4))[0]
                    if sym.entry["st_shndx"] == "SHN_UNDEF":
                        s = stub_for(sym.name) | 1
                    else:
                        s = LIB_BASE + sym["st_value"]
                    uc.mem_write(where, struct.pack("<I", (s + v) & 0xFFFFFFFF))
                else:
                    raise EmuError("unsupported relocation type %d" % typ)
        self.entry = None
        for s in dynsym.iter_symbols():
            if s.name == "SrvInit":
                self.entry = LIB_BASE + s["st_value"]
        if self.entry is None:
            raise EmuError("SrvInit not exported")
        self.init_funcs = []
        dyn = elf.get_section_by_name(".dynamic")
        init_arr = init_arrsz = init_fn = None
        for t in dyn.iter_tags():
            if t.entry.d_tag == "DT_INIT":
                init_fn = t.entry.d_val
            elif t.entry.d_tag == "DT_INIT_ARRAY":
                init_arr = t.entry.d_val
            elif t.entry.d_tag == "DT_INIT_ARRAYSZ":
                init_arrsz = t.entry.d_val
        if init_fn:
            self.init_funcs.append(LIB_BASE + init_fn)
        if init_arr is not None:
            for i in range(init_arrsz // 4):
                fn = struct.unpack("<I", uc.mem_read(LIB_BASE + init_arr + 4 * i, 4))[0]
                if fn not in (0, 0xFFFFFFFF):
                    self.init_funcs.append(fn)

        uc.mem_map(self.struct_base, STRUCT_SIZE, UC_PROT_ALL)
        uc.mem_map(self.heap_base, HEAP_SIZE, UC_PROT_ALL)
        uc.mem_map(self.devbuf_base, DEVBUF_SIZE, UC_PROT_ALL)
        uc.mem_map(self.stack_top - STACK_SIZE, STACK_SIZE, UC_PROT_ALL)
        if self.fill:
            pat = bytes([self.fill]) * PAGE
            for base, size in ((self.heap_base, HEAP_SIZE), (self.stack_top - STACK_SIZE, STACK_SIZE)):
                for off in range(0, size, PAGE):
                    uc.mem_write(base + off, pat)
        uc.hook_add(UC_HOOK_CODE, self.on_stub, begin=STUB_BASE, end=STUB_BASE + stub_page)
        uc.hook_add(UC_HOOK_MEM_READ, self.on_struct_read, begin=self.struct_base,
                    end=self.struct_base + STRUCT_SIZE - 1)

    # --- helpers -----------------------------------------------------------
    def reg(self, r):
        return self.uc.reg_read(r)

    def arg(self, n):
        if n < 4:
            return self.reg((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)[n])
        return self.u32(self.reg(UC_ARM_REG_SP) + 4 * (n - 4))

    def u32(self, addr):
        return struct.unpack("<I", self.uc.mem_read(addr, 4))[0]

    def w32(self, addr, v):
        self.uc.mem_write(addr, struct.pack("<I", v & 0xFFFFFFFF))

    def cstr(self, addr):
        if addr == 0:
            return None
        out = bytearray()
        while len(out) < 4096:
            b = bytes(self.uc.mem_read(addr + len(out), 1))
            if b == b"\0":
                break
            out += b
        return out.decode("latin-1")

    def new_struct(self, size, label):
        a = round_up(self.struct_next, 16)
        self.struct_next = a + size
        assert self.struct_next <= self.struct_base + STRUCT_SIZE
        self.struct_regs.append((a, size, label))
        return a

    def on_struct_read(self, uc, access, addr, size, value, ud):
        for start, sz, label in self.struct_regs:
            if start <= addr < start + sz:
                if label == "meminfo":
                    self.struct_reads.add((label, (addr - start) % MEMINFO_SIZE))
                else:
                    self.struct_reads.add((label, addr - start))
                return

    def heap_alloc(self, size):
        a = round_up(self.heap_next, 16)
        self.heap_next = a + size
        if self.heap_next > self.heap_base + HEAP_SIZE:
            raise EmuError("emulator user heap exhausted")
        return a

    def site(self):
        return self.reg(UC_ARM_REG_LR) & ~1

    # --- stub dispatch -----------------------------------------------------
    def on_stub(self, uc, address, size, ud):
        if self.error:
            return
        name = self.stub_names.get(address)
        if name is None:
            return
        try:
            fn = getattr(self, "imp_" + name, None)
            if fn is None:
                raise EmuError("call to unimplemented import %s from 0x%x"
                               % (name, self.site() - LIB_BASE))
            site = self.site() - LIB_BASE
            ret, text = fn()
            uc.reg_write(UC_ARM_REG_R0, ret & 0xFFFFFFFF)
            self.calls.append("%04d %s(%s) = %s  [caller 0x%x]"
                              % (len(self.calls), name, text,
                                 "0x%x" % (ret & 0xFFFFFFFF) if ret > 9 else str(ret), site))
        except Exception as e:      # propagated after emu_start returns
            self.error = e
            uc.emu_stop()

    # --- libsrv_um imports ---------------------------------------------------
    def imp_PVRSRVInitSrvConnect(self):
        pp = self.arg(0)
        conn = self.new_struct(8 + 4 + 16 * 20 + 16 * 4 + 16 * 4 + 4, "connection")
        self.w32(conn, 1)          # hServices
        self.w32(conn + 4, 1000)   # ui32ProcessID
        self.w32(pp, conn)
        self.conn = conn
        return PVRSRV_OK, "&psConnection=0x%x -> 0x%x" % (pp, conn)

    def imp_PVRSRVInitSrvDisconnect(self):
        self.disconnect_arg = self.arg(1)
        return PVRSRV_OK, "psConnection=0x%x, bInitSuccess=%d" % (self.arg(0), self.arg(1))

    def imp_PVRSRVEnumerateDevices(self):
        conn, pnum, ids = self.arg(0), self.arg(1), self.arg(2)
        self.w32(pnum, 1)
        # PVRSRV_DEVICE_IDENTIFIER: type SGX, class 3D, index 0, two NULL pdump names
        self.uc.mem_write(ids, struct.pack("<5I", PVRSRV_DEVICE_TYPE_SGX, 0, 0, 0, 0))
        return PVRSRV_OK, ("psConnection=0x%x -> 1 device {type=SGX(7), class=3D(0), index=0}" % conn)

    def imp_PVRSRVAcquireDeviceData(self):
        conn, idx, pdev, typ = self.arg(0), self.arg(1), self.arg(2), self.arg(3)
        # type 0 (UNKNOWN) means "match by index only" in PVRSRVAcquireDeviceDataKM
        if typ not in (0, PVRSRV_DEVICE_TYPE_SGX) or idx != 0:
            raise EmuError("AcquireDeviceData: unexpected index %d type %d" % (idx, typ))
        self.w32(pdev, conn)
        self.w32(pdev + 4, H_DEVCOOKIE)
        return PVRSRV_OK, "devindex=%d type=%d -> {conn=0x%x, cookie=0x%x}" % (idx, typ, conn, H_DEVCOOKIE)

    def imp_SGXGetInfoForSrvInit(self):
        pdev, out = self.arg(0), self.arg(1)
        self.heap_info_addr = out
        blob = struct.pack("<I", self.pd_addr)
        for i in range(32):
            if i < len(HEAPS):
                hid, name, base, size, attr = HEAPS[i]
                blob += struct.pack("<6I", HEAP_ID(hid), self.h_heap + 0x40 * i, base, size, attr, 0)
            else:
                blob += struct.pack("<6I", 0xFFFFFFFF, 0, 0, 0, 0, 0)
        assert len(blob) == 772
        self.uc.mem_write(out, blob)
        return PVRSRV_OK, "psDevData=0x%x -> PD=0x%08x, %d heaps" % (pdev, self.pd_addr, len(HEAPS))

    def imp_PVRSRVCreateAppHintState(self):
        pp = self.arg(2)
        st = self.new_struct(8, "apphintstate")
        self.w32(pp, st)
        return 0, "module=%d, app=%r" % (self.arg(0), self.cstr(self.arg(1)))

    def imp_PVRSRVFreeAppHintState(self):
        return 0, "module=%d" % self.arg(0)

    def imp_PVRSRVGetAppHint(self):
        name = self.cstr(self.arg(1))
        typ, pdef, pret = self.arg(2), self.arg(3), self.arg(4)
        if typ == 1:
            val = self.cstr(pdef)
            self.uc.mem_write(pret, val.encode("latin-1") + b"\0")
            shown = repr(val)
        else:
            val = self.u32(pdef)
            self.w32(pret, val)
            shown = "0x%x" % val
        self.apphints.append((name, typ, shown))
        return 0, "%s type=%d default=%s -> not set, default returned" % (name, typ, shown)

    def imp_PVRSRVAllocUserModeMem(self):
        size = self.arg(0)
        a = self.heap_alloc(size)
        return a, "size=0x%x" % size

    def imp_PVRSRVFreeUserModeMem(self):
        return 0, "ptr=0x%x" % self.arg(0)

    def imp_PVRSRVMemCopy(self):
        d, s, n = self.arg(0), self.arg(1), self.arg(2)
        if n:
            self.uc.mem_write(d, bytes(self.uc.mem_read(s, n)))
        return d, "dst=0x%x src=0x%x size=0x%x" % (d, s, n)

    def imp_PVRSRVMemSet(self):
        d, v, n = self.arg(0), self.arg(1) & 0xFF, self.arg(2)
        if n:
            self.uc.mem_write(d, bytes([v]) * n)
        return d, "dst=0x%x val=0x%02x size=0x%x" % (d, v, n)

    def imp_PVRSRVAllocDeviceMem(self):
        pdev, hheap, flags, size, align, ppmi = [self.arg(i) for i in range(6)]
        hidx = (hheap - self.h_heap) // 0x40
        if hheap < self.h_heap or hidx >= len(HEAPS) or (hheap - self.h_heap) % 0x40:
            raise EmuError("AllocDeviceMem: unknown heap handle 0x%x" % hheap)
        if flags & ~KNOWN_ALLOC_FLAGS:
            raise EmuError("AllocDeviceMem: unsupported flags 0x%x" % flags)
        hid, hname, hbase, hsize, _ = HEAPS[hidx]
        devva = place_allocation(hbase, self.heap_prev_end.get(hidx), size, align)
        if devva + size > hbase + hsize + PAGE:   # heap size excludes the 4 KiB guard page
            return PVRSRV_ERROR_OUT_OF_MEMORY, "heap %s full" % hname
        self.heap_prev_end[hidx] = devva + size
        bufsz = round_up(size, PAGE)
        buf = self.dev_next
        self.dev_next += bufsz
        if self.dev_next > self.devbuf_base + DEVBUF_SIZE:
            raise EmuError("emulator device buffer area exhausted")
        if self.fill:
            self.uc.mem_write(buf, bytes([self.fill]) * bufsz)
        a = Alloc(len(self.allocs), hidx, size, align, flags, devva, buf, self.site() - LIB_BASE,
                  self.h_meminfo + 0x100 * len(self.allocs))
        mi = self.new_struct(MEMINFO_SIZE, "meminfo")
        a.meminfo = mi
        lin = 0 if flags & MEM_NOUSERVIRTUAL else buf
        # pvLinAddr, pvLinAddrKM, sDevVAddr, ui32Flags, ui32ClientFlags, uAllocSize, psClientSyncInfo,
        # hMappingInfo, hKernelMemInfo, hResItem, ui64Stamp, psNext
        self.uc.mem_write(mi, struct.pack(
            "<IIIIIIIIIIQI", lin, self.km_base | (buf - self.devbuf_base), devva,
            flags & ~MEM_NOUSERVIRTUAL, 0, size, 0, 0, a.handle, 0, 0, 0)[:MEMINFO_SIZE - 4]
            + b"\0" * 4)
        self.w32(ppmi, mi)
        self.allocs.append(a)
        return PVRSRV_OK, ("heap=%d(%s) flags=0x%x size=0x%x align=0x%x -> #%d devVA=0x%08x cpu=0x%x"
                           % (hid, hname, flags, size, align, a.index, devva, lin))

    def alloc_of(self, mi):
        for a in self.allocs:
            if a.meminfo == mi:
                return a
        raise EmuError("meminfo pointer 0x%x is not an allocation" % mi)

    def imp_PVRSRVFreeDeviceMem(self):
        a = self.alloc_of(self.arg(1))
        a.freed = True
        if self.part2 is not None:
            self.frees_after_part2 = True
        return PVRSRV_OK, "#%d devVA=0x%08x" % (a.index, a.devva)

    def imp_SGXDevInitPart2(self):
        pdev, pinit = self.arg(0), self.arg(1)
        client = bytes(self.uc.mem_read(pinit, CLIENT_INIT_READ))
        self.part2 = {"client": client,
                      "contents": [self.read_alloc(a) for a in self.allocs]}
        return PVRSRV_OK, ("psDevData=0x%x psInitInfo=0x%x (client init struct, build options 0x%08x)"
                           % (pdev, pinit, struct.unpack_from("<I", client, 0x20)[0]))

    # --- libsrv_um debug / libc ------------------------------------------------
    def imp_PVRSRVDebugPrintf(self):
        level, fname, line, fmt = self.arg(0), self.cstr(self.arg(1)), self.arg(2), self.cstr(self.arg(3))
        text = self.format(fmt, 4)
        if level <= 2:
            self.dbg_errors.append((level, fname, line, text))
        print("[pvr dbg %d] %s:%d: %s" % (level, fname, line, text), file=sys.stderr)
        return 0, "level=%d %s:%d %r" % (level, fname, line, text)

    def imp_PVRSRVDebugAssertFail(self):
        raise EmuError("PVRSRVDebugAssertFail(%s, %d) from 0x%x"
                       % (self.cstr(self.arg(0)), self.arg(1), self.site() - LIB_BASE))

    def imp___cxa_atexit(self):
        return 0, ""

    def imp___register_atfork(self):
        return 0, ""

    def imp___cxa_finalize(self):
        return 0, ""

    def format(self, fmt, first):
        import re
        n = [first]

        def nxt():
            v = self.arg(n[0])
            n[0] += 1
            return v

        def sub(m):
            flags, width, prec, length, conv = m.groups()
            if conv == "%":
                return "%"
            if length == "ll":
                raise EmuError("64-bit printf argument not supported: %r" % fmt)
            v = nxt()
            spec = "%" + flags + (width or "") + (("." + prec) if prec is not None else "")
            if conv == "s":
                return (spec + "s") % self.cstr(v)
            if conv in "di":
                v = v - (1 << 32) if v & 0x80000000 else v
                return (spec + "d") % v
            if conv in "uxXo":
                return (spec + conv.replace("u", "d")) % v
            if conv == "c":
                return chr(v & 0xFF)
            if conv == "p":
                return "0x%x" % v
            raise EmuError("printf conversion %%%s not supported: %r" % (conv, fmt))
        return re.sub(r"%([-+ #0]*)(\d+)?(?:\.(\d+))?(hh|h|ll|l|z|t)?([diuxXocspf%])", sub, fmt)

    # --- running -----------------------------------------------------------------
    def read_alloc(self, a):
        return bytes(self.uc.mem_read(a.buf, a.size))

    def call(self, addr):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_SP, self.stack_top - 16)
        uc.reg_write(UC_ARM_REG_LR, SENTINEL | 1)
        try:
            uc.emu_start(addr | 1, SENTINEL)
        except UcError as e:
            pc = uc.reg_read(UC_ARM_REG_PC)
            if self.error is None:
                self.error = EmuError("emulation fault: %s at pc=0x%x (lib+0x%x), lr=0x%x"
                                      % (e, pc, pc - LIB_BASE, uc.reg_read(UC_ARM_REG_LR)))
        if self.error:
            raise self.error
        if self.uc.reg_read(UC_ARM_REG_PC) & ~1 != SENTINEL:
            raise EmuError("emulation stopped early at pc=0x%x" % self.uc.reg_read(UC_ARM_REG_PC))
        return uc.reg_read(UC_ARM_REG_R0)

    def run(self):
        for fn in self.init_funcs:
            self.call(fn)
        self.ret = self.call(self.entry)
        self.final = [self.read_alloc(a) for a in self.allocs]


# ---------------------------------------------------------------------------
# Decoding of the client-side init struct (libsrv_um SGXDevInitPart2 mapping)
# ---------------------------------------------------------------------------
# Each entry: (client offset, kernel SGX_BRIDGE_INIT_INFO offset, bytes, kind)
# Derived from the disassembly of SGXDevInitPart2 (libsrv_um 0x8944): the
# bridge input is { u32 flags; u32 hDevCookie; SGX_BRIDGE_INIT_INFO } (kernel
# offsets below are relative to SGX_BRIDGE_INIT_INFO, i.e. bridge offset - 8).
MEM, WORD, DEVVA, COPY = "mem", "word", "devva", "copy"
K_BUILD, K_SIZES, K_KICK, K_SCRIPTS = 0xA04, 0xA08, 0x18, 0x44
K_DEVDATA, K_MEMH = 0xA68, 0xAC8
CLIENT_MAP = [
    (0x00, 0x00, 4, MEM),     # hKernelCCBMemInfo
    (0x0C, 0x04, 4, MEM),     # hKernelCCBCtlMemInfo
    (0x08, 0x08, 4, MEM),     # hKernelCCBEventKickerMemInfo
    (0x18, 0x0C, 4, MEM),     # hKernelSGXHostCtlMemInfo
    (0x14, 0x10, 4, MEM),     # hKernelSGXTA3DCtlMemInfo
    (0x1C, 0x14, 4, MEM),     # hKernelSGXMiscMemInfo
    (0x8C, K_KICK, 44, COPY),         # aui32HostKickAddr[11]
    (0xE8, K_SCRIPTS, 2496, COPY),    # sScripts
    (0x20, K_BUILD, 4, WORD),         # ui32ClientBuildOptions
    (0x24, K_SIZES, 52, COPY),        # sSGXStructSizes (13 words)
    (0x5C, 0xA3C, 4, MEM),    # hKernelHWPerfCBMemInfo
    (0x60, 0xA40, 4, MEM),    # hKernelTASigBufferMemInfo
    (0x64, 0xA44, 4, MEM),    # hKernel3DSigBufferMemInfo
    (0x58, 0xA48, 4, MEM),    # hKernelEDMStatusBufferMemInfo
    (0xD0, 0xA4C, 4, WORD),   # ui32EDMTaskReg0
    (0xD4, 0xA50, 4, WORD),   # ui32EDMTaskReg1
    (0xD8, 0xA54, 4, WORD),   # ui32ClkGateCtl
    (0xDC, 0xA58, 4, WORD),   # ui32ClkGateCtl2
    (0xE0, 0xA5C, 4, WORD),   # ui32ClkGateStatusReg
    (0xE4, 0xA60, 4, WORD),   # ui32ClkGateStatusMask
    (0xC8, 0xA64, 4, WORD),   # ui32CacheControl
    (0xCC, K_DEVDATA + 4 * 4, 4, WORD),    # asInitDevData[4]
    (0xC0, K_DEVDATA + 4 * 6, 4, WORD),    # [6]
    (0xC4, K_DEVDATA + 4 * 7, 4, WORD),    # [7]
    (0xB8, K_DEVDATA + 4 * 8, 4, WORD),    # [8]
    (0xBC, K_DEVDATA + 4 * 9, 4, WORD),    # [9]
    (0x68, K_MEMH + 4 * 0, 4, MEM),        # asInitMemHandles[0]
    (0x6C, K_MEMH + 4 * 1, 4, MEM),
    (0x74, K_MEMH + 4 * 2, 4, MEM),
    (0x70, K_MEMH + 4 * 3, 4, MEM),
    (0x78, K_MEMH + 4 * 4, 4, MEM),
    (0x7C, K_MEMH + 4 * 5, 4, MEM),
    (0x80, K_MEMH + 4 * 6, 4, MEM),
    (0x84, K_MEMH + 4 * 7, 4, MEM),
    (0x88, K_MEMH + 4 * 8, 4, MEM),
    (0xAA8, K_MEMH + 4 * 9, 4, MEM),
    (0xAAC, K_MEMH + 4 * 10, 4, MEM),
    (0xAA8, K_DEVDATA + 4 * 10, 4, DEVVA),  # asInitDevData[10] = devVA of stencil load buffer
    (0xAAC, K_DEVDATA + 4 * 11, 4, DEVVA),  # asInitDevData[11]
]
KERNEL_INFO_SIZE = 2832


def build_kernel_init_info(emu, client):
    """What libsrv_um's SGXDevInitPart2 puts into SGX_BRIDGE_INIT_INFO."""
    out = bytearray(KERNEL_INFO_SIZE)
    by_ptr = {a.meminfo: a for a in emu.allocs}
    covered = [False] * len(client)
    for coff, koff, n, kind in CLIENT_MAP:
        if kind == COPY:
            out[koff:koff + n] = client[coff:coff + n]
        else:
            v = struct.unpack_from("<I", client, coff)[0]
            if kind == WORD:
                pass
            else:
                if v == 0:
                    pass
                elif v not in by_ptr:
                    raise EmuError("client init struct +0x%x = 0x%x is not a meminfo" % (coff, v))
                elif kind == MEM:
                    v = by_ptr[v].handle
                else:
                    v = by_ptr[v].devva
            struct.pack_into("<I", out, koff, v)
        for i in range(coff, coff + n):
            covered[i] = True
    unmapped = {}
    for off in range(0, len(client), 4):
        if not any(covered[off:off + 4]):
            v = struct.unpack_from("<I", client, off)[0]
            if v:
                unmapped["0x%x" % off] = v
    return bytes(out), unmapped


def decode_scripts(raw):
    names = [("asInitCommandsPart1", 64), ("asInitCommandsPart2", 64),
             ("asDeinitCommands", 16), ("asSGXREGDebugCommandsSlave", 64)]
    res, pos = {}, 0
    for name, n in names:
        entries, halted = [], False
        for i in range(n):
            op, off, val = struct.unpack_from("<III", raw, pos + 12 * i)
            if not halted:
                entries.append({"op": OP_NAMES.get(op, str(op)), "offset": off, "value": val})
                halted = op == 4
            elif (op, off, val) != (0, 0, 0):
                entries.append({"op": "AFTER_HALT:%s" % OP_NAMES.get(op, op), "offset": off, "value": val})
        res[name] = entries
        pos += 12 * n
    return res


def decode_kernel_info(emu, kinfo, unmapped_client):
    w = lambda off: struct.unpack_from("<I", kinfo, off)[0]
    by_handle = {a.handle: a.index for a in emu.allocs}

    def mem(off):
        v = w(off)
        return None if v == 0 else by_handle[v]
    kicks = {CMD_NAMES[i]: w(K_KICK + 4 * i) for i in range(11)}
    return {
        "hKernelCCBMemInfo": mem(0), "hKernelCCBCtlMemInfo": mem(4),
        "hKernelCCBEventKickerMemInfo": mem(8), "hKernelSGXHostCtlMemInfo": mem(0xC),
        "hKernelSGXTA3DCtlMemInfo": mem(0x10), "hKernelSGXMiscMemInfo": mem(0x14),
        "aui32HostKickAddr": kicks,
        "aui32HostKickAddr_list": [w(K_KICK + 4 * i) for i in range(11)],
        "sScripts": decode_scripts(kinfo[K_SCRIPTS:K_SCRIPTS + 2496]),
        "ui32ClientBuildOptions": w(K_BUILD),
        "sSGXStructSizes": {SIZE_NAMES[i]: w(K_SIZES + 4 * i) for i in range(13)},
        "hKernelHWPerfCBMemInfo": mem(0xA3C), "hKernelTASigBufferMemInfo": mem(0xA40),
        "hKernel3DSigBufferMemInfo": mem(0xA44), "hKernelEDMStatusBufferMemInfo": mem(0xA48),
        "ui32EDMTaskReg0": w(0xA4C), "ui32EDMTaskReg1": w(0xA50),
        "ui32ClkGateCtl": w(0xA54), "ui32ClkGateCtl2": w(0xA58),
        "ui32ClkGateStatusReg": w(0xA5C), "ui32ClkGateStatusMask": w(0xA60),
        "ui32CacheControl": w(0xA64),
        "asInitDevData": [w(K_DEVDATA + 4 * i) for i in range(24)],
        "asInitMemHandles": [mem(K_MEMH + 4 * i) for i in range(18)],
        "client_words_not_copied_to_kernel_struct": unmapped_client,
    }


# ---------------------------------------------------------------------------
def run_once(lib, pd, fill, shifted=False):
    emu = Emu(lib, pd, fill, shifted)
    emu.run()
    return emu


def final_contents(emu):
    return emu.part2["contents"] if emu.frees_after_part2 else emu.final


def diff_ranges(a, b):
    ranges, i = [], 0
    while i < len(a):
        if a[i] != b[i]:
            j = i
            while j < len(a) and a[j] != b[j]:
                j += 1
            ranges.append([i, j - i])
            i = j
        else:
            i += 1
    return ranges


def kinfo_field(off):
    if off < 0x18:
        return "handle[%d]" % (off // 4)
    if off < K_SCRIPTS:
        return "aui32HostKickAddr[%d]" % ((off - K_KICK) // 4)
    if off < K_SCRIPTS + 2496:
        n = (off - K_SCRIPTS) // 12
        for name, cnt in (("asInitCommandsPart1", 64), ("asInitCommandsPart2", 64),
                          ("asDeinitCommands", 16), ("asSGXREGDebugCommandsSlave", 64)):
            if n < cnt:
                return "sScripts.%s[%d]" % (name, n)
            n -= cnt
    if K_SIZES <= off < K_SIZES + 52:
        return "sSGXStructSizes[%d]" % ((off - K_SIZES) // 4)
    if K_DEVDATA <= off < K_MEMH:
        return "asInitDevData[%d]" % ((off - K_DEVDATA) // 4)
    if off >= K_MEMH:
        return "asInitMemHandles[%d]" % ((off - K_MEMH) // 4)
    names = {K_BUILD: "ui32ClientBuildOptions", 0xA3C: "hKernelHWPerfCBMemInfo",
             0xA40: "hKernelTASigBufferMemInfo", 0xA44: "hKernel3DSigBufferMemInfo",
             0xA48: "hKernelEDMStatusBufferMemInfo", 0xA4C: "ui32EDMTaskReg0",
             0xA50: "ui32EDMTaskReg1", 0xA54: "ui32ClkGateCtl", 0xA58: "ui32ClkGateCtl2",
             0xA5C: "ui32ClkGateStatusReg", 0xA60: "ui32ClkGateStatusMask",
             0xA64: "ui32CacheControl"}
    return names.get(off & ~3, "kernel_info+0x%x" % off)


def script_slack(kinfo):
    """Script bytes the kernel never uses: whole entries after a HALT, the
    offset/value of a HALT, and the value of READ/PRINT entries (read-back slot)."""
    slack, pos = set(), K_SCRIPTS
    for cnt in (64, 64, 16, 64):
        halted = False
        for i in range(cnt):
            e = pos + 12 * i
            op = struct.unpack_from("<I", kinfo, e)[0]
            if halted:
                slack.update(range(e, e + 12))
            elif op == 4:
                halted = True
                slack.update(range(e + 4, e + 12))
            elif op in (2, 3):
                slack.update(range(e + 8, e + 12))
        pos += 12 * cnt
    return slack


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("libsrv_init")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("outdir", nargs="?",
                    default=os.path.normpath(os.path.join(here, "..", "..", "build", "sgx", "init")))
    ap.add_argument("--pd-addr", default="0x8F000000",
                    help="kernel page-directory physical address returned by SGXGetInfoForSrvInit")
    args = ap.parse_args()
    pd = int(args.pd_addr, 0)

    emu = run_once(args.libsrv_init, pd, 0x00)
    # Run B: uninitialised memory filled with 0xA5 instead of 0x00; bytes that
    # differ were not written by the library (or depend on uninitialised data).
    fillrun = run_once(args.libsrv_init, pd, 0xA5)
    # Run C: all host-side addresses and fake handles moved; differing bytes
    # depend on host pointers.
    ptrrun = run_once(args.libsrv_init, pd, 0x00, shifted=True)

    if emu.ret != PVRSRV_OK:
        raise SystemExit("SrvInit returned %d; debug errors: %s" % (emu.ret, emu.dbg_errors))
    if emu.part2 is None:
        raise SystemExit("SGXDevInitPart2 was never called")
    if emu.dbg_errors:
        print("note: library logged error-level messages: %s" % [e[3] for e in emu.dbg_errors],
              file=sys.stderr)
    for other in (fillrun, ptrrun):
        if [a.devva for a in emu.allocs] != [a.devva for a in other.allocs]:
            raise SystemExit("allocation sequence depends on uninitialised memory or host addresses")

    contents = final_contents(emu)
    changed_after_part2 = [i for i, (a, b) in enumerate(zip(emu.part2["contents"], emu.final)) if a != b]

    os.makedirs(args.outdir, exist_ok=True)
    for f in os.listdir(args.outdir):
        if f.startswith("alloc_") and f.endswith(".bin"):
            os.remove(os.path.join(args.outdir, f))

    kinfo, unmapped = build_kernel_init_info(emu, emu.part2["client"])
    init = decode_kernel_info(emu, kinfo, unmapped)

    # dependence of the init struct on uninitialised memory / host addresses
    fill_kinfo, _ = build_kernel_init_info(fillrun, fillrun.part2["client"])
    slack = script_slack(kinfo)
    fill_dep = sorted({kinfo_field(o) for o in range(len(kinfo))
                       if kinfo[o] != fill_kinfo[o] and o not in slack},
                      key=lambda n: (n.split("[")[0], n))
    slack_diff = sum(1 for o in slack if kinfo[o] != fill_kinfo[o])
    ptr_init = decode_kernel_info(ptrrun, build_kernel_init_info(ptrrun, ptrrun.part2["client"])[0], {})
    init_cmp = dict(init)
    init_cmp.pop("client_words_not_copied_to_kernel_struct")
    ptr_init.pop("client_words_not_copied_to_kernel_struct")
    ptr_dep = sorted(k for k in init_cmp if init_cmp[k] != ptr_init[k])

    allocs = []
    for a, data, fdata, pdata in zip(emu.allocs, contents, final_contents(fillrun), final_contents(ptrrun)):
        fname = "alloc_%02d.bin" % a.index
        with open(os.path.join(args.outdir, fname), "wb") as f:
            f.write(data)
        allocs.append({
            "index": a.index, "heap_id": HEAPS[a.heap][0], "heap_name": HEAPS[a.heap][1],
            "size": a.size, "alignment": a.align, "flags": a.flags, "devva": a.devva,
            "name": None, "call_site": "libsrv_init+0x%x" % a.site, "file": fname,
            "freed": a.freed, "no_cpu_mapping": bool(a.flags & MEM_NOUSERVIRTUAL),
            "never_written_ranges": diff_ranges(data, fdata),
            "host_pointer_dependent_ranges": diff_ranges(data, pdata),
        })
    with open(os.path.join(args.outdir, "kinit_info.bin"), "wb") as f:
        f.write(kinfo)

    manifest = {
        "library": os.path.basename(args.libsrv_init),
        "srvinit_return": emu.ret,
        "srvinit_arguments": "none (void SrvInit(void); returns PVRSRV_ERROR)",
        "page_directory_phys_addr": pd,
        "heap_table": [{"id": h[0], "name": h[1], "base": h[2], "size": h[3], "attribs": h[4]} for h in HEAPS],
        "allocation_policy": place_allocation.__doc__.strip(),
        "allocations": allocs,
        "contents_taken_at": ("SGXDevInitPart2 call (library frees allocations afterwards)"
                              if emu.frees_after_part2 else "SrvInit return"),
        "allocations_changed_between_DevInitPart2_and_return": changed_after_part2,
        "init_info_file": "kinit_info.bin (2832-byte SGX_BRIDGE_INIT_INFO as libsrv_um builds it; "
                          "handles are the emulator's fake hKernelMemInfo values 0xD0000000 + 0x100*index)",
        "init_info_fields_depending_on_uninitialised_memory": fill_dep,
        "init_info_script_bytes_after_halt_that_are_uninitialised": slack_diff,
        "init_info_fields_depending_on_host_pointers": ptr_dep,
        "devinitpart2": init,
        "apphints_queried": [{"name": n, "type": t, "default": d} for n, t, d in emu.apphints],
        "meminfo_and_other_struct_fields_read_by_library": sorted(
            "%s+0x%x" % (l, o) for l, o in emu.struct_reads),
        "library_error_level_messages": [e[3] for e in emu.dbg_errors],
        "notes": [
            "The library hands libsrv_um a client-side struct (SGXDevInitPart2 argument 2, 0xAB0 bytes, "
            "malloc'd and not cleared). It differs from the kernel's SGX_BRIDGE_INIT_INFO; the mapping was "
            "taken from the disassembly of libsrv_um SGXDevInitPart2 (0x8944) and is CLIENT_MAP in "
            "srvinit_emu.py. devinitpart2 above is decoded from the converted kernel-layout struct.",
            "never_written_ranges lists bytes whose final value differed between a run with uninitialised "
            "memory filled with 0x00 and one filled with 0xA5; the .bin files hold the 0x00 run. On the "
            "device these bytes are whatever the kernel's page allocator returns (the kernel clears the CCB, "
            "CCB control and kicker word in DevInitPart2).",
            "host_pointer_dependent_ranges lists bytes that changed when every emulator-side address and "
            "fake handle was moved; empty means the image does not contain CPU pointers or handles.",
            "Allocation names: this libsrv_um has no name parameter on PVRSRVAllocDeviceMem, so none are "
            "recorded; call_site is the return address inside libsrv_init.so.",
        ],
    }
    with open(os.path.join(args.outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    with open(os.path.join(args.outdir, "calls.txt"), "w") as f:
        f.write("\n".join(emu.calls) + "\n")
    print("SrvInit returned %d; %d allocations, %d import calls -> %s"
          % (emu.ret, len(emu.allocs), len(emu.calls), args.outdir))


if __name__ == "__main__":
    main()
