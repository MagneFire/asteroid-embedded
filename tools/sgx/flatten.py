#!/usr/bin/env python3
"""Writes recordings as lines a C test can read.

    flatten.py [INIT_DIR]           -> INIT_DIR/init.txt (build/sgx/init)
        alloc <index> <heap> <size> <flags> <address> <file or ->
        script <before|after|off> <register> <value>

    flatten.py draw [DRAW_DIR]      -> DRAW_DIR/<scenario>/flat.txt (build/sgx/draw)
        alloc <index> <heap> <size> <address> <file>
        kick <index> <shared header offset in the TA CCB> <first> <last> <the six command data words>
        hostkick <microkernel address of the TA command, from the init recording>
"""
import json
import sys
from pathlib import Path


def init(out):
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


def draw(top):
    m0 = json.loads((top / ".." / "init" / "manifest.json").read_text())
    ta = m0["devinitpart2"]["aui32HostKickAddr"]["TA"]
    for path in sorted(top.glob("*/manifest.json")):
        m = json.loads(path.read_text())
        lines = [f"alloc {a['index']} {a['heap_id']} {a['size']:#x} {a['devva']:#x} {a['file']}" for a in m["allocations"]]
        for k in m["kicks"]:
            f = k["fields"]
            data = " ".join(f"{d:#x}" for d in f["sCommand"]["ui32Data"])
            lines.append(f"kick {k['index']} {k['ccb_offset']:#x} {f['bFirstKickOrResume']} {f['bLastInScene']} {data}")
        lines.append(f"hostkick {ta:#x}")
        (path.parent / "flat.txt").write_text("\n".join(lines) + "\n")


if len(sys.argv) > 1 and sys.argv[1] == "draw":
    draw(Path(sys.argv[2] if len(sys.argv) > 2 else "build/sgx/draw"))
else:
    init(Path(sys.argv[1] if len(sys.argv) > 1 else "build/sgx/init"))
