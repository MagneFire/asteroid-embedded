#!/usr/bin/env python3
"""Runs host.c built for the Cortex-M4 (see run-emu.sh) under Unicorn with a heap of HEAP_BYTES:
prints the heap's peak after each screen and writes the screens as PNG files to OUT_DIR. Exit
status 2 if the UI ran out of heap.
usage: emu.py ELF HEAP_BYTES OUT_DIR [WIDTHxHEIGHT, as host.c was built for]"""
import os
import struct
import sys
import zlib

from elftools.elf.elffile import ELFFile
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_MCLASS, UC_MODE_THUMB, Uc, UcError
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP
from unicorn.arm_const import UC_CPU_ARM_CORTEX_M4

RAM, RAM_SIZE = 0x20000000, 4 << 20

elf_path, heap, out = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
W, H = map(int, (sys.argv[4] if len(sys.argv) > 4 else "402x476").split("x"))
os.makedirs(out, exist_ok=True)
elf = ELFFile(open(elf_path, "rb"))
syms = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}

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


status = None


def shot(uc, addr, size, _):
    name, pixels, peak = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
    print(f"{cstr(name):16s} heap peak {peak:6d}", flush=True)
    if pixels:
        png(os.path.join(out, cstr(name) + ".png"), bytes(uc.mem_read(pixels, W * H * 2)))


def done(uc, addr, size, _):
    global status
    code, msg, n = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
    status = code
    if code:
        print(f"exit {code}: {bytes(uc.mem_read(msg, n)).decode(errors='replace')}", file=sys.stderr)
    uc.emu_stop()


for name, fn in (("emu_shot", shot), ("emu_exit", done)):
    a = syms[name] & ~1
    uc.hook_add(UC_HOOK_CODE, fn, begin=a, end=a)
uc.reg_write(UC_ARM_REG_SP, RAM + RAM_SIZE)
uc.reg_write(UC_ARM_REG_LR, 0xFFFFFFFF)
try:
    uc.emu_start(syms["reset"] | 1, 0xFFFFFFFE)
except UcError as e:
    print(f"fault at {uc.reg_read(UC_ARM_REG_PC):#x}: {e}", file=sys.stderr)
    sys.exit(4)
sys.exit(status if status is not None else 4)
