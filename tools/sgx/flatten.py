#!/usr/bin/env python3
"""Writes the reference of srvinit_emu.py as lines a C test can read.

    alloc <index> <heap> <size> <flags> <address> <file or ->
    script <before|after|off> <register> <value>
"""
import json
import sys
from pathlib import Path

out = Path(sys.argv[1] if len(sys.argv) > 1 else "build/sgx/init")
m = json.loads((out / "manifest.json").read_text())
lines = []
for a in m["allocations"]:
    name = "-" if a["no_cpu_mapping"] else a["file"]
    lines.append(f"alloc {a['index']} {a['heap_id']} {a['size']:#x} {a['flags']:#x} {a['devva']:#x} {name}")
scripts = m["devinitpart2"]["sScripts"]
for key, name in (("asInitCommandsPart1", "before"), ("asInitCommandsPart2", "after"), ("asDeinitCommands", "off")):
    for e in scripts[key]:
        if e["op"] == "HALT":
            break
        if e["op"] != "WRITE":
            sys.exit(f"{key}: unexpected {e['op']}")
        lines.append(f"script {name} {e['offset']:#x} {e['value']:#x}")
(out / "init.txt").write_text("\n".join(lines) + "\n")
