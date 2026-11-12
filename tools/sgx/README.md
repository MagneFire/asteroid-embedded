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
