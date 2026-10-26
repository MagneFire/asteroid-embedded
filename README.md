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

    make run            # adb reboot, fastboot boot, show the log over adb
    make test           # host tests for the adb and console code

The running firmware is an adb device: `adb shell` gives the command
console (`help` lists the commands), `adb push`/`adb pull` move files to
and from a RAM file table, `adb reboot` returns to fastboot. The firmware
keeps its log and a fault record in DRAM across a warm reset; when an image
dies, `tools/run.py` fetches them through `fastboot oem ramoops raw` and
symbolizes the addresses in the fault record with `addr2line`.
