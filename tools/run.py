#!/usr/bin/env python3
"""Boot build/asteroid.img over fastboot, wait for the watch to come back to
fastboot, fetch the persistent log region and print the console tail, the
fault record and the symbolized addresses in it."""
import argparse, os, re, struct, subprocess, sys, time

BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")
IMG = os.path.join(BUILD, "asteroid.img")
ELF = os.path.join(BUILD, "asteroid.elf")
RAW = os.path.join(BUILD, "ramoops.bin")
SIG = 0x43474244
RECORD_SIZE = 0x2000
CONSOLE_SIZE = 0x40000
REGION_SIZE = 0x80000

def fastboot(*args, check=True, quiet=False):
    r = subprocess.run(["fastboot", *args], capture_output=True, text=True)
    if check and r.returncode:
        sys.exit("fastboot %s failed:\n%s%s" % (" ".join(args), r.stdout, r.stderr))
    if not quiet and (r.stdout.strip() or r.stderr.strip()):
        print((r.stdout + r.stderr).rstrip())
    return r

def devices():
    r = subprocess.run(["fastboot", "devices"], capture_output=True, text=True)
    return [l.split()[0] for l in r.stdout.splitlines() if l.strip()]

def wait_for(present, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if bool(devices()) == present:
            return time.time() - t0
        time.sleep(0.3)
    sys.exit("timeout waiting for the watch to %s" % what)

def zone(blob, off, size):
    sig, start, used = struct.unpack_from("<III", blob, off)
    data = blob[off + 12:off + size]
    if sig != SIG or used > len(data) or start >= len(data):
        return None
    if used < len(data):
        return data[:used]
    return data[start:] + data[:start]

def symbolize(text):
    if not os.path.exists(ELF):
        return
    addrs = set(re.findall(r"\b(?:pc|lr)=([0-9a-f]{8})", text))
    for m in re.finditer(r"stack: ((?:[0-9a-f]{8} ?)+)", text):
        addrs.update(m.group(1).split())
    addrs = sorted(a for a in addrs if 0x8000 <= int(a, 16) < 0x400000)
    if not addrs:
        return
    cross = os.environ.get("CROSS", "arm-none-eabi-")
    r = subprocess.run([cross + "addr2line", "-e", ELF, "-f", "-C", "-i", *addrs],
                       capture_output=True, text=True)
    lines = r.stdout.splitlines()
    print("symbols:")
    for i, a in enumerate(addrs):
        fn = lines[2 * i] if 2 * i < len(lines) else "?"
        loc = lines[2 * i + 1] if 2 * i + 1 < len(lines) else "?"
        if fn != "??":
            print("  %s  %s  %s" % (a, fn, loc.replace(os.getcwd() + "/", "")))

def show(blob, tail):
    console = zone(blob, (REGION_SIZE - CONSOLE_SIZE) // RECORD_SIZE * RECORD_SIZE, CONSOLE_SIZE)
    record = zone(blob, 0, RECORD_SIZE)
    if console is None:
        print("no console zone")
    else:
        text = console.decode("utf-8", "replace")
        lines = text.splitlines()
        print("console (%d bytes, last %d lines):" % (len(console), min(tail, len(lines))))
        for l in lines[-tail:]:
            print("  " + l)
    if record:
        text = record.decode("utf-8", "replace")
        print("fault record:")
        for l in text.splitlines():
            print("  " + l)
        symbolize(text)
    elif console is not None:
        print("no fault record")
        symbolize(console.decode("utf-8", "replace")[-4000:])

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--no-boot", action="store_true", help="only fetch and show the log")
    p.add_argument("--timeout", type=float, default=90)
    p.add_argument("--tail", type=int, default=40)
    a = p.parse_args()
    if not a.no_boot:
        wait_for(True, 10, "appear in fastboot")
        t0 = time.time()
        fastboot("boot", IMG, quiet=True)
        print("booted in %.1f s, waiting for the reboot" % (time.time() - t0))
        wait_for(False, 20, "leave fastboot")
        dt = wait_for(True, a.timeout, "come back to fastboot")
        print("back in fastboot after %.1f s" % dt)
    if os.path.exists(RAW):
        os.unlink(RAW)
    fastboot("oem", "ramoops", "raw", quiet=True)
    fastboot("get_staged", RAW, quiet=True)
    blob = open(RAW, "rb").read()
    if len(blob) != REGION_SIZE:
        sys.exit("unexpected log region size %d" % len(blob))
    show(blob, a.tail)

if __name__ == "__main__":
    main()
