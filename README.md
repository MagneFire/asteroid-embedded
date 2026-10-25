# asteroid-embedded

Bare-metal firmware for Qualcomm APQ8026 smartwatches (Huawei Watch
"sturgeon", ASUS ZenWatch 2 "sparrow"): no Linux kernel, no root filesystem,
one image booted over fastboot. The bootloader leaves the panel on and the
chip configured; the firmware takes it from there.

## Build

Needs `arm-none-eabi-gcc`, `dtc` and Python 3.

    make                # build/asteroid.img for sturgeon
    make WATCH=sparrow
    make TEST=hang      # a build that stalls its main loop (watchdog test)
    make TEST=abort     # a build that takes a data abort

## Run

The watch must sit in fastboot (lk2nd built with `LK2ND_FORCE_FASTBOOT=1`
and `SCRATCH_SIZE=0x0F000000` flashed to `boot`).

    make run            # fastboot boot, wait for the reboot, show the log

The firmware keeps its log and a fault record in DRAM across the warm reset;
`tools/run.py` fetches them through `fastboot oem ramoops raw` and
symbolizes the addresses in a fault record with `addr2line`.
