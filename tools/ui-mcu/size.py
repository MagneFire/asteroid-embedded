#!/usr/bin/env python3
"""What the UI library takes of an image, from the linker's map: flash and RAM by crate, by the
symbol each input section is named after, and the rest of the image. Constants without a name
(the glyph and icon fields, strings) count apart.
usage: size.py build/apollo3/asteroid.map"""
import re
import subprocess
import sys
from collections import defaultdict


def crate_of(symbol):
    symbol = re.sub(r"\[[0-9a-f]+\]", "", symbol)
    if "slint_generated" in symbol:
        return "generated Slint code"
    for module, part in (("asteroid_ui::fonts", "glyph tables"), ("asteroid_ui::icons", "icon tables")):
        if module in symbol:
            return part
    m = re.match(r"[<&*\s]*(?:dyn )?([a-z_][a-z0-9_]*)::", symbol)
    crate = m.group(1) if m else ("asteroid_ui" if symbol.startswith("ui_") else "other")
    return "core, alloc, libm, builtins" if crate in ("core", "alloc", "libm", "compiler_builtins") else crate


FLASH = (".text", ".rodata", ".ARM.exidx")
RAM = (".data", ".bss")

text = open(sys.argv[1]).read().split("Linker script and memory map", 1)[1]
size = defaultdict(lambda: defaultdict(int))
found = []
out = None
pending = None
for line in text.splitlines():
    m = re.match(r"^(\.[\w.]+)\s", line)
    if m:
        out = m.group(1)
    m = re.match(r"^ (\.\S+)$", line)
    if m:
        pending = m.group(1)
        continue
    m = re.match(r"^ (\.\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", line)
    name, pending = (m.group(1) if m and m.group(1) else pending), None
    if not m or out not in FLASH + RAM or int(m.group(2), 16) == 0:
        continue
    n, src = int(m.group(3), 16), m.group(4)
    kind = "flash" if out in FLASH else "ram"
    lib = re.search(r"libasteroid_ui\.a\((\w+?)-", src)
    if not lib:
        size["rest of the image"][kind] += n
        continue
    found.append((re.sub(r"^\.(text|rodata|data|bss)\.", "", name), kind, n, name.startswith(".rodata..L")))

names = subprocess.run(["arm-none-eabi-c++filt"], input="\n".join(f[0] for f in found), capture_output=True, text=True,
                       check=True).stdout.splitlines()
for (_, kind, n, anon), symbol in zip(found, names):
    size["unnamed constants (glyph and icon fields, strings)" if anon else crate_of(symbol)][kind] += n

ui = {k: v for k, v in size.items() if k != "rest of the image"}
flash = sum(v["flash"] for v in ui.values())
ram = sum(v["ram"] for v in ui.values())
print(f"UI: {flash} bytes ({flash / 1024:.1f} KiB) of flash, {ram} bytes of static RAM")
rest = size["rest of the image"]
print(f"rest of the image: {rest['flash']} bytes of flash, {rest['ram']} bytes of RAM\n")
for crate, v in sorted(ui.items(), key=lambda e: -e[1]["flash"]):
    if v["flash"] or v["ram"]:
        print(f"  {v['flash']:8d}  {v['flash'] / 1024:6.1f} KiB  {v['ram']:6d}  {crate}")
