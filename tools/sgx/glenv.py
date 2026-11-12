"""The Android-side environment the GLES stack expects, supplied by the harness:
a synthetic gralloc HAL module (hw_get_module), and the WSEGL window-system table
for the pbuffer-only case.  Every callback that is not expected to be reached
aborts with its name."""
import struct

from armproc import ProcError, PAGE

LIBHARDWARE = "/system/lib/libhardware.so"


class Arena:
    """Harness-owned guest memory for the synthetic structures."""

    def __init__(self, proc):
        self.proc = proc
        self.base = proc.mmap_alloc(0x10000)
        proc.map(self.base, 0x10000)
        self.next = self.base

    def alloc(self, size, align=16):
        self.next = (self.next + align - 1) & ~(align - 1)
        a = self.next
        self.next += size
        if self.next > self.base + 0x10000:
            raise ProcError("harness arena exhausted")
        return a

    def cstr(self, s):
        a = self.alloc(len(s) + 1, 1)
        self.proc.write(a, s.encode() + b"\0")
        return a


class Gralloc:
    """gralloc_module_t as the EGL services init uses it: the module with its
    methods->open, and the PowerVR extension slot at +0x10c that returns the
    transfer-queue context."""

    def __init__(self, proc, arena):
        self.proc, self.arena = proc, arena
        a = arena
        self.module = a.alloc(0x140)
        self.methods = a.alloc(8)
        self.device = a.alloc(0x80)
        mod = bytearray(0x140)
        struct.pack_into("<IHHIII", mod, 0, 0x48574D54, 0x0100, 0x0100, a.cstr("gralloc"),
                         a.cstr("Synthetic gralloc"), a.cstr("sgx harness"))
        struct.pack_into("<I", mod, 0x14, self.methods)
        for off in range(0x80, 0x140, 4):
            struct.pack_into("<I", mod, off, proc.make_stub("gralloc_module+0x%x" % off, self._unexpected("gralloc_module+0x%x" % off)))
        struct.pack_into("<I", mod, 0x10C, proc.make_stub("gralloc_module+0x10c(get_transfer_context)", self.get_transfer_context))
        proc.write(self.module, bytes(mod))
        proc.write(self.methods, struct.pack("<I", proc.make_stub("gralloc_methods.open", self.open)))
        dev = bytearray(0x80)
        for off in range(0x40, 0x80, 4):
            struct.pack_into("<I", dev, off, proc.make_stub("alloc_device+0x%x" % off, self._unexpected("alloc_device+0x%x" % off)))
        proc.write(self.device, bytes(dev))
        self.tq_ctx = a.alloc(0x100)

    def _unexpected(self, name):
        def f():
            raise ProcError("synthetic gralloc: unexpected call to %s from 0x%x" % (name, self.proc.reg(14)))
        return f

    def open(self):
        module, name, pdev = self.proc.arg(0), self.proc.cstr(self.proc.arg(1)), self.proc.arg(2)
        self.proc.log("   gralloc open(%r)" % name)
        if module != self.module or name != "gpu0":
            raise ProcError("gralloc open(%r)" % name)
        self.proc.w32(pdev, self.device)
        return 0

    def get_transfer_context(self):
        self.proc.w32(self.proc.arg(1), 0)
        self.proc.log("   gralloc transfer context -> NULL")
        return 0


def install_hardware(proc, arena):
    g = Gralloc(proc, arena)

    def hw_get_module():
        mid = proc.cstr(proc.arg(0))
        if mid != "gralloc":
            raise ProcError("hw_get_module(%r)" % mid)
        proc.w32(proc.arg(1), g.module)
        return 0
    proc.hook_entry(proc.sym(LIBHARDWARE, "hw_get_module") & ~1, "hw_get_module", hw_get_module)
    return g


WSEGL_SLOTS = {
    0x04: "IsDisplayValid", 0x08: "InitialiseDisplay", 0x0C: "CloseDisplay",
    0x10: "CreateWindowDrawable", 0x14: "CreatePixmapDrawable", 0x18: "DeleteDrawable",
    0x1C: "SwapDrawable", 0x20: "SwapControlInterval", 0x24: "WaitNative",
    0x28: "CopyFromDrawable", 0x2C: "CopyFromPBuffer", 0x30: "GetDrawableParameters",
    0x34: "ConnectDrawable", 0x38: "DisconnectDrawable", 0x3C: "FlagStartFrame",
}
WSEGL_LIB = "/system/lib/libpvrANDROID_WSEGL.so"


class Wsegl:
    """The window-system table for a process that only renders to pbuffers: display
    validation, display initialisation (capabilities and one configuration) and
    close are implemented, every drawable callback aborts."""

    def __init__(self, proc, arena, config_words):
        self.proc, self.arena = proc, arena
        self.disp = 0x57534547
        caps = struct.pack("<10I", 3, 1, 1, 0, 2, 5, 5, 1, 0, 0)       # same list libpvrANDROID_WSEGL.so returns
        self.caps = arena.alloc(len(caps))
        proc.write(self.caps, caps)
        cfg = struct.pack("<9I", *config_words) + b"\0" * 36
        self.configs = arena.alloc(len(cfg))
        proc.write(self.configs, cfg)
        self.table = arena.alloc(0x40)
        tab = bytearray(0x40)
        struct.pack_into("<I", tab, 0, 4)
        for off, name in WSEGL_SLOTS.items():
            fn = {0x04: self.is_valid, 0x08: self.init_display, 0x0C: self.close_display}.get(off) or self._unexpected(name)
            struct.pack_into("<I", tab, off, proc.make_stub("WSEGL." + name, fn))
        proc.write(self.table, bytes(tab))

    def _unexpected(self, name):
        def f():
            raise ProcError("WSEGL %s called (pbuffer-only synthetic window system) from 0x%x" % (name, self.proc.reg(14)))
        return f

    def is_valid(self):
        return 0

    def init_display(self):
        p = self.proc
        p.w32(p.arg(1), self.disp)
        p.w32(p.arg(2), self.caps)
        p.w32(p.arg(3), self.configs)
        return 0

    def close_display(self):
        return 0


def install_wsegl(proc, arena, config_words):
    w = Wsegl(proc, arena, config_words)
    proc.hook_entry(proc.sym(WSEGL_LIB, "WSEGL_GetFunctionTablePointer") & ~1, "WSEGL_GetFunctionTablePointer",
                    lambda: w.table)
    return w
