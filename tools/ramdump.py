#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyusb==1.3.1", "libusb-package==1.0.30.1"]
# ///
"""Read a Qualcomm watch's memory in crash dump mode (USB 05c6:900e), which
its boot chain enters after a reset while download mode is armed: print the
persistent log and fault record from it, check the image the bootloader
loaded against the build, or save whole regions. Only reads: it answers
the Sahara hello in memory debug mode and sends memory reads, never a reset.
The watch stays in that mode until a long press of the power key."""
import argparse, os, struct, sys

import libusb_package, usb.core, usb.util

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import run

VID, PID = 0x05c6, 0x900e
CHUNK = 0x1000
# Where the APQ8009W's DRAM starts and where the firmware's log is there.
DRAM = 0x80000000
LOG = 0x9ff80000

class Sahara:
    def __init__(self):
        dev = usb.core.find(idVendor=VID, idProduct=PID, backend=libusb_package.get_libusb1_backend())
        if dev is None:
            sys.exit("no watch in crash dump mode (USB %04x:%04x)" % (VID, PID))
        intf = dev.get_active_configuration()[(0, 0)]
        try:
            if dev.is_kernel_driver_active(0):
                dev.detach_kernel_driver(0)
        except (NotImplementedError, usb.core.USBError):
            pass
        usb.util.claim_interface(dev, 0)
        def ep(direction):
            return usb.util.find_descriptor(
                intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == direction)
        self.inp, self.out = ep(usb.util.ENDPOINT_IN), ep(usb.util.ENDPOINT_OUT)
        # An earlier run that stopped mid-read leaves the endpoints halted.
        for e in (self.inp, self.out):
            dev.clear_halt(e.bEndpointAddress)
        self.table = self.start()

    def read(self, n, ms):
        try:
            return bytes(self.inp.read(n, timeout=ms))
        except usb.core.USBTimeoutError:
            return b""

    def start(self):
        """The region table's address and length: from the device's memory
        debug command, after a hello if it sends one. A device that already
        sent it to an earlier run sends nothing, and the table is found
        again by its usual place in the command's answer of that run."""
        for _ in range(16):
            pkt = self.read(65536, 1000)
            if len(pkt) < 8:
                break
            cmd, length = struct.unpack_from("<II", pkt)
            if length != len(pkt) or cmd not in (1, 9):
                continue
            if cmd == 1:
                version, minimum = struct.unpack_from("<II", pkt, 8)
                self.out.write(struct.pack("<12I", 2, 0x30, version, minimum, 0, 2, 0, 0, 0, 0, 0, 0))
            elif cmd == 9:
                table = struct.unpack_from("<II", pkt, 8)
                self.save_table(table)
                return table
        table = self.load_table()
        if not table:
            sys.exit("the device sent no memory debug command: is it in crash dump mode?")
        return table

    TABLE_FILE = os.path.join(run.BUILD, "ramdump-table")

    def save_table(self, table):
        with open(self.TABLE_FILE, "w") as f:
            f.write("%x %x\n" % table)

    def load_table(self):
        try:
            return tuple(int(v, 16) for v in open(self.TABLE_FILE).read().split())
        except (OSError, ValueError):
            return None

    def mem(self, addr, length):
        out = bytearray()
        while len(out) < length:
            n = min(CHUNK, length - len(out))
            self.out.write(struct.pack("<4I", 0x0a, 16, addr + len(out), n))
            got = bytearray()
            while len(got) < n:
                b = self.read(n - len(got), 5000)
                if not b:
                    sys.exit("read stalled at %#x" % (addr + len(out) + len(got)))
                got += b
            # An answer of whole USB packets ends in a zero-length one, which
            # has to be read before the device takes the next request.
            if n % self.inp.wMaxPacketSize == 0:
                self.read(self.inp.wMaxPacketSize, 200)
            out += got
        return bytes(out)

    def regions(self):
        t = self.mem(*self.table)
        out = []
        for i in range(len(t) // 52):
            _, base, length, desc, name = struct.unpack_from("<III20s20s", t, i * 52)
            out.append((base, length, desc.split(b"\0")[0].decode(errors="replace").strip(),
                        name.split(b"\0")[0].decode(errors="replace")))
        return out

def check_image(s):
    """Lines of the loaded image that are not the build's: what the CPU
    found when it jumped there."""
    ref = open(os.path.join(run.BUILD, "asteroid.bin"), "rb").read()
    got = s.mem(DRAM + 0x8000, len(ref))
    bad = [i for i in range(0, len(ref), 64) if got[i:i + 64] != ref[i:i + 64]]
    if not bad:
        print("the image at %#x is the build's" % (DRAM + 0x8000))
        return
    print("%d of %d cache lines of the image at %#x differ from the build:" % (len(bad), len(ref) // 64, DRAM + 0x8000))
    pages = {}
    for i in bad:
        pages[i // 0x1000] = pages.get(i // 0x1000, 0) + 1
    for p, n in sorted(pages.items()):
        print("  page %#x: %d lines" % (DRAM + 0x8000 + p * 0x1000, n))

def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--log", type=lambda v: int(v, 16), default=LOG, help="address of the log region (hex)")
    p.add_argument("--tail", type=int, default=60, help="log lines to show")
    p.add_argument("--image", action="store_true", help="check the loaded image against build/asteroid.bin")
    p.add_argument("--save", metavar="DIR", help="save the regions named after it (all if none) into DIR")
    p.add_argument("names", nargs="*", help="regions to save, by file name (DDRCS0.BIN, OCIMEM.BIN, ...)")
    a = p.parse_args()
    s = Sahara()
    regions = s.regions()
    print("regions:")
    for base, length, desc, name in regions:
        print("  %08x %08x %-22s %s" % (base, length, desc, name))
    blob = s.mem(a.log, run.REGION_SIZE)
    open(run.RAW, "wb").write(blob)
    run.show(blob, a.tail)
    if a.image:
        check_image(s)
    if a.save:
        os.makedirs(a.save, exist_ok=True)
        for base, length, desc, name in regions:
            if a.names and name not in a.names:
                continue
            path = os.path.join(a.save, name)
            open(path, "wb").write(s.mem(base, length))
            print("saved %s (%d bytes)" % (path, length))

if __name__ == "__main__":
    main()
