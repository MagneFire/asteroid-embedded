#!/usr/bin/env python3
"""Runs host.c built for the Cortex-M4 (see run-emu.sh) under Unicorn with a heap of HEAP_BYTES:
prints the heap's peak after each screen and writes the screens as PNG files to OUT_DIR. Exit
status 2 if the UI ran out of heap.

--profile SCREEN,... counts every ui_draw that draws on those screens: its instructions, the code it
runs against the Apollo3's 16 KiB flash cache, and its lines. Constants read from flash are not
counted: Unicorn 2.1.4's read hooks break pop {..., pc}. The hottest functions and source lines go
to OUT_DIR/profile-SCREEN.txt. --layout FIRMWARE_ELF counts the cache at the addresses the
firmware has the UI's functions at; code of host.c stays where it is.
usage: emu.py ELF HEAP_BYTES OUT_DIR [WIDTHxHEIGHT, as host.c was built for] [--profile SCREEN,...]
       [--layout FIRMWARE_ELF]"""
import bisect
import os
import struct
import subprocess
import sys
import zlib
from collections import Counter, OrderedDict

from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_ARM, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc, UcError
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP
from unicorn.arm_const import UC_CPU_ARM_CORTEX_M4

RAM, RAM_SIZE = 0x20000000, 4 << 20
LINE = 16

args = sys.argv[1:]


def option(name):
    if name not in args:
        return None
    i = args.index(name)
    value = args[i + 1]
    del args[i:i + 2]
    return value


profiled = set((option("--profile") or "").split(",")) - {""}
layout = option("--layout")
elf_path, heap, out = args[0], int(args[1], 0), args[2]
W, H = map(int, (args[3] if len(args) > 3 else "402x476").split("x"))
os.makedirs(out, exist_ok=True)
elf = ELFFile(open(elf_path, "rb"))
symtab = list(elf.get_section_by_name(".symtab").iter_symbols())
syms = {s.name: s["st_value"] for s in symtab}

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M4)
uc.mem_map(0, 16 << 20)
uc.mem_map(RAM, RAM_SIZE)
for seg in elf.iter_segments():
    if seg["p_type"] == "PT_LOAD":
        uc.mem_write(seg["p_vaddr"], seg.data())
uc.mem_write(syms["heap_size"], struct.pack("<I", heap))


def png(path, pixels):
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for (p,) in struct.iter_unpack("<H", pixels[y * W * 2:(y + 1) * W * 2]):
            raw += bytes(((p >> 11) * 255 // 31, (p >> 5 & 63) * 255 // 63, (p & 31) * 255 // 31))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def cstr(addr):
    return bytes(uc.mem_read(addr, 64)).split(b"\0")[0].decode()


class Cache:
    """The Apollo3's flash cache: 16-byte lines in sets of ways, each replacing its least recently
    used line."""

    def __init__(self, sets, ways):
        self.sets, self.ways = sets, ways
        self.lines = [OrderedDict() for _ in range(sets)]
        self.misses = 0

    def read(self, line):
        s = self.lines[line % self.sets]
        if line in s:
            s.move_to_end(line)
            return
        self.misses += 1
        s[line] = None
        if len(s) > self.ways:
            s.popitem(last=False)


class Draw:
    def __init__(self, screen):
        self.screen = screen
        self.instructions = 0
        self.blocks = Counter()
        self.code = set()
        self.caches = {"1-way 1024": Cache(1024, 1), "2-way 512": Cache(512, 2)}
        self.line_at = []
        self.widths = []
        self.ys = []
        self.misses_by_block = Counter()


if profiled:
    from capstone import CS_ARCH_ARM, CS_MODE_MCLASS, CS_MODE_THUMB, Cs

    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    funcs = sorted((s["st_value"] & ~1, s["st_size"], s.name) for s in symtab
                   if s["st_info"]["type"] == "STT_FUNC" and s["st_size"])
    starts = [f[0] for f in funcs]
    firmware = {}
    if layout:
        firmware = {s.name: s["st_value"] & ~1 for s in ELFFile(open(layout, "rb")).get_section_by_name(".symtab")
                    .iter_symbols() if s["st_info"]["type"] == "STT_FUNC"}
block_len = {}
cache_addr = {}


def firmware_addr(addr):
    """Where the firmware has the code at addr, if it has its function."""
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and addr < funcs[i][0] + funcs[i][1] and funcs[i][2] in firmware:
        return firmware[funcs[i][2]] + addr - funcs[i][0]
    return addr


def function_of(addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and addr < funcs[i][0] + funcs[i][1]:
        return funcs[i][2]
    return f"{addr:#x}"


status = None
screen = None
draw = None
draws = []
pending = []  # actions run between emu_start calls: hooks are only added and removed there
hooks = []


def on_block(uc, addr, size, _):
    n = block_len.get((addr, size))
    if n is None:
        n = block_len[addr, size] = sum(1 for _ in md.disasm_lite(bytes(uc.mem_read(addr, size)), addr))
    at = cache_addr.get(addr)
    if at is None:
        at = cache_addr[addr] = firmware_addr(addr)
    d = draw
    d.instructions += n
    d.blocks[addr, size] += 1
    for line in range(at // LINE, (at + size - 1) // LINE + 1):
        d.code.add(line)
        for c in d.caches.values():
            m = c.misses
            c.read(line)
            if c is d.caches["1-way 1024"] and c.misses != m:
                d.misses_by_block[addr, size] += 1


def draw_start(uc, addr, size, _):
    global draw
    if draw or screen not in profiled:
        return
    draw = Draw(screen)
    ret, sp = uc.reg_read(UC_ARM_REG_LR) & ~1, uc.reg_read(UC_ARM_REG_SP)
    pending.append(("start", ret, sp))
    uc.emu_stop()


def draw_return(uc, addr, size, sp):
    global draw
    if not draw or uc.reg_read(UC_ARM_REG_SP) != sp:
        return
    if uc.reg_read(UC_ARM_REG_R0) & 0xff:
        draws.append(draw)
    draw = None
    pending.append(("stop",))
    uc.emu_stop()


def line_drawn(uc, addr, size, _):
    if draw:
        draw.line_at.append(draw.instructions)
        draw.widths.append(uc.reg_read(UC_ARM_REG_R2))
        draw.ys.append(uc.reg_read(UC_ARM_REG_R0))


def report(draws_of_screen, name):
    path = os.path.join(out, f"profile-{name}.txt")
    with open(path, "w") as f:
        for i, d in enumerate(draws_of_screen):
            px = sum(d.widths)
            hdr = (f"{name} draw {i}: {d.instructions} instructions, {len(d.widths)} lines, {px} pixels "
                   f"({d.instructions / max(px, 1):.1f} per pixel); code {len(d.code) * LINE} bytes; cache misses "
                   + ", ".join(f"{k} {c.misses}" for k, c in d.caches.items()))
            print(hdr, flush=True)
            f.write(hdr + "\n")
            if d.line_at:
                setup = d.line_at[0]
                per = [b - a for a, b in zip(d.line_at, d.line_at[1:])]
                per.sort()
                f.write(f"  before the first line {setup} instructions; per line min {per[0] if per else 0} median "
                        f"{per[len(per) // 2] if per else 0} max {per[-1] if per else 0}\n")
                f.write("  instructions per line, in steps of 16 lines:\n   ")
                cost = [b - a for a, b in zip([setup] + d.line_at, d.line_at)]
                for j in range(0, len(cost), 16):
                    f.write(f" {d.ys[j]}:{sum(cost[j:j + 16]) // len(cost[j:j + 16])}")
                f.write("\n")
            if i:
                continue
            by_func, miss_func = Counter(), Counter()
            by_addr = Counter()
            for (a, s), n in d.blocks.items():
                k = block_len[a, s]
                by_func[function_of(a)] += n * k
                by_addr[a] += n * k
            for (a, s), n in d.misses_by_block.items():
                miss_func[function_of(a)] += n
            names = subprocess.run(["arm-none-eabi-c++filt"], input="\n".join(by_func), capture_output=True,
                                   text=True).stdout.splitlines()
            demangled = dict(zip(by_func, names)) if len(names) == len(by_func) else {}
            f.write("\n  instructions  %    code misses  function\n")
            for fn, n in by_func.most_common(60):
                f.write(f"  {n:12d} {100 * n / d.instructions:5.1f} {miss_func[fn]:8d}  {demangled.get(fn, fn)[:160]}\n")
            top = [a for a, _ in by_addr.most_common(40)]
            lines = subprocess.run(["arm-none-eabi-addr2line", "-e", elf_path, "-a", "-i", "-C"] + [f"{a:#x}" for a in top],
                                   capture_output=True, text=True).stdout
            f.write("\n  hottest blocks, with where they were inlined from:\n")
            for chunk in lines.split("0x")[1:]:
                rows = chunk.strip().splitlines()
                a = int(rows[0], 16)
                f.write(f"  {a:#x} {by_addr[a]:10d}  " + " <- ".join(r.split("/")[-1] for r in rows[1:]) + "\n")
    return path


def shot(uc, addr, size, _):
    name, pixels, peak = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
    print(f"{cstr(name):16s} heap peak {peak:6d}", flush=True)
    if pixels:
        png(os.path.join(out, cstr(name) + ".png"), bytes(uc.mem_read(pixels, W * H * 2)))
    mine = [d for d in draws if d.screen == cstr(name)]
    if mine:
        report(mine, cstr(name))


def on_screen(uc, addr, size, _):
    global screen
    screen = cstr(uc.reg_read(UC_ARM_REG_R0))


def done(uc, addr, size, _):
    global status
    code, msg, n = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
    status = code
    if code:
        print(f"exit {code}: {bytes(uc.mem_read(msg, n)).decode(errors='replace')}", file=sys.stderr)
    uc.emu_stop()


def at(name, fn, data=None):
    a = syms[name] & ~1
    return uc.hook_add(UC_HOOK_CODE, fn, data, begin=a, end=a)


for name, fn in (("emu_shot", shot), ("emu_exit", done), ("emu_screen", on_screen)):
    at(name, fn)
if profiled:
    at("ui_draw", draw_start)
    at("ui_draw_line", line_drawn)
uc.reg_write(UC_ARM_REG_SP, RAM + RAM_SIZE)
uc.reg_write(UC_ARM_REG_LR, 0xFFFFFFFF)
pc = syms["reset"] | 1
try:
    while True:
        uc.emu_start(pc, 0xFFFFFFFE)
        if not pending:
            break
        for action in pending:
            if action[0] == "start":
                _, ret, sp = action
                hooks += [uc.hook_add(UC_HOOK_BLOCK, on_block),
                          uc.hook_add(UC_HOOK_CODE, draw_return, sp, begin=ret, end=ret)]
            else:
                for h in hooks:
                    uc.hook_del(h)
                hooks = []
        # Code translated before a hook was added runs without it.
        uc.ctl_flush_tb()
        pending = []
        pc = uc.reg_read(UC_ARM_REG_PC) | 1
except UcError as e:
    print(f"fault at {uc.reg_read(UC_ARM_REG_PC):#x}: {e}", file=sys.stderr)
    sys.exit(4)
sys.exit(status if status is not None else 4)
