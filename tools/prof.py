#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Names the functions in the output of the console's prof command.

    adb shell prof 4000; (use the watch); adb shell prof | tools/prof.py build/omap3/asteroid.elf
"""
import bisect
import subprocess
import sys

BLOCK = 64


def main():
    elf = sys.argv[1]
    nm = subprocess.run(["arm-none-eabi-nm", "-n", "-C", elf], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in nm.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16) & ~1, parts[2]))
    addrs = [a for a, _ in syms]
    total, by_name = 0, {}
    for line in sys.stdin:
        parts = line.split()
        if len(parts) != 2:
            continue
        try:
            addr, count = int(parts[0], 16), int(parts[1])
        except ValueError:
            continue
        # A block is counted for the function that holds its middle.
        i = bisect.bisect_right(addrs, addr + BLOCK // 2) - 1
        name = syms[i][1] if i >= 0 else hex(addr)
        by_name[name] = by_name.get(name, 0) + count
        total += count
    for name, count in sorted(by_name.items(), key=lambda kv: -kv[1]):
        print("%5.1f%% %5d  %s" % (100.0 * count / total, count, name[:110]))
    print("%d samples in the blocks shown" % total)


if __name__ == "__main__":
    main()
