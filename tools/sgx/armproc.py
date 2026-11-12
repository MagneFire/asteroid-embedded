"""An ARM Linux user-space process under Unicorn, for running unmodified
Android 6 bionic libraries.

The process is started the way the kernel starts it: the dump's own
/system/bin/linker is mapped, together with a small synthetic executable that
only names the libraries to load; the initial stack carries argv, envp and
auxv.  The linker then maps libc.so and the rest by itself, through the
system calls emulated here (a virtual file system rooted at the dump's
system/ directory, anonymous and file mappings, TLS through the CP15 user
read-only thread register, single-threaded futex).  Every system call is
logged; one that is not implemented aborts with its number and call site.
"""
import os
import struct

from elftools.elf.elffile import ELFFile
from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_ALL,
                     UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_UNMAPPED,
                     UC_CTL_CPU_MODEL, UC_CTL_UC_USE_EXITS)
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
                               UC_ARM_REG_R7, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC, UC_ARM_REG_CPSR,
                               UC_ARM_REG_C13_C0_3, UC_ARM_REG_C1_C0_2,
                               UC_ARM_REG_FPEXC, UC_ARM_REG_FPSCR, UC_ARM_REG_D0,
                               UC_CPU_ARM_CORTEX_A8)

PAGE = 4096
SENTINEL = 0xFFFF0F00        # return address of calls made by the harness
KUSER = 0xFFFF0000
STUB_BASE = 0x00200000       # python-implemented functions
STUB_SIZE = 0x10000

EPERM, ENOENT, EBADF, EAGAIN, ENOMEM, EACCES, EFAULT, EINVAL = 1, 2, 9, 11, 12, 13, 14, 22
ENOTTY, ENOSYS, ETIMEDOUT, ERANGE = 25, 38, 110, 34

PROT_NONE, MAP_FIXED, MAP_ANONYMOUS = 0, 0x10, 0x20


def round_up(v, a):
    return (v + a - 1) & ~(a - 1)


class ProcError(Exception):
    pass


class Layout:
    """Guest addresses.  `shift` moves everything the harness chooses, to find
    recorded bytes that depend on them."""

    def __init__(self, shift=0):
        self.exe_base = 0x00400000
        self.brk_base = 0x00800000 + shift
        self.mmap_top = 0xB6F00000 - shift
        self.stack_top = 0xBEFF0000 - shift
        self.stack_size = 0x00200000
        self.fake_time = 1_000_000_000 + shift      # seconds-scale epoch offset irrelevant
        self.pid = 1000


class NeonFix:
    """Unicorn 2.1.4 on Apple silicon crashes the host process when it executes the
    64-bit-element D-register forms of VSHL (register), VQADD and VQSUB.  Every
    occurrence in executable file mappings is found by pattern and executed in
    Python instead (same results, including FPSCR.QC), through a code hook on
    that one address.  No guest instruction is altered."""

    def __init__(self, proc):
        self.proc = proc
        self.count = 0
        self.hooked = set()

    @staticmethod
    def decode(u, lo):
        # lo: bits 15:0 of the second halfword (Thumb) / low half of the word (ARM)
        q = lo >> 6 & 1
        op = lo >> 8 & 15
        b4 = lo >> 4 & 1
        if q:
            return None
        if op == 4 and b4 == 0:
            return "vshl"
        if op == 0 and b4 == 1:
            return "vqadd"
        if op == 2 and b4 == 1:
            return "vqsub"
        return None

    def scan(self, data, base):
        n = 0
        # Thumb-2 (halfword aligned): hw1 = 111U 1111 0Dss nnnn with ss == 3
        for off in range(0, len(data) - 3, 2):
            hw1, hw2 = struct.unpack_from("<HH", data, off)
            if hw1 & 0xEF80 == 0xEF00 and (hw1 >> 4 & 3) == 3:
                kind = self.decode(hw1 >> 12 & 1, hw2)
                if kind:
                    self.install(base + off, True, hw1 >> 12 & 1, hw1, hw2, kind)
                    n += 1
        # ARM (word aligned): 1111 001U 0Dss nnnn dddd oooo NQMb mmmm
        for off in range(0, len(data) - 3, 4):
            w, = struct.unpack_from("<I", data, off)
            if w & 0xFE800000 == 0xF2000000 and (w >> 20 & 3) == 3:
                kind = self.decode(w >> 24 & 1, w & 0xFFFF)
                if kind:
                    self.install(base + off, False, w >> 24 & 1, w >> 16, w & 0xFFFF, kind)
                    n += 1
        return n

    def install(self, addr, thumb, u, hi, lo, kind):
        if addr in self.hooked:
            return
        self.hooked.add(addr)
        d = (hi >> 6 & 1) << 4 | (lo >> 12 & 15)
        n = (lo >> 7 & 1) << 4 | (hi & 15)
        m = (lo >> 5 & 1) << 4 | (lo & 15)
        length = 4
        self.count += 1
        self.proc.uc.hook_add(UC_HOOK_CODE, self.run, begin=addr, end=addr,
                              user_data=(addr, thumb, u, d, n, m, kind, length))

    def run(self, uc, address, size, ud):
        addr, thumb, u, d, n, m, kind, length = ud
        if self.proc.err:
            return
        M = (1 << 64) - 1
        rd = lambda r: uc.reg_read(UC_ARM_REG_D0 + r)
        sgn = lambda v: v - (1 << 64) if v >> 63 else v
        if kind == "vshl":
            val, sh = rd(m), rd(n) & 0xFF
            sh = sh - 256 if sh >= 128 else sh
            if sh >= 64:
                r = 0
            elif sh >= 0:
                r = val << sh & M
            elif u:
                r = 0 if -sh >= 64 else val >> -sh
            else:
                r = (sgn(val) >> min(-sh, 63)) & M
        else:
            a, b = rd(n), rd(m)
            sat = False
            if u:
                r = a + b if kind == "vqadd" else a - b
                if r > M:
                    r, sat = M, True
                elif r < 0:
                    r, sat = 0, True
            else:
                r = sgn(a) + sgn(b) if kind == "vqadd" else sgn(a) - sgn(b)
                if r > (1 << 63) - 1:
                    r, sat = (1 << 63) - 1, True
                elif r < -(1 << 63):
                    r, sat = -(1 << 63), True
                r &= M
            if sat:
                uc.reg_write(UC_ARM_REG_FPSCR, uc.reg_read(UC_ARM_REG_FPSCR) | 0x08000000)
        uc.reg_write(UC_ARM_REG_D0 + d, r)
        uc.reg_write(UC_ARM_REG_PC, (address + length) | (1 if thumb else 0))


class VFile:
    def __init__(self, path, data=None, device=None):
        self.path, self.data, self.device, self.pos = path, data, device, 0


class Proc:
    def __init__(self, sysroot, layout=None, logfn=None):
        self.sysroot = sysroot            # directory holding the dump's /system
        self.lay = layout or Layout()
        self.calls = []
        self.files = {}
        self.next_fd = 3
        self.devices = {}                 # path -> object with open/ioctl/mmap/close
        self.err = None
        self.brk = self.lay.brk_base
        self.brk_start = self.brk
        self.mmap_cursor = self.lay.mmap_top
        self.stub_names = {}
        self.stub_fns = {}
        self.stub_next = STUB_BASE
        self.hooks_at = {}                # address -> python callable run before the instruction
        self.time_ns = 0
        self.log_sink = []
        self.loaded = {}                  # path -> (base, ELFFile-lite info)
        self.mmaps = []                   # (start, length, path, offset) file mappings, in order
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A8)
        self.extra_syscalls = {}
        self.neonfix = NeonFix(self)
        self.stop_reason = None
        self.logfn = logfn
        uc = self.uc
        uc.hook_add(UC_HOOK_INTR, self._on_intr)
        uc.hook_add(UC_HOOK_CODE, self._on_code, begin=STUB_BASE, end=STUB_BASE + STUB_SIZE)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        uc.mem_map(STUB_BASE, STUB_SIZE, UC_PROT_ALL)
        self._setup_kuser()
        self._enable_fp()

    # ------------------------------------------------------------------ cpu
    def _enable_fp(self):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_C1_C0_2, 0x00F00000)
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)

    def _setup_kuser(self):
        """The kernel's user-helper page: __kuser_get_tls, cmpxchg, memory barrier."""
        uc = self.uc
        uc.mem_map(KUSER, PAGE, UC_PROT_ALL)
        code = {
            0xFFFF0FA0: b"\x5f\xf0\x7f\xf5\x0e\xf0\xa0\xe1",              # dmb ish; mov pc, lr
            0xFFFF0FC0: bytes.fromhex(                                    # cmpxchg
                "00c090e5" "00005ce1" "0100a0e3" "0000e0e3" "0e00a0e1"),  # placeholder, replaced below
            0xFFFF0FE0: b"\x70\x0f\x1d\xee\x0e\xf0\xa0\xe1",              # mrc p15,0,r0,c13,c0,3; mov pc, lr
        }
        # __kuser_cmpxchg: if (*r2 == r0) { *r2 = r1; return 0 } else return nonzero
        cmpxchg = [0xE5923000,   # ldr r3,[r2]
                   0xE0530000,   # subs r0,r3,r0
                   0x05821000,   # streq r1,[r2]
                   0xE12FFF1E]   # bx lr
        code[0xFFFF0FC0] = b"".join(struct.pack("<I", w) for w in cmpxchg)
        for a, b in code.items():
            uc.mem_write(a, b)
        uc.mem_write(0xFFFF0FFC, struct.pack("<I", 3))        # __kuser_helper_version
        uc.mem_write(SENTINEL, struct.pack("<I", 0xE12FFF1E))

    def set_tls(self, v):
        self.uc.reg_write(UC_ARM_REG_C13_C0_3, v & 0xFFFFFFFF)

    # --------------------------------------------------------------- memory
    def reg(self, r):
        return self.uc.reg_read(r)

    def u32(self, a):
        return struct.unpack("<I", self.uc.mem_read(a, 4))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def read(self, a, n):
        return bytes(self.uc.mem_read(a, n))

    def write(self, a, data):
        self.uc.mem_write(a, bytes(data))

    def cstr(self, a, limit=4096):
        out = bytearray()
        while len(out) < limit:
            c = bytes(self.uc.mem_read(a + len(out), 1))
            if c == b"\0":
                break
            out += c
        return out.decode("latin-1")

    def _regions(self):
        return [(b, e + 1) for b, e, _ in self.uc.mem_regions()]

    def unmap(self, start, end):
        for b, e in self._regions():
            lo, hi = max(b, start), min(e, end)
            if lo < hi:
                self.uc.mem_unmap(lo, hi - lo)

    def map(self, start, length):
        end = start + length
        self.unmap(start, end)
        self.uc.mem_map(start, length, UC_PROT_ALL)

    def mmap_alloc(self, length):
        length = round_up(length, PAGE)
        self.mmap_cursor -= length
        if self.mmap_cursor < 0x40000000:
            raise ProcError("guest mmap area exhausted")
        return self.mmap_cursor

    # ----------------------------------------------------------- filesystem
    def host_path(self, gpath):
        if gpath.startswith("/system/"):
            return os.path.join(self.sysroot, gpath[len("/system/"):])
        if gpath.startswith("/vendor/"):
            return os.path.join(self.sysroot, "vendor", gpath[len("/vendor/"):])
        return None

    def add_device(self, path, obj):
        self.devices[path] = obj

    def new_fd(self, vf):
        fd = self.next_fd
        self.next_fd += 1
        self.files[fd] = vf
        return fd

    # ------------------------------------------------------- python stubs
    def make_stub(self, name, fn):
        """Return the guest address of a function implemented in Python.  fn() reads
        arguments with self.arg(n) and returns r0 (or (r0, r1))."""
        a = self.stub_next
        self.stub_next += 8
        self.stub_names[a] = name
        self.stub_fns[a] = fn
        self.uc.mem_write(a, struct.pack("<I", 0xE12FFF1E))     # bx lr
        return a

    def arg(self, n):
        if n < 4:
            return self.reg((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)[n])
        return self.u32(self.reg(UC_ARM_REG_SP) + 4 * (n - 4))

    def _on_code(self, uc, address, size, ud):
        if self.err:
            return
        fn = self.stub_fns.get(address)
        if fn is None:
            return
        try:
            r = fn()
            r1 = None
            if isinstance(r, tuple):
                r, r1 = r
            if r is None:
                r = 0
            self.log("import %s(0x%x, 0x%x, 0x%x, 0x%x) = 0x%x  [caller 0x%x]"
                     % (self.stub_names[address], self.arg(0), self.arg(1), self.arg(2), self.arg(3),
                        r & 0xFFFFFFFF, self.reg(UC_ARM_REG_LR) & ~1))
            uc.reg_write(UC_ARM_REG_R0, r & 0xFFFFFFFF)
            if r1 is not None:
                uc.reg_write(UC_ARM_REG_R1, r1 & 0xFFFFFFFF)
        except Exception as e:
            self.err = e
            uc.emu_stop()

    def hook_entry(self, addr, name, fn):
        """Run fn() instead of the function at addr: its result becomes r0 and
        execution continues at the caller (this is an interception of the function,
        recorded as such in calls.txt)."""
        stub = self.make_stub(name, fn)
        self.hooks_at[addr & ~1] = stub
        self._install_entry_hook()
        self._rebuild_entry_hooks()

    def _install_entry_hook(self):
        if getattr(self, "_entry_hook", None):
            return

        def on_entry(uc, address, size, ud):
            if self.err:
                return
            stub = self.hooks_at.get(address)
            if stub is None:
                return
            uc.reg_write(UC_ARM_REG_PC, stub)
        # hooks on arbitrary code ranges slow the CPU, so only the entry points are covered
        self._entry_hook = on_entry
        self._entry_hook_ids = {}

    def _rebuild_entry_hooks(self):
        for a in self.hooks_at:
            if a not in self._entry_hook_ids:
                self._entry_hook_ids[a] = self.uc.hook_add(UC_HOOK_CODE, self._entry_hook, begin=a, end=a + 1)

    # -------------------------------------------------------------- logging
    def log(self, text):
        self.calls.append("%05d %s" % (len(self.calls), text))
        if self.logfn:
            self.logfn(text)

    def _on_unmapped(self, uc, access, address, size, value, ud):
        pc = uc.reg_read(UC_ARM_REG_PC)
        self.err = ProcError("unmapped memory access (type %d) at 0x%08x size %d, pc=0x%08x lr=0x%08x"
                             % (access, address, size, pc, uc.reg_read(UC_ARM_REG_LR)))
        return False

    # ---------------------------------------------------------- syscalls
    def _on_intr(self, uc, intno, ud):
        if self.err:
            return
        if intno != 2:
            self.err = ProcError("unexpected exception %d at pc=0x%x" % (intno, self.reg(UC_ARM_REG_PC)))
            uc.emu_stop()
            return
        nr = self.reg(UC_ARM_REG_R7)
        a = [self.reg(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                                   UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5)]
        pc = self.reg(UC_ARM_REG_PC)
        if nr == 0x900001:
            self.stop_reason = "ready"
            uc.emu_stop()
            return
        try:
            fn = self.extra_syscalls.get(nr) or getattr(self, "sys_%d" % nr, None)
            if fn is None:
                raise ProcError("unimplemented system call %d (args %s) at pc=0x%x lr=0x%x"
                                % (nr, ["0x%x" % x for x in a], pc, self.reg(UC_ARM_REG_LR)))
            r, text = fn(*a)
            name = fn.__name__[4:] if fn.__name__.startswith("sys_") else fn.__name__
            self.log("syscall %s(%s) = %s  [pc 0x%x lr 0x%x]"
                     % (name, text, r if -4096 < r < 4096 and r >= -4095 else "0x%x" % (r & 0xFFFFFFFF),
                        pc, self.reg(UC_ARM_REG_LR)))
            uc.reg_write(UC_ARM_REG_R0, r & 0xFFFFFFFF)
        except Exception as e:
            self.err = e
            uc.emu_stop()

    def _fake_time(self, advance=50_000):
        self.time_ns += advance
        return self.lay.fake_time * 1_000_000_000 + self.time_ns

    # --- individual system calls: each returns (result, argument text)
    def sys_1(self, code, *_):                       # exit
        raise ProcError("exit(%d) called" % code)

    sys_248 = sys_1                                  # exit_group

    def sys_3(self, fd, buf, n, *_):                 # read
        vf = self.files.get(fd)
        if vf is None:
            return -EBADF, "fd=%d" % fd
        if vf.device is not None:
            raise ProcError("read on device %s" % vf.path)
        chunk = vf.data[vf.pos:vf.pos + n]
        self.write(buf, chunk)
        vf.pos += len(chunk)
        return len(chunk), "fd=%d (%s) n=%d" % (fd, vf.path, n)

    def sys_180(self, fd, buf, n, pad, lo, hi):      # pread64
        vf = self.files.get(fd)
        if vf is None or vf.device is not None:
            return -EBADF, "fd=%d" % fd
        off = hi << 32 | lo
        chunk = vf.data[off:off + n]
        self.write(buf, chunk)
        return len(chunk), "fd=%d (%s) n=%d off=0x%x" % (fd, vf.path, n, off)

    def sys_4(self, fd, buf, n, *_):                 # write
        data = self.read(buf, n)
        if fd in (1, 2) or (fd in self.files and self.files[fd].path.startswith("/dev/")):
            self.log_sink.append((fd, data))
            return n, "fd=%d %r" % (fd, data[:80])
        raise ProcError("write to fd %d" % fd)

    def _open(self, path, flags):
        if path in self.devices:
            fd = self.new_fd(VFile(path, device=self.devices[path]))
            self.devices[path].open(self, fd)
            return fd
        if path in ("/dev/stderr", "/dev/stdout", "/dev/log/main", "/dev/log/system", "/dev/log/radio",
                    "/dev/log/events"):
            return self.new_fd(VFile(path, b""))
        if path.startswith("/dev/"):
            return -ENOENT
        hp = self.host_path(path)
        if hp is None or not os.path.isfile(hp):
            return -ENOENT
        with open(hp, "rb") as f:
            data = f.read()
        return self.new_fd(VFile(path, data))

    def sys_5(self, p, flags, mode, *_):             # open
        path = self.cstr(p)
        return self._open(path, flags), "%r flags=0x%x" % (path, flags)

    def sys_322(self, dirfd, p, flags, mode, *_):    # openat
        path = self.cstr(p)
        if not path.startswith("/"):
            return -ENOENT, "%r (relative to the working directory /)" % path
        return self._open(path, flags), "%r flags=0x%x" % (path, flags)

    def sys_6(self, fd, *_):                         # close
        vf = self.files.pop(fd, None)
        if vf is None:
            return -EBADF, "fd=%d" % fd
        if vf.device is not None:
            vf.device.close(self, fd)
        return 0, "fd=%d (%s)" % (fd, vf.path)

    def sys_19(self, fd, off, whence, *_):           # lseek
        vf = self.files.get(fd)
        if vf is None or vf.device is not None:
            return -EBADF, "fd=%d" % fd
        vf.pos = {0: off, 1: vf.pos + off, 2: len(vf.data) + off}[whence]
        return vf.pos, "fd=%d off=%d whence=%d" % (fd, off, whence)

    def sys_20(self, *_):                            # getpid
        return self.lay.pid, ""

    def sys_224(self, *_):                           # gettid
        return self.lay.pid, ""

    def sys_199(self, *_):                           # getuid32
        return 1000, ""

    sys_200 = sys_201 = sys_202 = sys_199           # getgid32, geteuid32, getegid32

    def sys_45(self, addr, *_):                      # brk
        if addr == 0:
            return self.brk, "0"
        if addr < self.brk_start:
            return self.brk, "0x%x (refused)" % addr
        old_top = round_up(self.brk, PAGE)
        new_top = round_up(addr, PAGE)
        if new_top > old_top:
            self.map(old_top, new_top - old_top)
        elif new_top < old_top:
            self.unmap(new_top, old_top)
        self.brk = addr
        return addr, "0x%x" % addr

    def _file_pages(self, start, length, vf, offset):
        data = vf.data[offset:offset + length]
        if data:
            self.write(start, data)

    def sys_192(self, addr, length, prot, flags, fd, pgoff):    # mmap2
        offset = pgoff * 4096
        length_r = round_up(length, PAGE)
        if length == 0:
            return -EINVAL, "len=0"
        vf = None
        if not flags & MAP_ANONYMOUS:
            vf = self.files.get(fd & 0xFFFFFFFF if fd < 0x80000000 else fd)
            if vf is None:
                return -EBADF, "fd=%d" % fd
        if flags & MAP_FIXED:
            start = addr
            if start & (PAGE - 1):
                return -EINVAL, "unaligned fixed"
        else:
            start = self.mmap_alloc(length_r)
        text = "addr=0x%x len=0x%x prot=%d flags=0x%x fd=%d off=0x%x" % (addr, length, prot, flags, fd if vf is None else fd, offset)
        if vf is not None and vf.device is not None:
            r = vf.device.mmap(self, start, length_r, prot, flags, offset, bool(flags & MAP_FIXED))
            return r, text + " (%s)" % vf.path
        self.map(start, length_r)
        if vf is not None:
            self._file_pages(start, length, vf, offset)
            self.mmaps.append((start, length_r, vf.path, offset, prot, flags))
            if prot & 4:
                self.neonfix.scan(vf.data[offset:offset + length], start)
            text += " (%s)" % vf.path
        return start, text

    def sys_91(self, addr, length, *_):              # munmap
        self.unmap(addr, round_up(addr + length, PAGE))
        return 0, "addr=0x%x len=0x%x" % (addr, length)

    def sys_125(self, addr, length, prot, *_):       # mprotect
        return 0, "addr=0x%x len=0x%x prot=%d" % (addr, length, prot)

    def sys_220(self, addr, length, advice, *_):     # madvise
        return 0, "addr=0x%x len=0x%x advice=%d" % (addr, length, advice)

    def sys_983045(self, tp, *_):                    # ARM set_tls
        self.set_tls(tp)
        return 0, "0x%x" % tp

    def sys_983042(self, *_):                        # ARM cacheflush
        return 0, ""

    def sys_172(self, option, a2, a3, a4, a5, *_):   # prctl
        if option == 0x53564d41:                     # PR_SET_VMA
            return 0, "PR_SET_VMA"
        return -EINVAL, "option=0x%x" % option

    def sys_175(self, how, setp, oldp, sz, *_):      # rt_sigprocmask
        if oldp:
            self.write(oldp, b"\0" * min(sz, 8))
        return 0, "how=%d" % how

    def sys_174(self, sig, act, oact, sz, *_):       # rt_sigaction
        if oact:
            self.write(oact, b"\0" * 20)
        return 0, "sig=%d" % sig

    def sys_67(self, sig, act, oact, *_):             # sigaction (old)
        if oact:
            self.write(oact, b"\0" * 16)
        return 0, "sig=%d" % sig

    def sys_186(self, ss, oss, *_):                  # sigaltstack
        return 0, ""

    def sys_263(self, clk, ts, *_):                  # clock_gettime
        t = self._fake_time()
        self.write(ts, struct.pack("<II", t // 1_000_000_000, t % 1_000_000_000))
        return 0, "clk=%d" % clk

    def sys_264(self, clk, ts, *_):                  # clock_getres
        if ts:
            self.write(ts, struct.pack("<II", 0, 1))
        return 0, "clk=%d" % clk

    def sys_78(self, tv, tz, *_):                    # gettimeofday
        t = self._fake_time()
        if tv:
            self.write(tv, struct.pack("<II", t // 1_000_000_000, (t % 1_000_000_000) // 1000))
        return 0, ""

    def sys_162(self, req, rem, *_):                 # nanosleep
        s, ns = struct.unpack("<II", self.read(req, 8))
        self._fake_time(s * 1_000_000_000 + ns)
        return 0, "%d.%09d" % (s, ns)

    def sys_240(self, uaddr, op, val, timeout, uaddr2, val3):    # futex
        cmd = op & 0x7F
        if cmd == 1:                                  # FUTEX_WAKE
            return 0, "WAKE 0x%x" % uaddr
        if cmd == 0 or cmd == 9:                      # FUTEX_WAIT
            if self.u32(uaddr) != val:
                return -EAGAIN, "WAIT 0x%x (value changed)" % uaddr
            if timeout:
                s, ns = struct.unpack("<II", self.read(timeout, 8))
                self._fake_time(s * 1_000_000_000 + ns)
                return -ETIMEDOUT, "WAIT 0x%x timeout" % uaddr
            raise ProcError("futex wait without timeout on 0x%x would block forever (single thread)" % uaddr)
        raise ProcError("futex op %d" % op)

    def sys_136(self, persona, *_):                   # personality
        return 0, "0x%x" % persona

    def sys_158(self, *_):                            # sched_yield
        return 0, ""

    @staticmethod
    def _ino(path):
        import zlib
        return zlib.crc32(path.encode()) & 0x7FFFFFFF | 1

    def _stat_buf(self, buf, size, mode, rdev=0, path="/"):
        st = bytearray(104)
        struct.pack_into("<Q", st, 0, 1)                         # st_dev
        struct.pack_into("<I", st, 12, self._ino(path))          # __st_ino
        struct.pack_into("<Q", st, 96, self._ino(path))          # st_ino
        struct.pack_into("<I", st, 16, mode)
        struct.pack_into("<I", st, 20, 1)                        # nlink
        struct.pack_into("<Q", st, 32, rdev)
        struct.pack_into("<q", st, 48, size)
        struct.pack_into("<I", st, 56, 4096)                     # blksize
        struct.pack_into("<q", st, 64, (size + 511) // 512)
        self.write(buf, bytes(st))

    def sys_197(self, fd, buf, *_):                  # fstat64
        vf = self.files.get(fd)
        if vf is None:
            return -EBADF, "fd=%d" % fd
        if vf.device is not None:
            self._stat_buf(buf, 0, 0o20666, 0x1234, vf.path)
        else:
            self._stat_buf(buf, len(vf.data), 0o100644, 0, vf.path)
        return 0, "fd=%d (%s)" % (fd, vf.path)

    def sys_195(self, p, buf, *_):                   # stat64
        path = self.cstr(p)
        hp = self.host_path(path)
        if path in self.devices:
            self._stat_buf(buf, 0, 0o20666, 0x1234, path)
            return 0, repr(path)
        if hp and os.path.isfile(hp):
            self._stat_buf(buf, os.path.getsize(hp), 0o100644, 0, path)
            return 0, repr(path)
        if hp and os.path.isdir(hp):
            self._stat_buf(buf, 4096, 0o40755, 0, path)
            return 0, repr(path)
        return -ENOENT, repr(path)

    sys_196 = sys_195                                 # lstat64

    def sys_33(self, p, mode, *_):                    # access
        path = self.cstr(p)
        hp = self.host_path(path)
        ok = path in self.devices or (hp and os.path.exists(hp))
        return (0 if ok else -ENOENT), repr(path)

    def sys_334(self, dirfd, p, mode, *_):            # faccessat
        return self.sys_33(p, mode)

    def sys_54(self, fd, cmd, arg, *_):               # ioctl
        vf = self.files.get(fd)
        if vf is None:
            return -EBADF, "fd=%d" % fd
        if vf.device is not None:
            return vf.device.ioctl(self, fd, cmd, arg)
        return -ENOTTY, "fd=%d cmd=0x%x" % (fd, cmd)

    def sys_221(self, fd, cmd, arg, *_):              # fcntl64
        if fd not in self.files and fd > 2:
            return -EBADF, "fd=%d" % fd
        if cmd in (1, 2, 3, 4):                       # GETFD SETFD GETFL SETFL
            return 0, "fd=%d cmd=%d" % (fd, cmd)
        raise ProcError("fcntl cmd %d" % cmd)

    def sys_122(self, buf, *_):                       # uname
        fields = [b"Linux", b"localhost", b"3.4.0", b"#1", b"armv7l", b""]
        self.write(buf, b"".join(f.ljust(65, b"\0") for f in fields))
        return 0, ""

    def sys_191(self, res, rlim, *_):                 # ugetrlimit
        self.write(rlim, struct.pack("<II", 0x800000 if res == 3 else 1024, 0xFFFFFFFF))
        return 0, "res=%d" % res

    def sys_256(self, *_):                            # set_tid_address
        return self.lay.pid, ""

    def sys_268(self, tgid, tid, sig, *_):            # tgkill
        raise ProcError("tgkill(%d,%d,%d): the process aborted itself" % (tgid, tid, sig))

    def sys_242(self, pid, sz, mask, *_):             # sched_getaffinity
        self.write(mask, b"\x01" + b"\0" * (sz - 1))
        return 4, ""

    def sys_116(self, buf, *_):                       # sysinfo
        self.write(buf, struct.pack("<iIIIIIIIIIHHII", 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1)[:64])
        return 0, ""

    def sys_85(self, p, buf, n, *_):                  # readlink
        path = self.cstr(p)
        if path.startswith("/proc/self/fd/"):
            vf = self.files.get(int(path.rsplit("/", 1)[1]))
            if vf is None:
                return -ENOENT, repr(path)
            data = vf.path.encode()[:n]
            self.write(buf, data)
            return len(data), "%r -> %r" % (path, vf.path)
        hp = self.host_path(path)
        if hp and os.path.islink(hp):
            raise ProcError("readlink of a symlink: %r" % path)
        return (-EINVAL if hp and os.path.exists(hp) else -ENOENT), repr(path)

    def sys_332(self, dirfd, p, buf, n, *_):          # readlinkat
        return self.sys_85(p, buf, n)

    def sys_327(self, dirfd, p, buf, flags, *_):      # fstatat64
        return self.sys_195(p, buf)

    def sys_183(self, buf, n, *_):                    # getcwd
        self.write(buf, b"/\0")
        return 2, ""

    def sys_281(self, dom, typ, proto, *_):           # socket
        if dom != 1:
            return -EACCES, "domain=%d" % dom
        return self.new_fd(VFile("/dev/log/socket", b"")), "AF_UNIX type=%d" % typ

    def sys_283(self, fd, addr, alen, *_):            # connect
        vf = self.files.get(fd)
        if vf is None:
            return -EBADF, "fd=%d" % fd
        return 0, "fd=%d %r" % (fd, self.read(addr + 2, alen - 2).split(b"\0")[0])

    def _log_out(self, fd, data):
        self.log_sink.append((fd, data))

    def sys_290(self, fd, buf, n, flags, addr, alen):   # sendto
        data = self.read(buf, n)
        self._log_out(fd, data)
        return n, "fd=%d %r" % (fd, data[:100])

    def sys_296(self, fd, msg, flags, *_):            # sendmsg
        iov, cnt = self.u32(msg + 8), self.u32(msg + 12)
        data = b"".join(self.read(*struct.unpack("<II", self.read(iov + 8 * i, 8))) for i in range(cnt))
        self._log_out(fd, data)
        return len(data), "fd=%d %r" % (fd, data[:100])

    def sys_146(self, fd, iov, cnt, *_):              # writev
        total = 0
        data = b""
        for i in range(cnt):
            base, ln = struct.unpack("<II", self.read(iov + 8 * i, 8))
            data += self.read(base, ln)
            total += ln
        if fd in (1, 2) or (fd in self.files and self.files[fd].path.startswith("/dev/log")):
            self.log_sink.append((fd, data))
            return total, "fd=%d %r" % (fd, data[:80])
        raise ProcError("writev to fd %d" % fd)

    # -------------------------------------------------------------- running
    def run(self, start, until, timeout_insns=0):
        uc = self.uc
        try:
            uc.emu_start(start, until, count=timeout_insns)
        except UcError as e:
            if self.err is None:
                pc = self.reg(UC_ARM_REG_PC)
                self.err = ProcError("emulation fault: %s at pc=0x%08x lr=0x%08x sp=0x%08x"
                                     % (e, pc, self.reg(UC_ARM_REG_LR), self.reg(UC_ARM_REG_SP)))
        if self.err:
            e, self.err = self.err, None
            raise e

    def call(self, addr, args=(), sp=None):
        """Call a guest function (ARM or Thumb by bit 0) and return r0 (and r1)."""
        uc = self.uc
        sp = sp if sp is not None else self.call_sp
        nstack = max(0, len(args) - 4)
        sp -= round_up(4 * nstack, 8)
        for i, v in enumerate(args):
            if i < 4:
                uc.reg_write((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)[i], v & 0xFFFFFFFF)
            else:
                self.w32(sp + 4 * (i - 4), v)
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, SENTINEL)
        self.run(addr, SENTINEL)
        return uc.reg_read(UC_ARM_REG_R0)

    # -------------------------------------------------------------- symbols
    def symbolize(self, addr):
        """library:offset for an address inside a mapped library, else None."""
        addr &= ~1
        best = None
        for start, ln, path, off, prot, flags in self.mmaps:
            if prot & 4 and start <= addr < start + ln:
                best = path
        if best is None:
            return None
        return "%s+0x%x" % (os.path.basename(best), addr - self.module_base(best))

    def caller_in(self, libs, scan_words=512):
        """First return address on the stack that lies in one of `libs` and follows a call
        instruction.  A heuristic: stale stack contents can match."""
        sp = self.reg(UC_ARM_REG_SP)
        try:
            data = self.read(sp, 4 * scan_words)
        except Exception:
            return None
        for i in range(scan_words):
            v, = struct.unpack_from("<I", data, 4 * i)
            sym = self.symbolize(v) if 0x10000 <= v < 0xC0000000 else None
            if sym is None or sym.split("+")[0] not in libs:
                continue
            a = v & ~1
            try:
                prev = self.read(a - 4, 4)
            except Exception:
                continue
            hw1, hw2 = struct.unpack("<HH", prev)
            thumb_bl = (hw1 & 0xF800) == 0xF000 and (hw2 & 0xC000) == 0xC000
            arm_bl = (struct.unpack("<I", prev)[0] >> 24) & 0xF == 0xB
            thumb_blx_reg = (hw2 & 0xFF80) == 0x4780
            if v & 1 and (thumb_bl or thumb_blx_reg) or (not v & 1 and arm_bl):
                return sym
        return None

    def module_base(self, path):
        """Load address of a library the linker mapped (path as opened, e.g.
        /system/lib/libc.so)."""
        if path in self.loaded:
            return self.loaded[path][0]
        hp = self.host_path(path)
        elf = ELFFile(open(hp, "rb"))
        first = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"][0]
        for start, ln, p, off, prot, flags in self.mmaps:
            if p == path and flags & MAP_FIXED and off == first["p_offset"] & ~(PAGE - 1):
                base = start - (first["p_vaddr"] & ~(PAGE - 1))
                self.loaded[path] = (base, elf)
                return base
        raise ProcError("library %s is not loaded" % path)

    def sym(self, path, name):
        base = self.module_base(path)
        elf = self.loaded[path][1]
        for secname in (".dynsym",):
            sec = elf.get_section_by_name(secname)
            for s in sec.iter_symbols():
                if s.name == name and s["st_shndx"] != "SHN_UNDEF":
                    return base + s["st_value"]
        raise ProcError("symbol %s not found in %s" % (name, path))

    # -------------------------------------------------------------- loading
    def load_elf_segments(self, data, base):
        elf = ELFFile(__import__("io").BytesIO(data))
        top = 0
        for seg in elf.iter_segments():
            if seg["p_type"] == "PT_LOAD":
                top = max(top, seg["p_vaddr"] + seg["p_memsz"])
        lo = min(seg["p_vaddr"] for seg in elf.iter_segments() if seg["p_type"] == "PT_LOAD") & ~(PAGE - 1)
        self.map(base + lo, round_up(top - lo, PAGE))
        for seg in elf.iter_segments():
            if seg["p_type"] == "PT_LOAD":
                self.write(base + seg["p_vaddr"], seg.data())
        return elf

    def build_exe(self, needed, entry_stub=True):
        """A minimal position-independent executable that names the libraries to load; its entry point
        reports to the harness."""
        base = self.lay.exe_base
        interp = b"/system/bin/linker\0"
        strtab = b"\0"
        offs = []
        for n in needed:
            offs.append(len(strtab))
            strtab += n.encode() + b"\0"
        strtab = strtab.ljust(round_up(len(strtab), 4), b"\0")
        nph = 5
        phoff, ehsize, phsize = 52, 52, 32
        o_interp = phoff + nph * phsize
        o_text = round_up(o_interp + len(interp), 8)
        # mov r7, #0x900000 ; orr r7, r7, #1 ; svc 0 ; b .
        text = struct.pack("<IIII", 0xE3A07609, 0xE3877001, 0xEF000000, 0xEAFFFFFE)
        o_hash = o_text + len(text)
        hashtab = struct.pack("<IIII", 1, 1, 0, 0)
        o_sym = round_up(o_hash + len(hashtab), 4)
        symtab = b"\0" * 16
        o_str = o_sym + len(symtab)
        o_dyn = round_up(o_str + len(strtab), 8)
        dyn = b""
        for o in offs:
            dyn += struct.pack("<iI", 1, o)                  # DT_NEEDED
        for tag, val in ((4, o_hash), (5, o_str), (6, o_sym),
                         (10, len(strtab)), (11, 16), (21, 0), (0, 0)):
            dyn += struct.pack("<iI", tag, val)
        size = o_dyn + len(dyn)
        ph = b""
        ph += struct.pack("<8I", 6, phoff, phoff, phoff, nph * phsize, nph * phsize, 4, 4)       # PHDR
        ph += struct.pack("<8I", 3, o_interp, o_interp, o_interp, len(interp), len(interp), 4, 1)   # INTERP
        ph += struct.pack("<8I", 1, 0, 0, 0, size, size, 7, 0x1000)                                       # LOAD rwx
        ph += struct.pack("<8I", 2, o_dyn, o_dyn, o_dyn, len(dyn), len(dyn), 6, 4)                # DYNAMIC
        ph += struct.pack("<8I", 0x6474E551, 0, 0, 0, 0, 0, 6, 4)                                               # GNU_STACK
        eh = b"\x7fELF\x01\x01\x01\0" + b"\0" * 8
        eh += struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, o_text, phoff, 0, 0x05000200,
                          ehsize, phsize, nph, 40, 0, 0)
        img = bytearray(size)
        img[0:len(eh)] = eh
        img[phoff:phoff + len(ph)] = ph
        img[o_interp:o_interp + len(interp)] = interp
        img[o_text:o_text + len(text)] = text
        img[o_hash:o_hash + len(hashtab)] = hashtab
        img[o_sym:o_sym + len(symtab)] = symtab
        img[o_str:o_str + len(strtab)] = strtab
        img[o_dyn:o_dyn + len(dyn)] = dyn
        return base, bytes(img), base + o_text, phoff, nph

    def start(self, needed, argv=("/system/bin/sgxrec",), env=()):
        """Map linker and executable, build the initial stack, run until the
        executable's entry point is reached (all libraries loaded, relocated
        and initialised by the linker)."""
        exe_base, exe_img, exe_entry, phoff, nph = self.build_exe(needed)
        self.map(exe_base, round_up(len(exe_img), PAGE))
        self.write(exe_base, exe_img)
        lpath = os.path.join(self.sysroot, "bin", "linker")
        with open(lpath, "rb") as f:
            ldata = f.read()
        lelf = ELFFile(__import__("io").BytesIO(ldata))
        lsize = max(s["p_vaddr"] + s["p_memsz"] for s in lelf.iter_segments() if s["p_type"] == "PT_LOAD")
        lbase = self.mmap_alloc(round_up(lsize, PAGE))
        self.load_elf_segments(ldata, lbase)
        for seg in lelf.iter_segments():
            if seg["p_type"] == "PT_LOAD" and seg["p_flags"] & 1:
                self.neonfix.scan(seg.data(), lbase + seg["p_vaddr"])
        self.linker_base = lbase
        self.linker_elf = lelf
        # stack
        stk = self.lay.stack_top
        self.map(stk - self.lay.stack_size, self.lay.stack_size + PAGE)
        sp = stk
        strs = []

        def push_str(s):
            nonlocal sp
            b = s.encode() + b"\0"
            sp -= len(b)
            self.write(sp, b)
            return sp
        arg_ptrs = [push_str(a) for a in argv]
        env_ptrs = [push_str(e) for e in env]
        plat = push_str("v7l")
        sp -= 16
        rnd = sp
        self.write(rnd, bytes(range(0x40, 0x50)))
        sp &= ~0xF
        auxv = [(3, exe_base + phoff), (4, 32), (5, nph), (6, PAGE), (7, lbase), (8, 0),
                (9, exe_entry), (11, 1000), (12, 1000), (13, 1000), (14, 1000), (15, plat),
                (16, 0xB0D7), (17, 100), (23, 0), (25, rnd), (0, 0)]
        words = [len(argv)] + arg_ptrs + [0] + env_ptrs + [0]
        for k, v in auxv:
            words += [k, v]
        sp -= 4 * len(words)
        sp &= ~0x7
        self.write(sp, struct.pack("<%dI" % len(words), *words))
        self.initial_sp = sp
        self.call_sp = (sp - 0x10000) & ~0xF
        self.uc.reg_write(UC_ARM_REG_SP, sp)
        self.uc.reg_write(UC_ARM_REG_R0, 0)
        entry = lbase + lelf.header["e_entry"]
        self.stop_reason = None
        self.run(entry, 0xFFFFFFF0)
        if self.stop_reason != "ready":
            raise ProcError("linker returned without reaching the executable's entry point")
