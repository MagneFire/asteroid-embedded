"""A fake PowerVR services kernel (/dev/pvrsrvkm) for the user-space libraries.

It answers the bridge ioctls the libraries issue, using the structure layouts of
the vendor kernel driver (services4/include/pvr_bridge.h, sgx_bridge.h,
sgxinfo.h), presents the device as srvinit_emu.py left it (same heap table, same
asDevData, init allocations in place) and records every allocation, bridge
call and kick.  Nothing here executes the GPU: DOKICK snapshots memory, applies
the completion writes the microkernel would make (see complete_kick) and
snapshots again.
"""
import json
import os
import struct

from srvinit_emu import HEAPS, HEAP_ID, place_allocation, PAGE, round_up, diff_ranges
from armproc import ProcError, ENOENT, EINVAL

PVRSRV_OK = 0
PVRSRV_ERROR_OUT_OF_MEMORY = 1
PVRSRV_ERROR_INVALID_PARAMS = 3
PVRSRV_ERROR_BAD_MAPPING = 0x6C       # not used by the libraries here; reported if hit

MEM_NO_SYNCOBJ = 1 << 3
MEM_CACHE_CONSISTENT = 1 << 2
MEM_RAM_BACKED = 1 << 9
HAP_CACHETYPE_MASK = (1 << 12) | (1 << 13) | (1 << 14)
MEM_NOUSERVIRTUAL = 1 << 27

# ioctl number = 0xC01C6700 | bridge id; ids from the PVRSRV_BRIDGE_* ordering in pvr_bridge.h / sgx_bridge.h
BRIDGE_NAMES = {
    0: "COMPAT_CHECK", 1: "ENUM_DEVICES", 2: "ACQUIRE_DEVICEINFO", 3: "RELEASE_DEVICEINFO",
    4: "CREATE_DEVMEMCONTEXT", 5: "DESTROY_DEVMEMCONTEXT", 6: "GET_DEVMEM_HEAPINFO",
    7: "ALLOC_DEVICEMEM", 8: "FREE_DEVICEMEM", 9: "GETFREE_DEVICEMEM", 12: "MHANDLE_TO_MMAP_DATA",
    13: "CONNECT_SERVICES", 14: "DISCONNECT_SERVICES", 16: "GET_DEVICEMEMINFO",
    26: "RELEASE_MMAP_DATA", 37: "GET_MISC_INFO", 38: "RELEASE_MISC_INFO",
    71: "EVENT_OBJECT_WAIT", 72: "EVENT_OBJECT_OPEN", 73: "EVENT_OBJECT_CLOSE",
    85: "SGX_GETCLIENTINFO", 86: "SGX_RELEASECLIENTINFO", 87: "SGX_GETINTERNALDEVINFO", 88: "SGX_DOKICK",
    99: "SGX_GETMISCINFO", 105: "SGX_REGISTER_HW_RENDER_CONTEXT", 106: "SGX_FLUSH_HW_RENDER_TARGET",
    107: "SGX_UNREGISTER_HW_RENDER_CONTEXT", 111: "SGX_SCHEDULE_PROCESS_QUEUES",
}
SGX_BUILD_OPTIONS = 0x0016801C


class Alloc:
    def __init__(self, index, heap, size, align, flags, devva, site, call, kernel, fill=0):
        self.index, self.heap, self.size, self.align = index, heap, size, align
        self.flags, self.devva, self.site, self.call = flags, devva, site, call
        self.kernel = kernel
        self.data = bytearray([fill]) * round_up(size, PAGE)
        self.guest = None          # CPU mapping in the process, once mmap'd
        self.sync = None           # kernel Alloc holding the sync data, or None
        self.freed_at = None
        self.handle = 0
        self.map_handle = 0
        self.mmap_key = 0
        self.client_meminfo_flags = 0
        self.tag = None
        self.owner = None


class FakeKernel:
    CLIENT_LIBS = ("libGLESv2_POWERVR_SGX530_125.so", "libIMGegl.so", "libEGL_POWERVR_SGX530_125.so",
                   "libglslcompiler.so", "libusc.so", "libpvrANDROID_WSEGL.so")

    def __init__(self, init_dir, proc, shift=0, fill=0):
        self.fill = fill
        self.proc = proc
        self.shift = shift
        self.init_dir = init_dir
        with open(os.path.join(init_dir, "manifest.json")) as f:
            self.init = json.load(f)
        self.heap_prev_end = {}
        for a in self.init["allocations"]:
            if not a["freed"]:
                self.heap_prev_end[a["heap_id"]] = a["devva"] + a["size"]
        self.init_dev_data = self.init["devinitpart2"]["asInitDevData"]
        self.allocs = []
        self.live = []
        self.bridge_log = []
        self.kicks = []
        self.pid = 1000
        self.next_handle = 0x1000 + (shift >> 12) * 4
        self.handles = {}
        self.cookie = self.new_handle("devcookie")
        self.memctx = None
        self.heap_handles = {}
        self.mmap_keys = {}
        self.event_handle = None
        self.frame_counter = 0
        self.timer_page = None
        self.out_dir = None
        self.snapshot_hook = None
        self.hw_ctx_alloc = None
        self.current_call = None
        self.event_waits = 0
        self.dispatch = {}
        for bid in BRIDGE_NAMES:
            fn = getattr(self, "br_%s" % BRIDGE_NAMES[bid].lower(), None)
            if fn:
                self.dispatch[bid] = fn

    # --- handle bookkeeping ---------------------------------------------------
    def new_handle(self, kind, obj=None):
        h = self.next_handle
        self.next_handle += 4
        self.handles[h] = (kind, obj)
        return h

    def get(self, h, kind):
        e = self.handles.get(h)
        if e is None or e[0] != kind:
            raise ProcError("bridge call used handle 0x%x as %s but it is %s" % (h, kind, e and e[0]))
        return e[1]

    # --- device node interface used by armproc ----------------------------------
    def open(self, proc, fd):
        self.fd = fd

    def close(self, proc, fd):
        pass

    def ioctl(self, proc, fd, cmd, arg):
        if cmd >> 8 != 0xC01C67:
            raise ProcError("ioctl 0x%x on /dev/pvrsrvkm is not a bridge call" % cmd)
        bid = cmd & 0xFF
        pk = proc.read(arg, 28)
        pid_, size, pin, insz, pout, outsz, hsrv = struct.unpack("<7I", pk)
        if pid_ not in (bid, cmd) or size != 28:
            raise ProcError("bridge package mismatch: id %d vs ioctl %d, size %d" % (pid_, bid, size))
        inb = proc.read(pin, insz) if insz else b""
        name = BRIDGE_NAMES.get(bid)
        fn = self.dispatch.get(bid)
        entry = {"seq": len(self.bridge_log), "id": bid, "name": name or "UNKNOWN_%d" % bid,
                 "in_size": insz, "out_size": outsz, "in_hex": inb.hex()}
        self.bridge_log.append(entry)
        if fn is None:
            raise ProcError("unimplemented bridge call %d (%s): in=%s out_size=%d"
                            % (bid, name, inb.hex(), outsz))
        self.current_call = entry
        entry["site"] = "0x%x" % (proc.reg(14) & ~1)
        out = bytearray(outsz)
        ret = fn(inb, out)
        entry["out_hex"] = bytes(out).hex()
        if outsz:
            proc.write(pout, bytes(out))
        text = "bridge %s in=%d out=%d" % (entry["name"], insz, outsz)
        entry.update(self.decode(entry["name"], inb, bytes(out)))
        return (ret or 0), text

    def decode(self, name, inb, out):
        return {}

    # --- memory ------------------------------------------------------------------
    def heap_index(self, h):
        return self.get(h, "heap")

    def place(self, hidx, size, align):
        hid, hname, hbase, hsize, hattr = HEAPS[hidx]
        devva = place_allocation(hbase, self.heap_prev_end.get(hidx), size, align)
        if devva + size > hbase + hsize + PAGE:
            return None
        self.heap_prev_end[hidx] = devva + size
        return devva

    def alloc(self, hidx, size, align, flags, kernel, site, tag=None):
        devva = self.place(hidx, size, align)
        if devva is None:
            return None
        a = Alloc(len(self.allocs), hidx, size, align, flags, devva, site,
                  self.current_call["name"] if self.current_call else "", kernel, self.fill)
        a.owner = None if kernel else {"services_site": self.proc.symbolize(site),
                                       "client_site_heuristic": self.proc.caller_in(self.CLIENT_LIBS)}
        a.handle = self.new_handle("meminfo", a)
        a.map_handle = self.new_handle("osmem", a)
        a.mmap_key = len(self.allocs) + 1 + (self.shift >> 20)
        a.tag = tag
        self.mmap_keys[a.mmap_key] = a
        self.allocs.append(a)
        self.live.append(a)
        if self.current_call is not None:
            self.current_call.setdefault("allocs", []).append(a.index)
        return a

    def read(self, a):
        if a.guest is not None:
            return self.proc.read(a.guest, a.size)
        return bytes(a.data[:a.size])

    def write(self, a, off, data):
        if a.guest is not None:
            self.proc.write(a.guest + off, data)
        else:
            a.data[off:off + len(data)] = data

    def find(self, devva):
        for a in self.live:
            if a.devva <= devva < a.devva + max(a.size, 1):
                return a
        for ia in self.init["allocations"]:
            if ia["devva"] <= devva < ia["devva"] + ia["size"]:
                return None
        return None

    def w32gpu(self, devva, v):
        a = self.find(devva)
        if a is None:
            raise ProcError("completion write to GPU address 0x%08x which is not in a recorded allocation" % devva)
        self.write(a, devva - a.devva, struct.pack("<I", v & 0xFFFFFFFF))

    def r32gpu(self, devva):
        a = self.find(devva)
        if a is None:
            raise ProcError("read of GPU address 0x%08x which is not in a recorded allocation" % devva)
        o = devva - a.devva
        return struct.unpack("<I", self.read(a)[o:o + 4])[0]

    def mmap(self, proc, start, length, prot, flags, offset, fixed):
        a = self.mmap_keys.get(offset >> 12)
        if a is None:
            if offset == 0:
                raise ProcError("mmap of /dev/pvrsrvkm at offset 0")
            raise ProcError("mmap of /dev/pvrsrvkm with unknown offset 0x%x" % offset)
        if a.guest is not None:
            raise ProcError("allocation #%d is mapped twice" % a.index)
        if length < round_up(a.size, PAGE) and a.tag != "timer":
            raise ProcError("mmap of #%d shorter than the allocation" % a.index)
        proc.map(start, length)
        proc.write(start, bytes(a.data))
        a.guest = start
        proc.log("   mmap #%d (%s, %d bytes) at guest 0x%08x" % (a.index, a.tag or HEAPS[a.heap][1], a.size, start))
        return start

    # --- bridge calls ---------------------------------------------------------------
    @staticmethod
    def i32(b, off):
        return struct.unpack_from("<I", b, off)[0]

    def br_compat_check(self, inb, out):
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    def br_connect_services(self, inb, out):
        struct.pack_into("<II", out, 0, PVRSRV_OK, self.new_handle("services"))

    def br_disconnect_services(self, inb, out):
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    def br_enum_devices(self, inb, out):
        struct.pack_into("<II", out, 0, PVRSRV_OK, 1)
        struct.pack_into("<5I", out, 8, 7, 0, 0, 0, 0)       # type SGX, class 3D, index 0

    def br_acquire_deviceinfo(self, inb, out):
        _, idx, typ = struct.unpack_from("<III", inb, 0)
        if idx != 0 or typ not in (0, 7):
            raise ProcError("ACQUIRE_DEVICEINFO index %d type %d" % (idx, typ))
        struct.pack_into("<II", out, 0, PVRSRV_OK, self.cookie)

    def br_release_deviceinfo(self, inb, out):
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    def heap_info_blob(self):
        blob = b""
        for hid, name, base, size, attr in HEAPS:
            h = self.heap_handles.setdefault(hid, self.new_handle("heap", hid))
            blob += struct.pack("<6I", HEAP_ID(hid), h, base, size, attr, 0)
        return blob

    def br_create_devmemcontext(self, inb, out):
        if self.memctx is None:
            self.memctx = self.new_handle("memctx")
        blob = self.heap_info_blob()
        self.current_call["decoded"] = {"heaps": [h[1] for h in HEAPS]}
        struct.pack_into("<III", out, 0, PVRSRV_OK, self.memctx, len(HEAPS))
        out[12:12 + len(blob)] = blob

    def br_get_devmem_heapinfo(self, inb, out):
        blob = self.heap_info_blob()
        struct.pack_into("<II", out, 0, PVRSRV_OK, len(HEAPS))
        out[8:8 + len(blob)] = blob

    def br_alloc_devicemem(self, inb, out):
        # IN: flags, hDevCookie, hDevMemHeap, ui32Attribs, uSize, uAlignment, pvPrivData,
        #     ui32PrivDataLength [, ui32ChunkSize, ui32NumVirtChunks, ui32NumPhysChunks, pabMapChunk]
        _, hcookie, hheap, attribs, size, align, priv, privlen = struct.unpack_from("<8I", inb, 0)
        hid = self.get(hheap, "heap")
        hidx = hid
        if not size:
            raise ProcError("zero-size ALLOC_DEVICEMEM")
        if len(inb) > 32 and any(struct.unpack_from("<4I", inb, 32)):
            raise ProcError("sparse allocation fields in ALLOC_DEVICEMEM")
        caller = self.proc.reg(14) & ~1
        a = self.alloc(hidx, size, align, 0, False, caller)
        if a is None:
            struct.pack_into("<I", out, 0, PVRSRV_ERROR_OUT_OF_MEMORY)
            return
        hattr = HEAPS[hidx][4]
        flags = attribs | MEM_RAM_BACKED
        flags |= hattr
        if attribs & HAP_CACHETYPE_MASK:
            flags = flags & ~HAP_CACHETYPE_MASK | attribs & HAP_CACHETYPE_MASK
        a.flags = attribs
        a.client_meminfo_flags = flags
        if not attribs & MEM_NO_SYNCOBJ:
            s = self.alloc(8, 40, 4, MEM_CACHE_CONSISTENT, True, caller, tag="sync")
            s.data[:40] = bytes(40)             # PVRSRVAllocSyncInfoKM clears the sync data
            a.sync = s
        self.current_call["decoded"] = {
            "heap_id": HEAPS[hidx][0], "heap": HEAPS[hidx][1], "size": size, "alignment": align,
            "attribs": attribs, "returned_flags": flags, "devva": a.devva, "alloc_index": a.index,
            "sync_alloc_index": a.sync.index if a.sync else None}
        struct.pack_into("<II", out, 0, PVRSRV_OK, a.handle)
        # PVRSRV_CLIENT_MEM_INFO (0x38)
        mi = struct.pack("<IIIIIIIIIIQI", 0, 0, a.devva, flags, 0, size, 0, a.map_handle, a.handle, 0, 0, 0)
        out[8:8 + 0x34] = mi[:0x34]
        if a.sync is not None:
            s = a.sync
            struct.pack_into("<6I", out, 8 + 0x38, 0, s.devva + 4, s.devva + 12, s.devva + 20,
                             s.map_handle, s.handle)
            struct.pack_into("<I", out, 8 + 0x18, 0)  # psClientSyncInfo is set by the client library
        return 0

    def br_free_devicemem(self, inb, out):
        _, hcookie, hmi = struct.unpack_from("<3I", inb, 0)
        a = self.get(hmi, "meminfo")
        self.current_call["decoded"] = {"alloc_index": a.index, "devva": a.devva}
        self.free(a)
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    def free(self, a):
        if a.freed_at is None:
            a.freed_at = len(self.bridge_log) - 1
            if a in self.live:
                self.live.remove(a)
            if a.sync is not None:
                self.free(a.sync)

    def br_mhandle_to_mmap_data(self, inb, out):
        _, h = struct.unpack_from("<2I", inb, 0)
        e = self.handles.get(h)
        if e is None or e[0] not in ("osmem", "meminfo"):
            raise ProcError("MHANDLE_TO_MMAP_DATA with handle 0x%x (%s)" % (h, e and e[0]))
        a = e[1]
        self.current_call["decoded"] = {"alloc_index": a.index if a.index >= 0 else "timer-page",
                                        "mmap_offset_pages": a.mmap_key, "real_byte_size": round_up(a.size, PAGE)}
        struct.pack_into("<5I", out, 0, PVRSRV_OK, a.mmap_key, 0, round_up(a.size, PAGE), 0)

    def br_release_mmap_data(self, inb, out):
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    MISC_SIZE = 0x94
    GLOBAL_EVENT_NAME = b"PVRSRV_GLOBAL_EVENTOBJECT"

    def br_get_misc_info(self, inb, out):
        mi = bytearray(inb[4:4 + self.MISC_SIZE])
        req = struct.unpack_from("<I", mi, 0)[0]
        if req & ~(1 | 8):
            raise ProcError("GET_MISC_INFO state request 0x%x not implemented" % req)
        present = 0
        if req & 1:
            if self.timer_page is None:
                t = Alloc(-1, 0, PAGE, PAGE, 0, 0, 0, "GET_MISC_INFO", True)
                t.tag = "timer"
                t.handle = self.new_handle("osmem", t)
                t.map_handle = t.handle
                t.mmap_key = 0x7000 + len(self.mmap_keys) + (self.shift >> 20)
                self.mmap_keys[t.mmap_key] = t
                self.timer_page = t
            present |= 1
            struct.pack_into("<I", mi, 8, 0xC0000000)                     # pvSOCTimerRegisterKM (kernel pointer, not used)
            struct.pack_into("<I", mi, 0x10, self.timer_page.map_handle)  # hSOCTimerRegisterOSMemHandle
        if req & 8:
            present |= 8
            if self.event_handle is None:
                self.event_handle = self.new_handle("event")
            mi[0x28:0x28 + 50] = self.GLOBAL_EVENT_NAME.ljust(50, b"\0")
            struct.pack_into("<I", mi, 0x5C, self.event_handle)
        struct.pack_into("<I", mi, 4, present)
        self.current_call["decoded"] = {"state_request": req, "state_present": present}
        struct.pack_into("<I", out, 0, PVRSRV_OK)
        out[4:4 + self.MISC_SIZE] = mi

    def br_event_object_open(self, inb, out):
        name = inb[:50].split(b"\0")[0]
        if name != self.GLOBAL_EVENT_NAME:
            raise ProcError("EVENT_OBJECT_OPEN of %r" % name)
        struct.pack_into("<II", out, 0, self.event_handle, PVRSRV_OK)

    def br_event_object_close(self, inb, out):
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    def br_event_object_wait(self, inb, out):
        self.event_waits += 1
        if self.event_waits > 2000:
            raise ProcError("more than 2000 event waits without a kick: user space is waiting for a state change "
                            "the fake kernel does not make")
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    def br_release_misc_info(self, inb, out):
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    # --- SGX ------------------------------------------------------------------------
    DDK_VERSION = (1 << 16) | (12 << 8)
    DDK_BUILD = (2917986 << 8) | 4          # PVRVERSION_MOTBUILD with MOT_BUILD=4 (kernel Makefile)

    def br_sgx_getmiscinfo(self, inb, out):
        _, hcookie, pmi = struct.unpack_from("<3I", inb, 0)
        mi = bytearray(self.proc.read(pmi, 0x40))
        req = struct.unpack_from("<I", mi, 0)[0]
        if req == 3:       # DRIVER_SGXREV
            mi[8:8 + 32] = struct.pack("<8I", 0, 0, self.DDK_VERSION, self.DDK_BUILD, 0, 0, SGX_BUILD_OPTIONS, 0)
        else:
            raise ProcError("SGX_GETMISCINFO request %d" % req)
        self.proc.write(pmi, bytes(mi))
        self.current_call["decoded"] = {"request": req, "result_words": list(struct.unpack_from("<8I", mi, 8))}
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    def br_sgx_getclientinfo(self, inb, out):
        # OUT: SGX_CLIENT_INFO (ProcessID, pvProcess, PVRSRV_MISC_INFO[0x94], asDevData[24]) then eError
        struct.pack_into("<I", out, 0, self.pid)
        struct.pack_into("<24I", out, 0x9C, *self.init_dev_data)
        struct.pack_into("<I", out, 0xFC, PVRSRV_OK)

    def br_sgx_releaseclientinfo(self, inb, out):
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    def br_sgx_getinternaldevinfo(self, inb, out):
        # OUT: SGX_INTERNAL_DEVINFO {ui32Flags, hHostCtlKernelMemInfoHandle, bForcePTOff}, eError
        hostctl = self.init["devinitpart2"]["hKernelSGXHostCtlMemInfo"]
        struct.pack_into("<III", out, 0, 0, self.init_handle(hostctl), 0)
        struct.pack_into("<I", out, 12, PVRSRV_OK)

    def init_handle(self, init_index):
        key = ("init", init_index)
        for h, (kind, obj) in self.handles.items():
            if kind == "initmem" and obj == init_index:
                return h
        return self.new_handle("initmem", init_index)

    APP_PD_DEV_PADDR = 0x8F100000       # page directory of the application's memory context (a physical address; fake)
    PD_ALIGN_SHIFT = 12

    def br_sgx_register_hw_render_context(self, inb, out):
        _, hcookie, ptr, size, patch_off, hmemctx = struct.unpack_from("<6I", inb, 0)
        self.get(hmemctx, "memctx")
        caller = self.proc.reg(14) & ~1
        flags = 1 | 2 | MEM_NO_SYNCOBJ | (1 << 6) | MEM_CACHE_CONSISTENT
        a = self.alloc(3, size, 32, flags, True, caller, tag="HW render context")
        if a is None:
            struct.pack_into("<I", out, 0, PVRSRV_ERROR_OUT_OF_MEMORY)
            return
        a.flags = flags
        img = bytearray(self.proc.read(ptr, size))
        struct.pack_into("<I", img, patch_off, self.APP_PD_DEV_PADDR >> self.PD_ALIGN_SHIFT)
        self.write(a, 0, bytes(img))
        self.hw_ctx_alloc = a
        h = self.new_handle("hwrendercontext", a)
        self.current_call["decoded"] = {"alloc_index": a.index, "devva": a.devva, "image_size": size,
                                        "pd_patch_offset": patch_off, "pd_dev_paddr_fake": self.APP_PD_DEV_PADDR,
                                        "image_hex": img.hex()}
        struct.pack_into("<III", out, 0, PVRSRV_OK, h, a.devva)

    def br_sgx_unregister_hw_render_context(self, inb, out):
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    def br_sgx_flush_hw_render_target(self, inb, out):
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    # --- DOKICK -------------------------------------------------------------------------
    KICK_SIZE = 0x298
    SHARED_SIZE = 0x468
    SYNC_WRITE_PENDING, SYNC_WRITE_COMPLETE = 0, 4
    SYNC_READ_PENDING, SYNC_READ_COMPLETE = 8, 12
    SYNC_READ2_PENDING, SYNC_READ2_COMPLETE = 16, 20

    def sync_alloc(self, h):
        if not h:
            return None
        a = self.handles.get(h)
        if a is None or a[0] != "meminfo":
            raise ProcError("kick references sync handle 0x%x which is not a sync object" % h)
        return a[1]

    def sget(self, s, off):
        return struct.unpack_from("<I", self.read(s), off)[0]

    def sput(self, s, off, v):
        self.write(s, off, struct.pack("<I", v & 0xFFFFFFFF))

    def take_read(self, s):
        v = self.sget(s, self.SYNC_READ_PENDING)
        self.sput(s, self.SYNC_READ_PENDING, v + 1)
        return v

    def take_write(self, s):
        v = self.sget(s, self.SYNC_WRITE_PENDING)
        self.sput(s, self.SYNC_WRITE_PENDING, v + 1)
        return v

    def decode_kick(self, kb):
        u = lambda o: struct.unpack_from("<I", kb, o)[0]
        k = {"sCommand": {"ui32ServiceAddress": u(0), "ui32CacheControl": u(4),
                          "ui32Data": [u(8 + 4 * i) for i in range(6)]}}
        k.update({"hCCBKernelMemInfo": u(0x20), "ui32NumDstSyncObjects": u(0x24),
                  "hKernelHWSyncListMemInfo": u(0x28), "pahDstSyncHandles": u(0x2C),
                  "ui32NumTAStatusVals": u(0x30), "ui32Num3DStatusVals": u(0x34)})
        k["TAStatus"] = [{"addr": u(0x38 + 12 * i), "value": u(0x3C + 12 * i), "hMemInfo": u(0x40 + 12 * i)}
                         for i in range(u(0x30))]
        k["3DStatus"] = [{"addr": u(0x1B8 + 12 * i), "value": u(0x1BC + 12 * i), "hMemInfo": u(0x1C0 + 12 * i)}
                         for i in range(u(0x34))]
        k.update({"bFirstKickOrResume": u(0x1E8), "bLastInScene": u(0x1EC), "ui32CCBOffset": u(0x1F0),
                  "ui32NumSrcSyncs": u(0x1F4), "ahSrcKernelSyncInfo": [u(0x1F8 + 4 * i) for i in range(u(0x1F4))],
                  "bTADependency": u(0x278), "hTA3DSyncInfo": u(0x27C), "hTASyncInfo": u(0x280),
                  "h3DSyncInfo": u(0x284), "hDevMemContext": u(0x288), "sHWRTDataSetDevAddr": u(0x28C),
                  "sHWRTDataDevAddr": u(0x290), "ui32FrameNum": u(0x294)})
        return k

    def br_sgx_dokick(self, inb, out):
        if len(inb) != 8 + self.KICK_SIZE:
            raise ProcError("DOKICK input is %d bytes, expected %d" % (len(inb), 8 + self.KICK_SIZE))
        kb = inb[8:]
        k = self.decode_kick(kb)
        u = lambda o: struct.unpack_from("<I", kb, o)[0]
        ccb = self.get(k["hCCBKernelMemInfo"], "meminfo")
        off = k["ui32CCBOffset"]
        if off + self.SHARED_SIZE > ccb.size:
            raise ProcError("invalid CCB offset 0x%x" % off)
        entry = self.current_call
        kick = {"index": len(self.kicks), "bridge_call": entry["seq"], "fields": k,
                "ccb_alloc": ccb.index, "ccb_offset": off,
                "raw_hex": kb.hex()}
        # the user-written part of the command, before the kernel touches it
        full = self.read(ccb)
        kick["cmdta_command_hex"] = full[off - 100:off - 100 + 0x540].hex() if off >= 100 else None
        self.kicks.append(kick)
        shared = bytearray(self.read(ccb)[off:off + self.SHARED_SIZE])
        p32 = lambda o, v: struct.pack_into("<I", shared, o, v & 0xFFFFFFFF)
        g32 = lambda o: struct.unpack_from("<I", shared, o)[0]
        sy = {}
        if k["hTA3DSyncInfo"]:
            s = self.sync_alloc(k["hTA3DSyncInfo"])
            p32(0x38, s.devva + self.SYNC_WRITE_COMPLETE)
            p32(0x34, self.sget(s, self.SYNC_WRITE_PENDING))
            if k["bTADependency"]:
                self.take_write(s)
            sy["ta3d"] = s.index
        if k["hTASyncInfo"]:
            s = self.sync_alloc(k["hTASyncInfo"])
            p32(0x18, s.devva + self.SYNC_READ_COMPLETE)
            p32(0x10, s.devva + self.SYNC_WRITE_COMPLETE)
            p32(0x14, self.take_read(s))
            p32(0x0C, self.sget(s, self.SYNC_WRITE_PENDING))
            sy["tatq"] = s.index
        if k["h3DSyncInfo"]:
            s = self.sync_alloc(k["h3DSyncInfo"])
            p32(0x28, s.devva + self.SYNC_READ_COMPLETE)
            p32(0x20, s.devva + self.SYNC_WRITE_COMPLETE)
            p32(0x24, self.take_read(s))
            p32(0x1C, self.sget(s, self.SYNC_WRITE_PENDING))
            sy["3dtq"] = s.index
        p32(0x04, k["ui32NumTAStatusVals"])
        p32(0x08, k["ui32Num3DStatusVals"])
        for i, st in enumerate(k["TAStatus"]):
            p32(0x348 + 8 * i, st["addr"])
            p32(0x34C + 8 * i, st["value"])
        for i, st in enumerate(k["3DStatus"]):
            p32(0x448 + 8 * i, st["addr"])
            p32(0x44C + 8 * i, st["value"])
        if k["ui32NumSrcSyncs"]:
            for i, h in enumerate(k["ahSrcKernelSyncInfo"]):
                s = self.sync_alloc(h)
                p32(0x48 + 24 * i + 4, s.devva + self.SYNC_READ_COMPLETE)
                p32(0x48 + 24 * i + 12, s.devva + self.SYNC_WRITE_COMPLETE)
                p32(0x48 + 24 * i + 0, self.take_read(s))
                p32(0x48 + 24 * i + 8, self.sget(s, self.SYNC_WRITE_PENDING))
                p32(0x48 + 24 * i + 16, self.sget(s, self.SYNC_READ2_PENDING))
                p32(0x48 + 24 * i + 20, s.devva + self.SYNC_READ2_COMPLETE)
        p32(0x44, k["ui32NumSrcSyncs"])
        dst_syncs = []
        if k["bFirstKickOrResume"] and k["ui32NumDstSyncObjects"] > 0:
            lst = self.get(k["hKernelHWSyncListMemInfo"], "meminfo")
            n = k["ui32NumDstSyncObjects"]
            body = bytearray(self.read(lst))
            struct.pack_into("<I", body, 4, n)
            handles = struct.unpack("<%dI" % n, self.proc.read(k["pahDstSyncHandles"], 4 * n))
            for i, h in enumerate(handles):
                base = 8 + 24 * i
                if h:
                    s = self.sync_alloc(h)
                    dst_syncs.append(s.index)
                    struct.pack_into("<6I", body, base,
                                     self.sget(s, self.SYNC_READ_PENDING), s.devva + self.SYNC_READ_COMPLETE,
                                     self.take_write(s), s.devva + self.SYNC_WRITE_COMPLETE,
                                     self.sget(s, self.SYNC_READ2_PENDING), s.devva + self.SYNC_READ2_COMPLETE)
                else:
                    struct.pack_into("<6I", body, base, 0, 0, 0, 0, 0, 0)
            self.write(lst, 0, bytes(body))
        p32(0x00, g32(0x00) | 1)                    # READY, written last
        self.write(ccb, off, bytes(shared))
        kick["sync_objects"] = {"named": sy, "dst": dst_syncs}
        self.current_call["decoded"] = {"kick": kick["index"]}
        kick["pre"] = self.snapshot("pre")
        self.complete_kick(kick, shared)
        kick["post"] = self.snapshot("post")
        self.event_waits = 0
        struct.pack_into("<I", out, 0, PVRSRV_OK)

    def complete_kick(self, kick, shared):
        """What the microkernel and the 3D core do when the work finishes."""
        k = kick["fields"]
        done = {"status_writes": [], "sync_completions": [], "resource_releases": [], "ccb_read": None}
        for st in k["TAStatus"] + k["3DStatus"]:
            self.w32gpu(st["addr"], st["value"])
            done["status_writes"].append([st["addr"], st["value"]])
        for a in self.live:
            if a.tag == "sync":
                for p, c in ((self.SYNC_WRITE_PENDING, self.SYNC_WRITE_COMPLETE),
                             (self.SYNC_READ_PENDING, self.SYNC_READ_COMPLETE),
                             (self.SYNC_READ2_PENDING, self.SYNC_READ2_COMPLETE)):
                    v = self.sget(a, p)
                    if self.sget(a, c) != v:
                        self.sput(a, c, v)
                        done["sync_completions"].append([a.index, c, v])
        if k["bLastInScene"]:
            cmd = bytes.fromhex(kick["cmdta_command_hex"])
            details, synclist = struct.unpack_from("<2I", cmd, 0x4C)
            rel = []
            if details:
                rel.append(struct.unpack("<I", self.read_gpu(details + 0x88, 4))[0])
            if synclist:
                rel.append(struct.unpack("<I", self.read_gpu(synclist, 4))[0])
            for addr in rel:
                if addr:
                    self.w32gpu(addr, 0)
                    done["resource_releases"].append(addr)
        ctx = self.hw_ctx_alloc
        ctl_addr = struct.unpack_from("<I", self.read(ctx), 0x10)[0]
        wr = self.r32gpu(ctl_addr)
        self.w32gpu(ctl_addr + 4, wr)
        done["ccb_read"] = [ctl_addr + 4, wr]
        kick["completion"] = done

    def read_gpu(self, devva, n):
        a = self.find(devva)
        if a is None:
            raise ProcError("read of GPU address 0x%08x outside recorded allocations" % devva)
        o = devva - a.devva
        return self.read(a)[o:o + n]

    # --- snapshots ----------------------------------------------------------------------
    def snapshot(self, tag):
        if self.snapshot_hook is None:
            return None
        return self.snapshot_hook(tag, self)
