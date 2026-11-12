# SGX530 init emulation (minnow)

`srvinit_emu.py` runs the vendor init server library `libsrv_init.so` (function
`SrvInit`) on the host under Unicorn and records the device memory image it
builds for the SGX530 microkernel. `check_init.py` compares the result with
`asteroid-embedded-notes/hw/minnow/sgx/init-facts.md`. Nothing from the vendor
blob is stored in the repository; both scripts read it at run time.

## Running

```
python3 -m venv minnow/sgx-study/venv
minnow/sgx-study/venv/bin/pip install unicorn pyelftools

BLOB=minnow/dump/extracted_data/system/lib/libsrv_init.so
minnow/sgx-study/venv/bin/python tools/sgx/srvinit_emu.py $BLOB            # -> build/sgx/init
minnow/sgx-study/venv/bin/python tools/sgx/srvinit_emu.py $BLOB OUTDIR --pd-addr 0x8F000000
minnow/sgx-study/venv/bin/python tools/sgx/check_init.py $BLOB             # exit status 1 on any FAIL
```

Paths are relative to `~/Downloads/AsteroidOS`. The output directory defaults to
`build/sgx/init` in the worktree (git-ignored). Two runs give identical files.

## Output

- `manifest.json`: allocations in order (index, heap, requested size, alignment, flags, device
  virtual address, file, call site), the heap table, the page-directory address, the decoded
  `DevInitPart2` struct (handles as allocation indices, host kick addresses, the four scripts as
  `{op, offset, value}`, build options, 13 struct sizes, EDM task registers, clock gating, cache
  control, `asInitDevData[24]`, `asInitMemHandles[18]`), app hints the library asked for, and
  which bytes depend on uninitialised memory or host pointers.
- `alloc_NN.bin`: contents of allocation NN when `SrvInit` returns (`size` bytes, the requested
  size, not the page-rounded size). The library frees nothing after `SGXDevInitPart2`, and no
  allocation changes between that call and the return, so both points give the same data.
- `kinit_info.bin`: the 2832-byte `SGX_BRIDGE_INIT_INFO` as libsrv_um would pass it to the kernel.
  Handles in it are fake values (`0xD0000000 + 0x100 * index`).
- `calls.txt`: every call to an import, in order, with decoded arguments, result and caller
  address inside `libsrv_init.so`.

## Entry point and client struct

`SrvInit` takes no arguments (`PVRSRV_ERROR SrvInit(void)`); `pvrsrvctl` calls it bare. It
connects, enumerates devices, and for the SGX device (class 3D, type 7) builds the image and calls
`SGXDevInitPart2(psDevData, psClientInit)`.

The second argument is a 0xAB0-byte client struct (malloc'd, not cleared), not the kernel's
`SGX_BRIDGE_INIT_INFO`. libsrv_um's `SGXDevInitPart2` (0x8944) converts it: handle fields
become `psMemInfo->hKernelMemInfo` (+0x20), the build options, struct sizes, kick addresses and
scripts are copied, and stencil buffer addresses come from `sDevVAddr` of two meminfos. The
mapping is `CLIENT_MAP` in `srvinit_emu.py`. Two client words (+0x4 and +0x10, CPU pointers of the
CCB and CCB control) are dropped by the conversion. The kernel input is
`{flags, hDevCookie, SGX_BRIDGE_INIT_INFO}`; `kinit_info.bin` holds the last part.

## Allocation policy

`place_allocation()` in `srvinit_emu.py`:

- Per heap, the first allocation is at the heap base.
- Each next allocation goes to the lowest address at or after `previous_address +
  previous_requested_size`, rounded up to 4 KiB and then to the requested alignment (minimum
  4 KiB, power of two).
- Freed space is never reused (the library frees nothing).
- The allocation fails if it ends past `heap base + heap size + 4 KiB` (the heap size excludes
  the guard page).

## What is stubbed

All 21 imports of `libsrv_init.so` are implemented in Python; an unlisted import aborts with its
name and call site, as does `PVRSRVDebugAssertFail`.

- Connection: `PVRSRVInitSrvConnect` returns a zeroed connection struct,
  `PVRSRVEnumerateDevices` returns one device (type SGX, class 3D, index 0),
  `PVRSRVAcquireDeviceData` returns a fake cookie (the library passes type 0, meaning
  match by index).
- `SGXGetInfoForSrvInit`: page-directory address (`--pd-addr`) and the 11-entry heap table from
  `structures.md` 1.5 in creation order, remaining entries `0xFFFFFFFF`. Heap attributes are those
  of `SGXRegisterDevice` (write-combine, RAM backed, single or multi process).
- `PVRSRVAllocDeviceMem`: fills a 56-byte `PVRSRV_CLIENT_MEM_INFO` (layout taken from libsrv_um's
  allocator) with a CPU buffer, device address, size and a fake kernel handle. Flag `0x08000000`
  (no user mapping) gives `pvLinAddr = NULL`. `pvLinAddrKM` is fake; the library only prints it.
  No sync info is ever created (all flags include `NO_SYNCOBJ`).
- App hints: `PVRSRVGetAppHint` returns "not set" and copies the library's default to the output,
  as libsrv_um does when no hint is found. The hints asked for are listed in the manifest.
- `PVRSRVMemCopy`, `PVRSRVMemSet`, `PVRSRVAllocUserModeMem` (bump allocator, `Free` is a no-op),
  `PVRSRVDebugPrintf` (formatted to stderr), `SGXDevInitPart2` (captures the client struct and
  returns success), `PVRSRVInitSrvDisconnect`, and no-op `__cxa_atexit`, `__register_atfork`,
  `__cxa_finalize`. The library has no `.init_array` or `DT_INIT`, so no constructors run.

## Dependence checks

The emulator runs the library three times: normally, with uninitialised memory (heap, stack and
device buffers) filled with `0xA5` instead of `0x00`, and with every emulator address and fake
handle moved. Bytes that differ in the second run are recorded as `never_written_ranges`
(they are zero in the `.bin` files but undefined on a real device); bytes that differ in the third
run would be `host_pointer_dependent_ranges` (none). The same comparisons are applied to the init
struct.

## Known gaps

- Allocation placement assumes the kernel allocates nothing else in the KernelData and KernelCode
  heaps before and during init, and that its allocator is sequential as described above. Neither
  is verified against a running driver.
- App hints are all defaults. The library reads them from a file via libsrv_um; no such file is in
  the dump. A hint file on the watch would change the scripts and the TA/3D control block.
- Three host kick entries (2D, data breakpoint, flush PD cache) and `ui32ClkGateCtl2` are never
  written by the library; the kernel ignores them on this core, and on the device they hold
  whatever the malloc'd client struct contained.
- The 0x00 fill is an assumption for the contents of `never_written_ranges` (CCB, CCB control,
  kicker, stencil buffers); the kernel clears the first three in `DevInitSGXPart2KM`.
- The fake `hKernelMemInfo` values and `pvLinAddrKM` are placeholders; the image does not depend
  on them.
- Only `SrvInit` is emulated. What the kernel does with the result, and the microkernel
  itself, is out of scope.

# GLES recordings (minnow)

`glrec.py` runs the vendor EGL and GLES2 libraries for the SGX530 on the host and records the GPU
memory and commands they produce for small GL programs. It needs the init recording
(`build/sgx/init`, made by `srvinit_emu.py`) because the fake kernel continues from the device state
the init left.

## Running

```
PY=minnow/sgx-study/venv/bin/python
$PY tools/sgx/glrec.py tri tri2 tri_moved tri_color tri_blend tri_indexed tri_read tri_read2 tri_two   # -> build/sgx/draw/<name>
$PY tools/sgx/glrec.py --general-gap 0x100000 tri_read                     # -> build/sgx/draw/tri_read_gap
$PY tools/sgx/check_draw.py tri tri2 tri_moved tri_color tri_blend tri_indexed tri_read
$PY tools/sgx/diffrec.py build/sgx/draw/tri build/sgx/draw/tri_moved                # same kick, two recordings
$PY tools/sgx/diffrec.py build/sgx/draw/tri2 build/sgx/draw/tri2 --kick 0 --kick-b 1
$PY tools/sgx/diffrec.py build/sgx/draw/tri --kick 0                                # pre against post
# textured scenes (they create the gralloc transfer context, see "Transfer path")
$PY tools/sgx/glrec.py tri_tq quad_tex quad_tex_read quad_tex100 quad_tex320 quad_tex320_read quad_tex_moved \
    quad_tex_draw2 quad_alpha quad_alpha2 quad_a8 quad_a8_x quad_a8_y quad_a8_z quad_linear
$PY tools/sgx/check_draw.py quad_tex quad_tex320 quad_alpha
$PY tools/sgx/diffrec.py build/sgx/draw/quad_tex build/sgx/draw/quad_tex320 --hide-tq   # GL context only
$PY tools/sgx/diffrec.py build/sgx/draw/quad_tex build/sgx/draw/quad_tex100 --xfer 0   # transfer 0 instead of kick 0
$PY tools/sgx/texprobe.py                                                          # texture size/layout table
```

Paths are relative to `~/Downloads/AsteroidOS`. Options of `glrec.py`: `--out DIR`, `--shift N`
(move guest addresses, kernel handles and the clock), `-v` (print every system call).
One scenario takes about one second.

Scenarios: `tri` (one cleared triangle from a VBO), `tri2` (the same, two frames), `tri_moved` (other
vertex positions), `tri_color` (other clear colour), `tri_blend` (GL_ONE, GL_ONE_MINUS_SRC_ALPHA),
`tri_indexed` (glDrawElements, 3 unsigned shorts), `tri_read` (`tri` plus glReadPixels, which makes
the end-of-scene kick), `tri_swap` (`tri` plus eglSwapBuffers; produces no extra kick on a
pbuffer), `tri_read2` (two frames, each ended by glReadPixels: two whole scenes, four kicks),
`tri_two` (`tri_read` with two triangles in one draw).
`--general-gap N` starts the General heap N bytes later, which moves the render target (its first
allocation) and every other General buffer; the output directory gets `_gap` appended.

Textured scenarios (320x320 pbuffer, clear, one quad of two triangles from a VBO with `p` and `t`
attributes, GL_ONE / GL_ONE_MINUS_SRC_ALPHA blending, nearest filtering, CLAMP_TO_EDGE, `glFinish`; they run
with the gralloc transfer context, see "Transfer path"). The vertex shader is
`u=t; gl_Position=vec4(p,0.,1.);`.

| Scenario | Texture | Fragment shader |
|---|---|---|
| `tri_tq` | none; `tri` with the transfer context created, the baseline for the rest | `tri`'s |
| `quad_tex` | 32x32 RGBA | `texture2D(s,u)` |
| `quad_tex_read` | as `quad_tex`, plus glReadPixels (end-of-scene kick) | |
| `quad_tex100` | 100x100 RGBA (not in the request: an NPOT size that is not a multiple of 32) | |
| `quad_tex320` | 320x320 RGBA (NPOT, the firmware's layer size) | |
| `quad_tex320_read` | as `quad_tex320`, plus glReadPixels | |
| `quad_tex_moved` | 320x320; after the first draw glTexImage2D uploads other contents into the same texture object and the quad is drawn again (the texture gets a new address) | |
| `quad_tex_obj` | as `quad_tex_moved`, but the second contents go into a second texture object (same result; not recorded by default) | |
| `quad_tex_draw2` | 320x320, second draw without any change (baseline for `quad_tex_moved`) | |
| `quad_alpha`, `quad_alpha2` | 320x320 | `texture2D(s,u)*a`, `uniform lowp float a` = 0.5 / 0.25 |
| `quad_a8` | 32x32 GL_ALPHA | `c*texture2D(s,u).a`, `uniform lowp vec4 c` = (0.25, 0.5, 0.75, 1) |
| `quad_a8_x`, `_y`, `_z` | as `quad_a8` | c = (0.75, 0.5, 0.25, 1), (0.25, 0.5, 0.75, 0.5), (1, 1, 1, 1): they show where c lives |
| `quad_linear` | 320x320, GL_LINEAR min and mag | `texture2D(s,u)` |

All of them end with glGetError 0 and eglGetError 0x3000 (EGL_SUCCESS).

## Output of one scenario (`build/sgx/draw/<name>`)

- `manifest.json`: result (glGetError, eglGetError), chosen EGL configuration, sha-256 of the vendor
  files used, the heap table and `asInitDevData` of the init recording, every GPU allocation in
  order (index, heap, size, alignment, requested attributes, flags the kernel returns, GPU address,
  allocator: the kernel, or the services call site plus the first library return address found on
  the stack, which is a heuristic; bridge call; sync object index; free time; bytes user space never
  wrote), every bridge call with its raw input and output and decoded fields for the ones the
  harness implements, every kick (all fields of the 0x298-byte `SGX_CCB_KICK`, the CCB offset, the
  1304-byte command with its 64-byte padding as hex, the completion writes the harness made, and
  the snapshot directories), and the list of fakes.
- `kickNN_pre/`, `kickNN_post/`: `alloc_III.bin`, the bytes of every live GPU allocation, named by
  allocation index. "pre" is taken after the kernel has written the shared header (as `sgxkick.c`
  does it, READY last) and before completion; "post" after the harness's completion step. Init
  allocations are not copied; `manifest.json` points at `../../init`. Kernel allocations in
  KernelData and CacheCoherent continue after the last init allocation of those heaps.
- Textured scenarios only: `xferNN_pre/`, `xferNN_post/` (the same snapshot as for a kick, taken at each
  SUBMITTRANSFER: "pre" after the kernel wrote the shared header, "post" after the fake completion),
  `manifest.json` keys `transfers` (all fields of the 84-byte `PVRSRV_TRANSFER_SGX_KICK`, the CCB offset,
  sync objects, the command words `{service address, 0, HW transfer context address}`, completion writes)
  and `hw_transfer_contexts`, and a `group: "transfer-context"` entry on every allocation that the
  transfer context's creation made (`diffrec.py --hide-tq` leaves them out). Transfers are not in `kicks`:
  kick numbers keep their meaning.
- `calls.txt`: every system call (with the call site), every bridge ioctl, every call into a harness
  function (`hw_get_module`, gralloc, WSEGL) in order.
- `shaders/`: runs of non-zero data in the USSE and PDS heaps after the last kick (one file per run)
  and `index.json` with allocation, offset, size, GPU address and an inferred role.
- `log_output.txt`: what the libraries wrote to the Android log, if anything.

`DIFFS.md` in `build/sgx/draw` lists what differs between the scenarios.

## Approach

The process is a real ARM Linux process image: Unicorn runs the dump's own `/system/bin/linker`, which
loads `libc.so`, `libm.so`, `libdl.so`, `libcutils.so`, `liblog.so`, `libc++.so`, `libcrypto.so`,
`libhardware.so`, `libsync.so`, `libsrv_um.so`, `libIMGegl.so`, `libusc.so`, the EGL and GLES2
libraries and (when GLES loads it) `libglslcompiler.so`, and runs their constructors. Only the
system calls are emulated (`armproc.py`): a file system rooted at the dump's `system/`, anonymous and
file mappings, `brk`, TLS through the CP15 user read-only thread register, signals as no-ops,
single-threaded `futex`, a counter clock, `/dev/pvrsrvkm` (`pvrkm.py`). Any system call or bridge
call that is not implemented stops the run with its number or name and the call site. This avoids
writing a Python stub per libc import; the compiler's import list is large, and bionic's own
code (malloc, stdio, math, locale) runs as on the watch. The cost is a wider emulated system-call
boundary. Nothing in the vendor libraries is replaced except the functions listed under "What is
faked" (two library entry points and one host-emulator workaround).

The vendor libraries are driven from Python by calling the exported entry points
(`eglGetDisplay`, `glDrawArrays`, ...) with arguments written into guest memory. The ABI is
softfp: floats travel in integer registers.

## Allocation policy

`srvinit_emu.place_allocation` is used unchanged: per heap, the first allocation is at the heap base,
later ones at the lowest address at or after the previous address plus the previous requested size,
rounded up to 4 KiB and to the requested alignment (minimum 4 KiB); freed space is not reused.
The state per heap starts where the init recording left it (KernelData after the second stencil
buffer, KernelCode after PDS program 5); other heaps start empty. All heaps are treated as
one address space, so a second memory context would collide (none is created). Sync data of an
allocation (40 bytes, CacheCoherent heap, alignment 4, cleared) is allocated right after the allocation
itself, as `PVRSRVAllocSyncInfoKM` does after `AllocDeviceMem`. The HW render context (64 bytes,
KernelData, alignment 32) is allocated at REGISTER_HW_RENDER_CONTEXT. Sizes returned in the client
mem info are the requested sizes.

## What is faked

- The kernel: Python model of the bridge calls the libraries issue (connect, device enumeration,
  memory context and heap info, device memory alloc/free, mmap data, misc info, global event object,
  SGX client/internal/misc info, HW render context, DOKICK). It follows the structures of
  `pvr_bridge.h`, `sgx_bridge.h`, `sgxinfo.h`, `sgx_mkif_km.h`. Values it has to invent: the
  page-directory physical address in the HW render context (0x8F100000 >> 12), the timer-register
  page (zeros), `SGX_MISC_INFO_REQUEST_DRIVER_SGXREV` (DDK 1.12 build `(2917986 << 8) | 4`, build
  options 0x0016801C).
- Completion: after the pre-kick snapshot, every TA and 3D status pair is stored at its address, all
  sync objects' complete counters are set to their pending counters, on the last kick of a scene the
  render-details and sync-list access resources are cleared, and the TA CCB read offset is set to the
  write offset. Event waits return at once. Nothing else the hardware or microkernel would update
  is written (HWRTDATA flags, PB heads, region headers, tail pointers, the dataset pending counter,
  the kernel CCB, pixels).
- Window system: `libpvrANDROID_WSEGL.so` is loaded, and its `WSEGL_GetFunctionTablePointer` is
  intercepted (`glenv.py`) to return a harness table for pbuffers only: display validation,
  display initialisation and close are implemented, every drawable callback aborts. The vendor
  library's own initialisation needs PowerVR extension slots of the gralloc module (+0xb8 format list,
  +0xec preferred format). The capability list is copied from the vendor library; one configuration
  with pixel-format value 2 is returned, which gives the RGBA8888 GLES2 configurations.
- gralloc: `hw_get_module` is intercepted and returns a harness module whose `open("gpu0")`
  succeeds; the transfer-context query (+0x10c) stores NULL (untextured scenarios) or a real transfer
  context (textured ones, see "Transfer path"). Every other slot aborts.
- Unicorn 2.1.4 on Apple silicon crashes on three NEON forms (64-bit D-register `vshl`, `vqadd`,
  `vqsub`). `armproc.NeonFix` finds them by pattern in executable file mappings and executes them
  in Python through a code hook on each address. No guest instruction is changed.
- Determinism: the clock advances 50 us per read, `getpid` is 1000, random bytes (`AT_RANDOM`) are
  fixed. `check_draw.py` runs every scenario twice (all files identical), and once with guest
  addresses, kernel handles and the clock shifted (all GPU images and allocations identical).
  Fresh device memory is zero; `never_written_by_user_space_at_last_kick` in the manifest lists the
  ranges that differ when it is filled with 0xA5 instead.

## Transfer path (textures)

glTexImage2D does not write the texture with the CPU. The GLES driver copies the pixels (tightly packed
rows) into a staging buffer that belongs to a *transfer context* and queues one blit per 0x20000 staging
bytes with `SGXQueueTransfer`; the blit (a 3D-core job that samples the staging buffer as a strided texture and
renders into the texture in its final layout) copies them into the texture's memory. The transfer context is
not the GL driver's: EGL gets it from the Android gralloc module (`gralloc_module+0x10c`, called from EGL's
services init), and `gralloc.omap3.so` creates it at load with libsrv_um's `SGXCreateTransferContext`.
A NULL context makes `SGXQueueTransfer` abort ("invalid parameters"), a zeroed block makes it use a null mutex.

What the lab does, for scenarios named `quad_*` and `tri_tq` (`glrec.TRANSFER_SCENARIOS`):

- The synthetic gralloc's +0x10c callback does, on first call and with libsrv_um's own functions called from
  the callback (`glenv.call_nested`: a nested emulator call with registers and stack saved), what
  `gralloc.omap3.so` does around its call (read from the watch's module): `PVRSRVConnect` (a second
  connection), `PVRSRVEnumerateDevices`, `PVRSRVAcquireDeviceData`, `PVRSRVCreateDeviceMemContext`,
  `SGXGetClientInfo`, `PVRSRVGetMiscInfo` (state request 8, the global event object), then
  `SGXCreateTransferContext(&devdata, 0x20000, {hDevMemContext, hOSEvent}, &context)`. The returned pointer
  is stored for EGL. The module's other work (PVR2D device context, framebuffer device) is not done.
  It builds, in the lab: the transfer CCB (66740 bytes, KernelData), the 128 KiB staging buffer plus
  4 bytes (General, 0x1000), a 124 KiB ISP/vertex area (General), the blit USSE programs (about 60 small
  allocations and one 16 KiB block in PixelShaderUSSE), the blit PDS programs, status words and sync
  objects: 67 allocations, 352,900 bytes, and one `SGX_REGISTER_HW_TRANSFER_CONTEXT`. They are created
  before the GL context, so the GL context's allocations start later in General (+0x40000), PixelShaderUSSE
  (+0x3a000) and PDSPixelCodeData (+0x8000) than in `tri`; `tri_tq` is `tri` under the same conditions.
- The kernel gives a process one memory context (`BM_CreateContext` finds the process's existing one and
  takes a reference), so the second connection's `CreateDeviceMemContext` returns the same context; the
  fake does the same and the transfer context's page-directory word is the render context's fake.
- Fake kernel, `pvrkm.py`: `REGISTER_HW_TRANSFER_CONTEXT` (111) allocates the 228-byte context in KernelData
  (alignment 32, read|write|no sync|EDM|cache consistent), copies the user image and patches the page-directory
  address at the given offset as `SGXRegisterHWTransferContextKM` does; `UNREGISTER` (112) succeeds;
  `SUBMITTRANSFER` (98) writes the shared header as `SGXSubmitTransferKM` does (TA/3D sync snapshot, source
  and destination sync snapshots with duplicates removed unless KEEPPENDING, take read/write ops, counts), snapshots
  `xferNN_pre`, completes and snapshots `xferNN_post`. The bridge numbering in `BRIDGE_NAMES` had 111 as
  `SCHEDULE_PROCESS_QUEUES`; the kernel header has it at 113 (nothing called it).
- Completion of a transfer stores the status values the library placed in the command (`ui32NumStatusVals`
  at shared header +0xe8, entries at +0xec) and sets every sync object's complete counters to its pending
  counters. It ignores `NOSYNCUPDATE`, does not execute the blit (texture memory stays zero; the pixels are
  visible only in the staging buffer) and does not advance the transfer CCB's read offset (its place in the HW
  transfer context is not defined by the kernel headers; one transfer uses 0x1c0 bytes of the 66740-byte CCB
  and no scenario comes near filling it). What the GL driver reads back is only the sync counters: an upload
  of more than 0x20000 bytes is sent in chunks through the one staging buffer, and the driver waits (event
  waits) for a chunk to complete before it refills the buffer; with completion deferred, `quad_tex320` stops at
  that wait (100 event waits, then the driver's timeout diagnostics call gralloc +0xd8), while a single-chunk
  upload (`quad_tex`) draws and kicks unchanged. Texture pixels are never read, so the missing blit changes no
  recorded word. The TA kick has no source sync for the texture and no TQ sync in either case.
- `check_draw.py` also compares transfers. For the textured scenarios it notes (does not fail on) words
  in the PDS programs that differ between the normal and the shifted run by exactly the shift: CPU
  pointers (a heap address at PDS vertex ring +0x144 in `quad_tex*`, +0x134 in the PDS fragment ring in `quad_alpha*`, a
  library address at +0x120/+0x124 in `quad_a8`) the driver copied into the programs; they are
  not GPU addresses and differ on a real device from run to run.

## Known gaps

- `glFinish` on a pbuffer submits the TA work but does not end the scene (`bLastInScene` 0); the
  3D-start kick appears only with glReadPixels (`tri_read`). No eglSwapBuffers kick exists for
  pbuffers. Scene termination with preserved background, PB growth and out-of-space are not
  recorded.
- The fake completion does not model the microkernel. Values written by it are listed under
  `completion` in each kick; do not treat them as device behaviour.
- FBO paths, instanced draws, mipmaps, texture formats other than RGBA8888 and ALPHA8, wrap modes
  other than CLAMP_TO_EDGE and a target-address-only change are not recorded. Texture memory is never
  filled (the blit is not executed): the layout of the pixels in memory is read from the allocation
  size, the texture state and the transfer's destination description, not from data.
- The transfer context is created through the CPU-side library calls of `gralloc.omap3.so` only as far as
  the transfer context needs; the module's remaining initialisation (PVR2D, framebuffer) is not done.
- The allocation call sites in the manifest are the services return address (exact) and the nearest
  plausible library return address on the stack (heuristic).
- `check_init.py` reports one FAIL (alloc #6, microkernel trace buffer: it expects alignment 0x1000,
  `init-facts.md` and the emulation give 16). This is independent of the GLES tools.

# Native scene data (minnow)

`mkreplay.py` writes `src/gpu/sgx_scene_data.[ch]`, what `src/gpu/sgx_draw.c` replays: the GL
context's allocation table (heap, size, alignment, flags, recorded GPU address, and whether the kernel
makes it: sync data, HW render context), the memory image of `tri_read` at its first kick as runs of
non-zero words less every word the native code writes itself, the TA command template, the 3D
register block of the render details, the end-of-scene stream, the blend variants, the status lists
of both kicks and the addresses of every per-frame site. It finds the sites by comparing recordings
that differ in one input and stops when a comparison shows more or less than it expects.

```
$PY tools/sgx/glrec.py tri tri_moved tri_color tri_blend tri_read tri_read2 tri_two
$PY tools/sgx/glrec.py --general-gap 0x100000 tri_read
python3 tools/sgx/mkreplay.py
python3 tools/sgx/flatten.py draw          # flat.txt per recording, for tests/sgx_draw_test.c
SGX_LIBSRV_INIT=minnow/dump/extracted_data/system/lib/libsrv_init.so make test
```

The notes are `asteroid-embedded-notes/hw/minnow/sgx/native-scene.md`.
