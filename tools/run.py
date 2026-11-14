#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Boot build/asteroid.img over fastboot (after an adb reboot if the previous
image is running) and show its log over adb. If the image dies and the watch
returns to fastboot, fetch the persistent log region instead and print the
console tail, the fault record and the symbolized addresses in it. A bootloader
without that command gets a second boot, whose log has the previous record.
--soc picks another SoC's image. A watch whose installed Linux keeps its own
log in the same region shows the firmware's there after a reset: --pstore
reads it from that Linux over adb."""
import argparse, os, re, struct, subprocess, sys, time

BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")
IMG = os.path.join(BUILD, "asteroid.img")
ELF = os.path.join(BUILD, "asteroid.elf")
RAW = os.path.join(BUILD, "ramoops.bin")
# --serial / --adb-serial: a watch in fastboot and the firmware's adb can
# have different serials (smelt: M987BW25ZV and asteroid-smelt).
SERIAL = None
ADB_SERIAL = None

def fb_cmd(*args):
    return ["fastboot", *(["-s", SERIAL] if SERIAL else []), *args]

def adb_cmd(*args):
    return ["adb", *(["-s", ADB_SERIAL] if ADB_SERIAL else []), *args]
SIG = 0x43474244
RECORD_SIZE = 0x2000
CONSOLE_SIZE = 0x40000
REGION_SIZE = 0x80000

def fastboot(*args, check=True, quiet=False):
    r = subprocess.run(fb_cmd(*args), capture_output=True, text=True)
    if check and r.returncode:
        sys.exit("fastboot %s failed:\n%s%s" % (" ".join(args), r.stdout, r.stderr))
    if not quiet and (r.stdout.strip() or r.stderr.strip()):
        print((r.stdout + r.stderr).rstrip())
    return r

def devices():
    r = subprocess.run(["fastboot", "devices"], capture_output=True, text=True)
    found = [l.split()[0] for l in r.stdout.splitlines() if l.strip()]
    return [d for d in found if d == SERIAL] if SERIAL else found

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
    addrs = sorted(a for a in addrs if 0x8000 <= int(a, 16) < 0x01e00000 or 0x88000000 <= int(a, 16) < 0x8a000000)
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
    stage = zone(blob, (REGION_SIZE - CONSOLE_SIZE) // RECORD_SIZE * RECORD_SIZE - RECORD_SIZE, RECORD_SIZE)
    if stage and stage != b"up  ":
        print("the boot stopped at stage %s" % stage.decode("ascii", "replace").strip())
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

def adb_online():
    r = subprocess.run(["adb", "devices"], capture_output=True, text=True)
    return any(l.endswith("\tdevice") and (not ADB_SERIAL or l.split()[0] == ADB_SERIAL)
               for l in r.stdout.splitlines()[1:])

def show_log(tail):
    """Print the firmware's log tail over adb and symbolize the previous
    boot's fault record in it, if any."""
    r = subprocess.run(adb_cmd("shell", "log %d" % tail), capture_output=True, text=True, timeout=60)
    print(r.stdout.rstrip())
    prev = re.findall(r"previous boot ended with:\n(.*?)\n\n", r.stdout, re.S)
    if prev:
        symbolize(prev[-1])

def fetch_ramoops(tail):
    """False if this bootloader has no such command: the stock one on sparrow."""
    if os.path.exists(RAW):
        os.unlink(RAW)
    if fastboot("oem", "ramoops", "raw", check=False, quiet=True).returncode:
        return False
    fastboot("get_staged", RAW, quiet=True)
    blob = open(RAW, "rb").read()
    if len(blob) != REGION_SIZE:
        sys.exit("unexpected log region size %d" % len(blob))
    show(blob, tail)
    return True

def pstore(tail):
    """The log and fault record a Linux on the watch found in its ramoops
    region when it started."""
    sh = "mount | grep -q ' /sys/fs/pstore ' || mount -t pstore pstore /sys/fs/pstore; cat /sys/fs/pstore/%s 2>/dev/null"
    def read(name):
        return subprocess.run(adb_cmd("shell", sh % name), capture_output=True, timeout=60).stdout
    console = read("console-ramoops*")
    # The fault record is "annotate" on minnow's kernel and the first dump
    # record on catfish's, where the last one holds the boot stage.
    record = read("annotate-ramoops*") + read("dmesg-ramoops-0")
    stage = read("dmesg-ramoops-31").strip(b"\0").decode("ascii", "replace").strip()
    if stage and stage != "up":
        print("the boot stopped at stage %s" % stage)
    if not console and not record:
        sys.exit("nothing in /sys/fs/pstore: the region did not survive, or this boot was not the first after the reset")
    lines = console.decode("utf-8", "replace").splitlines()
    print("console (%d bytes, last %d lines):" % (len(console), min(tail, len(lines))))
    for l in lines[-tail:]:
        print("  " + l)
    if record.strip(b"\0").strip():
        text = record.decode("utf-8", "replace")
        print("fault record:")
        for l in text.splitlines():
            print("  " + l)
        symbolize(text)
    else:
        print("no fault record")
        symbolize(console.decode("utf-8", "replace")[-4000:])

def boot():
    t0 = time.time()
    fastboot("boot", IMG, quiet=True)
    print("booted in %.1f s" % (time.time() - t0))

def lk2nd():
    # A reboot to the bootloader can land in the stock one, which hangs on
    # fastboot boot: go on from there to lk2nd.
    r = subprocess.run(fb_cmd("getvar", "version-bootloader"), capture_output=True, text=True, timeout=10)
    if "STURGEON" in r.stdout + r.stderr:
        fastboot("continue", quiet=True)
        t0 = time.time()
        while time.time() - t0 < 30:
            time.sleep(0.5)
            if devices():
                r = subprocess.run(fb_cmd("getvar", "version-bootloader"), capture_output=True, text=True, timeout=10)
                if "STURGEON" not in r.stdout + r.stderr:
                    return
        sys.exit("timeout waiting for lk2nd")

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--no-boot", action="store_true", help="only fetch the log region from fastboot")
    p.add_argument("--no-wait", action="store_true", help="boot and return")
    p.add_argument("--attach", action="store_true",
                   help="no fastboot: show the running firmware's log over adb and symbolize its record")
    p.add_argument("--serial", "-s", help="fastboot serial (and adb serial unless --adb-serial)")
    p.add_argument("--adb-serial", help="the firmware's adb serial (asteroid-<board>)")
    p.add_argument("--soc", help="the image of this SoC's build directory (omap3)")
    p.add_argument("--pstore", action="store_true",
                   help="no fastboot: read the firmware's log from the Linux now running on the watch")
    p.add_argument("--timeout", type=float, default=20)
    p.add_argument("--tail", type=int, help="log lines to show (40, or 300 with --attach)")
    a = p.parse_args()
    global SERIAL, ADB_SERIAL, IMG, ELF
    SERIAL = a.serial
    ADB_SERIAL = a.adb_serial or a.serial
    if a.soc:
        IMG = os.path.join(BUILD, a.soc, "asteroid.img")
        ELF = os.path.join(BUILD, a.soc, "asteroid.elf")
    if a.tail is None:
        a.tail = 300 if a.attach or a.pstore else 40
    if a.pstore:
        if not adb_online():
            sys.exit("no adb device%s" % (" " + ADB_SERIAL if ADB_SERIAL else ""))
        pstore(a.tail)
        return
    if a.attach:
        if not adb_online():
            sys.exit("no adb device%s" % (" " + ADB_SERIAL if ADB_SERIAL else ""))
        show_log(a.tail)
        return
    if a.no_boot:
        if not fetch_ramoops(a.tail):
            sys.exit("no oem ramoops here: boot again and read the previous boot's record")
        return
    if adb_online():
        subprocess.run(adb_cmd("reboot", "bootloader"), capture_output=True, timeout=20)
        wait_for(True, 20, "come back to fastboot")
    else:
        wait_for(True, 10, "appear in fastboot")
    lk2nd()
    boot()
    if a.no_wait:
        return
    wait_for(False, 20, "leave fastboot")
    t0 = time.time()
    retried = False
    while time.time() - t0 < a.timeout:
        if adb_online():
            print("adb online after %.1f s" % (time.time() - t0))
            show_log(a.tail)
            return
        if devices():
            print("back in fastboot after %.1f s without adb: the image died" % (time.time() - t0))
            if fetch_ramoops(a.tail) or retried:
                return
            print("no oem ramoops: booting again for the previous boot's record")
            retried = True
            boot()
            wait_for(False, 20, "leave fastboot")
            t0 = time.time()
            continue
        time.sleep(0.3)
    sys.exit("running for %.0f s but no adb device: USB did not come up" % a.timeout)

if __name__ == "__main__":
    main()
