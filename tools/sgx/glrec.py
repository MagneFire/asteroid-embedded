#!/usr/bin/env python3
"""Run the vendor EGL/GLES2 stack for the watch's SGX530 under Unicorn, against a
fake pvrsrvkm, and record the GPU memory and commands it produces.

Usage: glrec.py [--out DIR] [--shift N] SCENARIO...
Scenarios: tri tri2 tri_moved tri_color tri_blend tri_indexed quad_tex
See README.md.
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armproc  # noqa: E402
import glenv  # noqa: E402
import pvrkm  # noqa: E402
from srvinit_emu import HEAPS  # noqa: E402

WORKTREE = os.path.normpath(os.path.join(HERE, "..", ".."))
SYSTEM = os.path.normpath(os.path.join(WORKTREE, "..", "minnow", "dump", "extracted_data", "system"))
INIT_DIR = os.path.join(WORKTREE, "build", "sgx", "init")
EGL = "/system/lib/egl/libEGL_POWERVR_SGX530_125.so"
GLES = "/system/lib/egl/libGLESv2_POWERVR_SGX530_125.so"
NEEDED = ["libc.so", EGL, GLES, "libhardware.so", "libpvrANDROID_WSEGL.so"]

VS = "attribute vec2 p; attribute vec4 c; varying lowp vec4 v; void main(){ v=c; gl_Position=vec4(p,0.,1.); }"
FS = "varying lowp vec4 v; void main(){ gl_FragColor=v; }"
VS_TEX = "attribute vec2 p; attribute vec2 t; varying mediump vec2 u; void main(){ u=t; gl_Position=vec4(p,0.,1.); }"
FS_TEX = "uniform sampler2D t; varying mediump vec2 u; void main(){ gl_FragColor=texture2D(t,u); }"


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


class Session:
    """One emulated process with the fake kernel, and helpers to call the GL stack."""

    def __init__(self, outdir, shift=0, verbose=False, fill=0, general_gap=0):
        self.outdir = outdir
        self.lay = armproc.Layout(shift)
        self.proc = armproc.Proc(SYSTEM, self.lay, logfn=(lambda t: print(t[:300], file=sys.stderr)) if verbose else None)
        self.kernel = pvrkm.FakeKernel(INIT_DIR, self.proc, shift, fill, general_gap)
        self.proc.add_device("/dev/pvrsrvkm", self.kernel)
        self.proc.start(NEEDED)
        self.arena = glenv.Arena(self.proc)
        self.hal = glenv.install_hardware(self.proc, self.arena)
        self.wsegl = glenv.install_wsegl(self.proc, self.arena, [1, 2, 0, 0, 0, 0, 0, 0, 0])
        self.kernel.snapshot_hook = self.snapshot
        self.snapdirs = []
        self.verbose = verbose
        self.gl_errors = []

    # --- snapshots
    def snapshot(self, tag, kernel):
        n = len(kernel.kicks) - 1
        d = os.path.join(self.outdir, "kick%02d_%s" % (n, tag))
        os.makedirs(d, exist_ok=True)
        files = {}
        for a in kernel.live:
            name = "alloc_%03d.bin" % a.index
            with open(os.path.join(d, name), "wb") as f:
                f.write(kernel.read(a))
            files[a.index] = name
        return {"dir": os.path.basename(d), "files": files}

    # --- calling
    def e(self, name, *args):
        return self.proc.call(self.proc.sym(EGL, name), list(args))

    def g(self, name, *args):
        return self.proc.call(self.proc.sym(GLES, name), list(args))

    def put(self, data, align=16):
        a = self.arena.alloc(len(data), align)
        self.proc.write(a, data)
        return a

    def cstr(self, s):
        return self.arena.cstr(s)

    def words(self, *w):
        return self.put(struct.pack("<%dI" % len(w), *w))

    def floats(self, *x):
        return self.put(struct.pack("<%df" % len(x), *x))

    def out32(self):
        return self.arena.alloc(4, 4)

    def u32(self, a):
        return self.proc.u32(a)


def setup_egl(s, width=320, height=320):
    info = {}
    d = s.e("eglGetDisplay", 0)
    assert s.e("eglInitialize", d, 0, 0) == 1, "eglInitialize failed"
    assert s.e("eglBindAPI", 0x30A0) == 1
    cfg, n = s.out32(), s.out32()
    attrs = s.words(0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3033, 1, 0x3040, 4, 0x3038)
    assert s.e("eglChooseConfig", d, attrs, cfg, 1, n) == 1 and s.u32(n) == 1, "eglChooseConfig"
    cfg = s.u32(cfg)
    q = s.out32()
    info["config"] = {}
    for nm, at in (("EGL_BUFFER_SIZE", 0x3020), ("EGL_ALPHA_SIZE", 0x3021), ("EGL_BLUE_SIZE", 0x3022),
                   ("EGL_GREEN_SIZE", 0x3023), ("EGL_RED_SIZE", 0x3024), ("EGL_DEPTH_SIZE", 0x3025),
                   ("EGL_STENCIL_SIZE", 0x3026), ("EGL_SURFACE_TYPE", 0x3033), ("EGL_RENDERABLE_TYPE", 0x3040),
                   ("EGL_CONFIG_ID", 0x3028)):
        s.e("eglGetConfigAttrib", d, cfg, at, q)
        info["config"][nm] = s.u32(q)
    surf = s.e("eglCreatePbufferSurface", d, cfg, s.words(0x3057, width, 0x3056, height, 0x3038))
    assert surf, "eglCreatePbufferSurface: error 0x%x" % s.e("eglGetError")
    ctx = s.e("eglCreateContext", d, cfg, 0, s.words(0x3098, 2, 0x3038))
    assert ctx, "eglCreateContext: error 0x%x" % s.e("eglGetError")
    assert s.e("eglMakeCurrent", d, surf, surf, ctx) == 1, "eglMakeCurrent: error 0x%x" % s.e("eglGetError")
    info.update(display=d, surface=surf, context=ctx)
    s.egl_info = {"config": info["config"], "pbuffer": [width, height]}
    return info


def build_program(s, vs, fs, attribs):
    prog = s.g("glCreateProgram")
    for kind, src in ((0x8B31, vs), (0x8B30, fs)):
        sh = s.g("glCreateShader", kind)
        s.g("glShaderSource", sh, 1, s.words(s.cstr(src)), 0)
        s.g("glCompileShader", sh)
        ok = s.out32()
        s.g("glGetShaderiv", sh, 0x8B81, ok)
        if s.u32(ok) != 1:
            buf, ln = s.arena.alloc(1024, 4), s.out32()
            s.g("glGetShaderInfoLog", sh, 1024, ln, buf)
            raise RuntimeError("shader compile failed: %r (glGetError 0x%x)" % (s.proc.read(buf, s.u32(ln)), s.g("glGetError")))
        s.g("glAttachShader", prog, sh)
    for i, nm in enumerate(attribs):
        s.g("glBindAttribLocation", prog, i, s.cstr(nm))
    s.g("glLinkProgram", prog)
    ok = s.out32()
    s.g("glGetProgramiv", prog, 0x8B82, ok)
    assert s.u32(ok) == 1, "program link failed"
    s.g("glUseProgram", prog)
    return prog


TRI_VERTS = [(-0.8, -0.8, 1.0, 0.0, 0.0, 1.0), (0.8, -0.8, 0.0, 1.0, 0.0, 1.0), (0.0, 0.8, 0.0, 0.0, 1.0, 1.0)]
TRI_TWO = TRI_VERTS + [(-0.8, 0.8, 1.0, 1.0, 0.0, 1.0), (-0.4, 0.8, 1.0, 1.0, 0.0, 1.0), (-0.8, 0.4, 1.0, 1.0, 0.0, 1.0)]
TRI_MOVED = [(-0.6, -0.8, 1.0, 0.0, 0.0, 1.0), (0.8, -0.6, 0.0, 1.0, 0.0, 1.0), (0.2, 0.8, 0.0, 0.0, 1.0, 1.0)]


def tri_scene(s, verts=TRI_VERTS, clear=(0.25, 0.5, 0.75, 1.0), blend=False, indexed=False, frames=1, swap=False, readback=False,
              read_each=False):
    egl = setup_egl(s)
    build_program(s, VS, FS, ["p", "c"])
    vbo = s.out32()
    s.g("glGenBuffers", 1, vbo)
    s.g("glBindBuffer", 0x8892, s.u32(vbo))
    flat = [x for v in verts for x in v]
    s.g("glBufferData", 0x8892, 4 * len(flat), s.floats(*flat), 0x88E4)
    if indexed:
        ibo = s.out32()
        s.g("glGenBuffers", 1, ibo)
        s.g("glBindBuffer", 0x8893, s.u32(ibo))
        s.g("glBufferData", 0x8893, 6, s.put(struct.pack("<3H", 0, 1, 2)), 0x88E4)
    s.g("glEnableVertexAttribArray", 0)
    s.g("glEnableVertexAttribArray", 1)
    s.g("glVertexAttribPointer", 0, 2, 0x1406, 0, 24, 0)
    s.g("glVertexAttribPointer", 1, 4, 0x1406, 0, 24, 8)
    s.g("glViewport", 0, 0, 320, 320)
    if blend:
        s.g("glEnable", 0x0BE2)
        s.g("glBlendFunc", 1, 0x0303)
    for _ in range(frames):
        s.g("glClearColor", *[f32(c) for c in clear])
        s.g("glClear", 0x4000)
        if indexed:
            s.g("glDrawElements", 4, 3, 0x1403, 0)
        else:
            s.g("glDrawArrays", 4, 0, len(verts))
        s.g("glFinish")
        if read_each:
            s.g("glReadPixels", 0, 0, 1, 1, 0x1908, 0x1401, s.arena.alloc(16, 4))
    if readback:
        s.g("glReadPixels", 0, 0, 1, 1, 0x1908, 0x1401, s.arena.alloc(16, 4))
    if swap:
        r = s.e("eglSwapBuffers", egl["display"], egl["surface"])
        assert r == 1, "eglSwapBuffers failed: error 0x%x" % s.e("eglGetError")


def quad_tex_scene(s):
    setup_egl(s)
    build_program(s, VS_TEX, FS_TEX, ["p", "t"])
    verts = [(-0.8, -0.8, 0.0, 0.0), (0.8, -0.8, 1.0, 0.0), (-0.8, 0.8, 0.0, 1.0),
             (-0.8, 0.8, 0.0, 1.0), (0.8, -0.8, 1.0, 0.0), (0.8, 0.8, 1.0, 1.0)]
    vbo = s.out32()
    s.g("glGenBuffers", 1, vbo)
    s.g("glBindBuffer", 0x8892, s.u32(vbo))
    flat = [x for v in verts for x in v]
    s.g("glBufferData", 0x8892, 4 * len(flat), s.floats(*flat), 0x88E4)
    s.g("glEnableVertexAttribArray", 0)
    s.g("glEnableVertexAttribArray", 1)
    s.g("glVertexAttribPointer", 0, 2, 0x1406, 0, 16, 0)
    s.g("glVertexAttribPointer", 1, 2, 0x1406, 0, 16, 8)
    tex = s.out32()
    s.g("glGenTextures", 1, tex)
    s.g("glActiveTexture", 0x84C0)
    s.g("glBindTexture", 0x0DE1, s.u32(tex))
    pix = bytearray()
    for y in range(32):
        for x in range(32):
            pix += bytes([(x * 8) & 255, (y * 8) & 255, ((x + y) * 4) & 255, 255 if (x // 4 + y // 4) % 2 else 128])
    s.g("glPixelStorei", 0x0CF5, 1)
    s.g("glTexImage2D", 0x0DE1, 0, 0x1908, 32, 32, 0, 0x1908, 0x1401, s.put(bytes(pix)))
    s.g("glTexParameteri", 0x0DE1, 0x2801, 0x2600)
    s.g("glTexParameteri", 0x0DE1, 0x2800, 0x2600)
    s.g("glTexParameteri", 0x0DE1, 0x2802, 0x812F)
    s.g("glTexParameteri", 0x0DE1, 0x2803, 0x812F)
    s.g("glViewport", 0, 0, 320, 320)
    s.g("glEnable", 0x0BE2)
    s.g("glBlendFunc", 1, 0x0303)
    s.g("glClearColor", f32(0.25), f32(0.5), f32(0.75), f32(1.0))
    s.g("glClear", 0x4000)
    s.g("glDrawArrays", 4, 0, 6)
    s.g("glFinish")


SCENARIOS = {
    "tri": lambda s: tri_scene(s),
    "tri2": lambda s: tri_scene(s, frames=2),
    "tri_moved": lambda s: tri_scene(s, verts=TRI_MOVED),
    "tri_color": lambda s: tri_scene(s, clear=(0.75, 0.25, 0.5, 1.0)),
    "tri_blend": lambda s: tri_scene(s, blend=True),
    "tri_indexed": lambda s: tri_scene(s, indexed=True),
    "tri_swap": lambda s: tri_scene(s, swap=True),
    "tri_read": lambda s: tri_scene(s, readback=True),
    "tri_read2": lambda s: tri_scene(s, frames=2, read_each=True),
    "tri_two": lambda s: tri_scene(s, verts=TRI_TWO, readback=True),
    "quad_tex": quad_tex_scene,
}


def sha256(path):
    import hashlib
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


FAKES = [
    "Kernel: pvrsrvkm is a Python model (pvrkm.py). Bridge structures follow the vendor kernel headers; "
    "allocation placement follows srvinit_emu.place_allocation continued from the init recording.",
    "Hardware: nothing executes on the GPU. complete_kick applies, after the pre-kick snapshot, the writes "
    "the microkernel/3D core make on completion: every TA/3D status pair is stored at its address, every "
    "sync object's complete counters are set to its pending counters, the render-details and sync-list "
    "access resources are cleared on the last kick of a scene, and the TA CCB read offset is set to the "
    "write offset. Other state the microkernel and hardware would update (HWRTDATA flags, PB list heads, "
    "dataset pending counter, region headers, tail pointers, rendered pixels) is left as written by user space.",
    "The HW render context's page-directory word is a fake physical address (0x8F100000 >> 12).",
    "Time is a counter that advances 50 us per read; getpid/gettid return 1000.",
    "Window system: libpvrANDROID_WSEGL.so is loaded, but WSEGL_GetFunctionTablePointer is intercepted and "
    "returns a harness table (pbuffer-only; capability list copied from the vendor library; one configuration).",
    "gralloc: hw_get_module is intercepted and returns a harness module; open(\"gpu0\") returns a device whose "
    "other entry points abort; the transfer-context query returns a zeroed block.",
    "Unicorn 2.1.4 crashes on 64-bit D-register VSHL/VQADD/VQSUB; those instructions are executed in Python "
    "(armproc.NeonFix).",
]


def never_written(outdir, filldir, kicks):
    """Byte ranges of each allocation that differ between a run whose fresh device memory is
    zero and one filled with 0xA5, in the pre-kick image of the last kick: user space never wrote
    them (their real content is whatever the kernel's page allocator returned)."""
    from srvinit_emu import diff_ranges
    res = {}
    last = "kick%02d_pre" % (kicks - 1)
    for f in sorted(os.listdir(os.path.join(outdir, last))):
        a = open(os.path.join(outdir, last, f), "rb").read()
        b = open(os.path.join(filldir, last, f), "rb").read()
        if a != b:
            r = diff_ranges(a, b)
            res[int(f[6:9])] = {"count": len(r), "bytes": sum(n for _, n in r), "first_ranges": r[:32]}
    return res


SHADER_ROLES = {
    (5, 32768): "vertex USSE code chunk: compiled vertex shader with the generated attribute-unpack prologue/epilogue",
    (4, 32768): "fragment USSE code chunk: compiled fragment shader with the generated output/blend epilogue",
    (6, 32768): "pixel PDS code chunk: pixel PDS program(s) (code + data)",
    (4, 1024): "surface USSE_FRAG buffer (CBUF8): end-of-tile / tile-store USSE program",
    (5, 16): "terminate vertex USSE block",
    (7, 51200): "PDS vertex ring (CBUF3) or MTE-copy ring: vertex PDS program + data, MTE/ISP state",
    (6, 102400): "PDS fragment ring: pixel-event and pixel PDS programs + data",
    (7, 128): "terminate PDS block", (7, 64): "terminate PDS block", (7, 72): "PDS block",
}


def nonzero_regions(data, gap=64):
    out, i, n = [], 0, len(data)
    while i < n:
        if data[i:i + 4] != b"\0\0\0\0":
            last = k = i
            while k < n:
                if data[k:k + 4] != b"\0\0\0\0":
                    last = k + 4
                elif k - last >= gap:
                    break
                k += 4
            out.append((i, last - i))
            i = last
        else:
            i += 4
    return out


def extract_shaders(outdir, allocs, kicks):
    """Write the contents of the shader and PDS code areas as placed in GPU memory after the last
    kick: runs of non-zero data in the USSE and PDS heaps.  Roles are INFERRED from heap and size."""
    d = os.path.join(outdir, "shaders")
    os.makedirs(d, exist_ok=True)
    index = []
    for a in allocs:
        if a["heap_id"] not in (4, 5, 6, 7) or a["freed_at_bridge_call"] is not None:
            continue
        data = open(os.path.join(outdir, "kick%02d_post" % (kicks - 1), a["file"]), "rb").read()[:a["size"]]
        regs = nonzero_regions(data)
        if len(regs) > 16:
            regs = [(regs[0][0], regs[-1][0] + regs[-1][1] - regs[0][0])]
        for off, n in regs:
            fn = "alloc%03d_off%05x_size%05x.bin" % (a["index"], off, n)
            with open(os.path.join(d, fn), "wb") as f:
                f.write(data[off:off + n])
            index.append({"alloc": a["index"], "heap": a["heap_name"], "alloc_devva": a["devva"], "offset": off,
                          "size": n, "gpu_va": a["devva"] + off, "file": fn,
                          "role_inferred": SHADER_ROLES.get((a["heap_id"], a["size"]), "code/data area")})
    with open(os.path.join(d, "index.json"), "w") as f:
        json.dump(index, f, indent=1)
        f.write("\n")
    return index


def finish(s, name, outdir, unwritten=None):
    k = s.kernel
    gl_err = s.g("glGetError")
    egl_err = s.e("eglGetError")
    lib_hashes = {}
    for lib in ("lib/libsrv_um.so", "lib/libIMGegl.so", "lib/egl/libEGL_POWERVR_SGX530_125.so",
                "lib/egl/libGLESv2_POWERVR_SGX530_125.so", "lib/libglslcompiler.so", "lib/libusc.so",
                "lib/libpvrANDROID_WSEGL.so", "lib/libc.so", "lib/libm.so", "lib/libhardware.so", "bin/linker"):
        lib_hashes[lib] = sha256(os.path.join(SYSTEM, lib))
    allocs = []
    for a in k.allocs:
        if a.index < 0:
            continue
        allocs.append({
            "index": a.index, "heap_id": HEAPS[a.heap][0], "heap_name": HEAPS[a.heap][1],
            "size": a.size, "alignment": a.align, "request_attribs": a.flags,
            "returned_flags": a.client_meminfo_flags or a.flags, "devva": a.devva,
            "allocator": "kernel" if a.kernel else a.owner, "bridge_call": a.call,
            "tag": a.tag, "sync_alloc": a.sync.index if a.sync else None,
            "freed_at_bridge_call": a.freed_at, "file": "alloc_%03d.bin" % a.index,
            "never_written_by_user_space_at_last_kick": (unwritten or {}).get(a.index)})
    manifest = {
        "scenario": name,
        "result": {"returned_from_glFinish": True, "glGetError": gl_err, "eglGetError": egl_err},
        "egl": s.egl_info,
        "vendor_files_sha256": lib_hashes,
        "init_recording": {"dir": "../../init", "allocations": len(k.init["allocations"]),
                           "note": "init allocations are not copied into the snapshots; kernel allocations "
                                   "here continue each heap after the last init allocation"},
        "heap_table": k.init["heap_table"],
        "asInitDevData": k.init_dev_data,
        "allocation_policy": __import__("srvinit_emu").place_allocation.__doc__.strip(),
        "allocations": allocs,
        "bridge_calls": k.bridge_log,
        "kicks": k.kicks,
        "faked": FAKES,
    }
    manifest["shader_artifacts"] = "shaders/index.json (%d regions)" % len(extract_shaders(outdir, allocs, len(k.kicks)))
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    with open(os.path.join(outdir, "calls.txt"), "w") as f:
        f.write("\n".join(s.proc.calls) + "\n")
    for fd, data in s.proc.log_sink:
        with open(os.path.join(outdir, "log_output.txt"), "ab") as f:
            f.write(b"fd%d: " % fd + data + b"\n")
    return manifest


def run(name, outdir, shift=0, verbose=False, general_gap=0):
    import shutil
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir, exist_ok=True)
    s = Session(outdir, shift, verbose, general_gap=general_gap)
    SCENARIOS[name](s)
    import tempfile
    filldir = tempfile.mkdtemp(prefix="sgxfill")
    try:
        sf = Session(filldir, shift, False, fill=0xA5, general_gap=general_gap)
        SCENARIOS[name](sf)
        unwritten = never_written(outdir, filldir, len(s.kernel.kicks))
    finally:
        shutil.rmtree(filldir, ignore_errors=True)
    s.manifest = finish(s, name, outdir, unwritten)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenarios", nargs="+")
    ap.add_argument("--out", default=os.path.join(WORKTREE, "build", "sgx", "draw"))
    ap.add_argument("--shift", type=int, default=0)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--general-gap", type=lambda x: int(x, 0), default=0,
                    help="start the General heap this many bytes later (moves the render target); output NAME_gap")
    a = ap.parse_args()
    for name in a.scenarios:
        out = os.path.join(a.out, name + ("_gap" if a.general_gap else ""))
        s = run(name, out, a.shift, a.verbose, a.general_gap)
        print(name, "done:", len(s.kernel.allocs), "allocations,", len(s.kernel.kicks), "kicks")


if __name__ == "__main__":
    main()
